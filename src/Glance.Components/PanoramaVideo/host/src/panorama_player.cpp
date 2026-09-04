#include "../include/panorama_player.h"

#include "glance/contracts/native_preview_protocol.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windowsx.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace glance::contracts::native_preview;
    using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
    using winrt::Windows::Media::MediaTimelineController;
    using winrt::Windows::Media::MediaTimelineControllerState;
    using winrt::Windows::Media::Playback::MediaPlaybackItem;
    using winrt::Windows::Media::Playback::MediaPlayer;
    using winrt::Windows::Storage::Streams::IRandomAccessStream;

    constexpr UINT request_message = WM_APP + 1;
    constexpr UINT frame_message = WM_APP + 2;
    constexpr UINT media_opened_message = WM_APP + 3;
    constexpr UINT media_failed_message = WM_APP + 4;
    constexpr UINT tracks_changed_message = WM_APP + 5;
    constexpr UINT software_frame_message = WM_APP + 6;
    constexpr UINT software_failed_message = WM_APP + 7;
    constexpr wchar_t player_window_class[] = L"Glance.PanoramaVideoHost";
    constexpr float minimum_fov = 30.0F;
    constexpr float maximum_fov = 120.0F;
    constexpr float default_fov = 90.0F;
    constexpr float insta360_fisheye_fov = 193.0F;
    constexpr float insta360_blend_overlap = 6.0F;
    constexpr float dji_fisheye_fov = 193.0F;
    constexpr float dji_blend_overlap = 6.0F;
    constexpr float minimum_lens_fisheye_fov = 180.0F;
    constexpr float maximum_lens_fisheye_fov = 220.0F;
    constexpr float minimum_lens_blend_overlap = 0.0F;
    constexpr float maximum_lens_blend_overlap = 20.0F;
    constexpr float insta360_initial_yaw = -1.57079633F;
    constexpr std::uint32_t software_frame_size = 1024;
    constexpr std::uint32_t failure_controller = 0x101;
    constexpr std::uint32_t failure_open_timeout = 0x102;
    constexpr std::uint32_t failure_resize = 0x103;
    constexpr std::uint32_t failure_track_selection = 0x104;
    constexpr std::uint32_t failure_media_opened = 0x106;
    constexpr std::uint32_t failure_frame_copy = 0x107;
    constexpr std::uint32_t failure_render = 0x108;
    constexpr std::uint32_t failure_open_async = 0x109;
    constexpr std::uint32_t failure_media_player = 0x200;

    constexpr char vertex_shader_source[] = R"(
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";

    constexpr char pixel_shader_source[] = R"(
Texture2D frame0 : register(t0);
Texture2D frame1 : register(t1);
SamplerState frameSampler : register(s0);

cbuffer ViewParameters : register(b0)
{
    float aspect;
    float fovRadians;
    float yaw;
    float pitch;
    float sourceMode;
    float fisheyeFovRadians;
    float blendOverlap;
    float padding;
};

struct PixelInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 rotateView(float3 direction)
{
    float cp = cos(pitch);
    float sp = sin(pitch);
    direction = float3(
        direction.x,
        direction.y * cp - direction.z * sp,
        direction.y * sp + direction.z * cp);
    float cy = cos(yaw);
    float sy = sin(yaw);
    return float3(
        direction.x * cy + direction.z * sy,
        direction.y,
        -direction.x * sy + direction.z * cy);
}

float4 sampleFisheye(
    Texture2D source,
    float3 direction,
    float2 scale,
    float2 offset,
    out float valid)
{
    float theta = acos(clamp(direction.z, -1.0, 1.0));
    float radius = theta / fisheyeFovRadians;
    valid = radius <= 0.5 ? 1.0 : 0.0;
    float angle = atan2(direction.y, direction.x);
    float2 uv = offset + scale * float2(
        0.5 + radius * cos(angle),
        0.5 - radius * sin(angle));
    return source.Sample(frameSampler, uv);
}

    float4 main(PixelInput input) : SV_Target
    {
    if (sourceMode > 1.5 && sourceMode < 2.5)
        {
            float2 uv = input.uv;
            const float contentAspect = 2.0;
            if (aspect > contentAspect)
            {
                float width = contentAspect / aspect;
                uv.x = (uv.x - (1.0 - width) * 0.5) / width;
            }
            else
            {
                float height = aspect / contentAspect;
                uv.y = (uv.y - (1.0 - height) * 0.5) / height;
            }
            if (any(uv < 0.0) || any(uv > 1.0))
            {
                return float4(0.0, 0.0, 0.0, 1.0);
            }
            return uv.x < 0.5
                ? frame0.Sample(frameSampler, float2(uv.x * 2.0, uv.y))
                : frame1.Sample(frameSampler, float2((uv.x - 0.5) * 2.0, uv.y));
        }

        float tangent = tan(fovRadians * 0.5);
    float2 screen = float2(
        (input.uv.x * 2.0 - 1.0) * aspect,
        1.0 - input.uv.y * 2.0);
    float3 direction = normalize(float3(screen * tangent, 1.0));
    direction = rotateView(direction);

    float frontValid;
    float rearValid;
    float3 rearDirection = float3(-direction.x, direction.y, -direction.z);
    float4 front;
    float4 rear;
    if (sourceMode > 2.5)
    {
        front = sampleFisheye(
            frame0,
            direction,
            float2(0.5, 1.0),
            float2(0.0, 0.0),
            frontValid);
        rear = sampleFisheye(
            frame0,
            rearDirection,
            float2(0.5, 1.0),
            float2(0.5, 0.0),
            rearValid);
    }
    else
    {
        front = sampleFisheye(
            frame0,
            direction,
            float2(1.0, 1.0),
            float2(0.0, 0.0),
            frontValid);
        rear = sampleFisheye(
            frame1,
            rearDirection,
            float2(1.0, 1.0),
            float2(0.0, 0.0),
            rearValid);
    }
    if (frontValid == 0.0)
    {
        return rearValid == 0.0 ? float4(0.0, 0.0, 0.0, 1.0) : rear;
    }
    if (rearValid == 0.0)
    {
        return front;
    }
    float frontWeight = smoothstep(-blendOverlap, blendOverlap, direction.z);
    return lerp(rear, front, frontWeight);
}
)";

    bool read_exact(HANDLE handle, void* destination, std::size_t size) noexcept
    {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size != 0)
        {
            DWORD read{};
            const auto request = static_cast<DWORD>(
                std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(handle, bytes, request, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE handle, const void* source, std::size_t size) noexcept
    {
        const auto* bytes = static_cast<const std::byte*>(source);
        while (size != 0)
        {
            DWORD written{};
            const auto request = static_cast<DWORD>(
                std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(handle, bytes, request, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    std::filesystem::path find_proxy(const std::filesystem::path& source) noexcept
    {
        if (_wcsicmp(source.extension().c_str(), L".insv") != 0)
        {
            return {};
        }
        auto stem = source.stem().wstring();
        if (stem.size() < 5 || _wcsnicmp(stem.c_str(), L"VID_", 4) != 0)
        {
            return {};
        }
        stem.replace(0, 4, L"LRV_");
        const auto camera = stem.rfind(L"_00_");
        if (camera == std::wstring::npos)
        {
            return {};
        }
        stem.replace(camera, 4, L"_01_");
        const auto proxy = source.parent_path() / (stem + L".lrv");
        std::error_code error;
        return std::filesystem::is_regular_file(proxy, error) && !error
            ? proxy
            : std::filesystem::path{};
    }

    struct ShaderParameters
    {
        float aspect{ 1.0F };
        float fov_radians{};
        float yaw{};
        float pitch{};
        float source_mode{};
        float fisheye_fov_radians{};
        float blend_overlap{};
        float padding{};
    };

    struct FrameResource
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        IDirect3DSurface surface{ nullptr };
        std::uint32_t width{};
        std::uint32_t height{};
        bool valid{};

        void reset() noexcept
        {
            valid = false;
            width = 0;
            height = 0;
            surface = nullptr;
            view = nullptr;
            texture = nullptr;
        }
    };

    struct SoftwareFrame
    {
        std::vector<std::byte> pixels;
        std::int64_t timestamp{};
    };

    class PanoramaSession final
    {
    public:
        Status open(
            const std::wstring& path,
            HWND parent,
            const RECT& bounds,
            const PreviewVisuals& visuals,
            HANDLE cancellation_event)
        {
            unload();
            std::error_code error;
            if (parent == nullptr || !IsWindow(parent) ||
                !std::filesystem::path(path).is_absolute() ||
                !std::filesystem::is_regular_file(path, error) || error ||
                WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0)
            {
                return Status::invalid_request;
            }
            visuals_ = visuals;
            bounds_ = bounds;
            cancellation_event_ = cancellation_event;
            source_path_ = path;
            proxy_path_ = find_proxy(source_path_);
            proxy_mode_ = !proxy_path_.empty();
            const bool insta360 = _wcsicmp(
                source_path_.extension().c_str(),
                L".insv") == 0;
            lens_fisheye_fov_ = insta360
                ? insta360_fisheye_fov
                : dji_fisheye_fov;
            lens_blend_overlap_ = insta360
                ? insta360_blend_overlap
                : dji_blend_overlap;
            open_started_tick_ = GetTickCount64();
            projected_view_ = true;
            fov_ = default_fov;
            yaw_ = _wcsicmp(source_path_.extension().c_str(), L".insv") == 0
                ? insta360_initial_yaw
                : 0.0F;
            pitch_ = 0.0F;
            if (!initialize_graphics(parent, bounds))
            {
                unload();
                return Status::decoder_unavailable;
            }
            const auto generation = ++generation_;
            open_media_async(
                proxy_mode_ ? proxy_path_.wstring() : source_path_.wstring(),
                generation,
                proxy_mode_);
            return Status::success;
        }

        Status resize(const RECT& bounds)
        {
            bounds_ = bounds;
            if (window_ == nullptr)
            {
                return Status::invalid_request;
            }
            const int width = std::max(0L, bounds.right - bounds.left);
            const int height = std::max(0L, bounds.bottom - bounds.top);
            SetWindowPos(
                window_,
                nullptr,
                bounds.left,
                bounds.top,
                width,
                height,
                SWP_NOACTIVATE | SWP_NOZORDER);
            resize_swap_chain(
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height));
            return Status::success;
        }

        Status set_visuals(const PreviewVisuals& visuals)
        {
            visuals_ = visuals;
            render();
            return Status::success;
        }

        Status play()
        {
            desired_playing_ = true;
            if (ready_ && controller_ != nullptr)
            {
                try
                {
                    if (controller_started_)
                    {
                        controller_.Resume();
                    }
                    else
                    {
                        controller_.Start();
                        controller_started_ = true;
                    }
                }
                catch (...)
                {
                    record_failure(failure_controller, winrt::to_hresult().value);
                    return Status::media_failed;
                }
            }
            sync_software_clock();
            return Status::success;
        }

        Status pause()
        {
            desired_playing_ = false;
            if (controller_ != nullptr)
            {
                try
                {
                    controller_.Pause();
                }
                catch (...)
                {
                    return Status::media_failed;
                }
            }
            sync_software_clock();
            return Status::success;
        }

        Status seek(std::int64_t position)
        {
            resume_position_ticks_ = std::max<std::int64_t>(0, position);
            if (controller_ == nullptr)
            {
                return Status::success;
            }
            try
            {
                const auto value = std::clamp<std::int64_t>(
                    resume_position_ticks_,
                    0,
                    duration_ticks_ > 0 ? duration_ticks_ : resume_position_ticks_);
                controller_.Position(winrt::Windows::Foundation::TimeSpan{ value });
                software_clock_position_.store(value, std::memory_order_release);
                software_seek_generation_.fetch_add(1, std::memory_order_acq_rel);
                render();
                return Status::success;
            }
            catch (...)
            {
                return Status::media_failed;
            }
        }

        Status set_volume(std::int64_t percent)
        {
            volume_percent_ = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(percent, 0, 100));
            if (players_[0] != nullptr)
            {
                try
                {
                    players_[0].Volume(static_cast<double>(volume_percent_) / 100.0);
                }
                catch (...)
                {
                    return Status::media_failed;
                }
            }
            return Status::success;
        }

        Status set_muted(std::int64_t muted)
        {
            muted_ = muted != 0;
            if (players_[0] != nullptr)
            {
                try
                {
                    players_[0].IsMuted(muted_);
                }
                catch (...)
                {
                    return Status::media_failed;
                }
            }
            return Status::success;
        }

        Status set_view_mode(std::int64_t projected) noexcept
        {
            const bool requested_projected = projected != 0;
            if (requested_projected == projected_view_)
            {
                return failed_ ? Status::media_failed : Status::success;
            }
            projected_view_ = requested_projected;
            if (proxy_mode_)
            {
                if (projected_view_)
                {
                    suspend_software_decoder();
                }
                else if (software_active_)
                {
                    resume_software_decoder();
                }
                else if (!start_software_decoder())
                {
                    return Status::media_failed;
                }
            }
            render();
            return failed_ ? Status::media_failed : Status::success;
        }

        Status set_settings(
            std::span<const MediaSettingValue> settings,
            std::uint64_t generation) noexcept
        {
            if (generation < settings_generation_)
            {
                return Status::success;
            }
            settings_generation_ = generation;
            const bool insta360 = _wcsicmp(
                source_path_.extension().c_str(),
                L".insv") == 0;
            const wchar_t* view_angle_id = insta360
                ? L"insv-view-angle"
                : L"osv-view-angle";
            const wchar_t* overlap_id = insta360
                ? L"insv-overlap"
                : L"osv-overlap";
            float view_angle = lens_fisheye_fov_;
            float overlap = lens_blend_overlap_;
            for (const auto& setting : settings)
            {
                if (wmemchr(
                        setting.setting_id,
                        L'\0',
                        media_setting_id_capacity) == nullptr)
                {
                    return Status::invalid_request;
                }
                if (wcscmp(setting.setting_id, view_angle_id) == 0)
                {
                    view_angle = std::clamp(
                        static_cast<float>(setting.value) / 10.0F,
                        minimum_lens_fisheye_fov,
                        maximum_lens_fisheye_fov);
                }
                else if (wcscmp(setting.setting_id, overlap_id) == 0)
                {
                    overlap = std::clamp(
                        static_cast<float>(setting.value) / 10.0F,
                        minimum_lens_blend_overlap,
                        maximum_lens_blend_overlap);
                }
            }
            if (view_angle != lens_fisheye_fov_ ||
                overlap != lens_blend_overlap_)
            {
                lens_fisheye_fov_ = view_angle;
                lens_blend_overlap_ = overlap;
                render();
            }
            return Status::success;
        }

        bool presentation_ready() const noexcept
        {
            if (!ready_)
            {
                return false;
            }
            if (proxy_mode_)
            {
                return projected_view_
                    ? frames_[0].valid
                    : software_frames_[0].valid && software_frames_[1].valid;
            }
            return frames_[0].valid && software_frames_[1].valid;
        }

        MediaState state() noexcept
        {
            const bool visual_ready = presentation_ready();
            if (!visual_ready && !failed_ && open_started_tick_ != 0 &&
                GetTickCount64() - open_started_tick_ >= 20000)
            {
                record_failure(
                    failure_open_timeout,
                    HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            }
            MediaState state{
                .duration_ticks = duration_ticks_,
                .position_ticks = resume_position_ticks_,
                .video_width = video_width_,
                .video_height = video_height_,
                .volume_percent = volume_percent_,
                .flags = (visual_ready ? media_state_ready : 0U) |
                    (muted_ ? media_state_muted : 0U) |
                    (failed_ ? media_state_failed : 0U) |
                    (projected_view_ ? media_state_projected_view : 0U),
                .interaction_generation = interaction_generation_,
                .failure_kind = failure_kind_.load(std::memory_order_acquire),
                .failure_hresult = failure_hresult_.load(std::memory_order_acquire) };
            if (controller_ != nullptr)
            {
                try
                {
                    state.position_ticks = controller_.Position().count();
                    if (controller_.State() == MediaTimelineControllerState::Running)
                    {
                        state.flags |= media_state_playing;
                    }
                }
                catch (...)
                {
                }
            }
            sync_software_clock();
            if (proxy_mode_ && !projected_view_)
            {
                state.video_width = software_frame_size;
                state.video_height = software_frame_size;
            }
            return state;
        }

        void unload() noexcept
        {
            ++generation_;
            desired_playing_ = false;
            resume_position_ticks_ = 0;
            reset_media();
            source_path_.clear();
            proxy_path_.clear();
            proxy_mode_ = false;
            if (context_ != nullptr)
            {
                context_->ClearState();
                context_->Flush();
            }
            render_target_ = nullptr;
            swap_chain_ = nullptr;
            constant_buffer_ = nullptr;
            sampler_ = nullptr;
            pixel_shader_ = nullptr;
            vertex_shader_ = nullptr;
            context_ = nullptr;
            device_ = nullptr;
            if (window_ != nullptr && IsWindow(window_))
            {
                DestroyWindow(window_);
            }
            window_ = nullptr;
            cancellation_event_ = nullptr;
            dragging_ = false;
            fov_ = default_fov;
            yaw_ = 0.0F;
            pitch_ = 0.0F;
        }

        ~PanoramaSession()
        {
            unload();
        }

        static LRESULT CALLBACK window_proc(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam) noexcept
        {
            auto* self = reinterpret_cast<PanoramaSession*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
                self = static_cast<PanoramaSession*>(create->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(self));
            }
            if (self == nullptr)
            {
                return DefWindowProcW(window, message, wparam, lparam);
            }
            switch (message)
            {
            case WM_SIZE:
                self->resize_swap_chain(
                    static_cast<std::uint32_t>(LOWORD(lparam)),
                    static_cast<std::uint32_t>(HIWORD(lparam)));
                return 0;
            case WM_LBUTTONDOWN:
                self->dragging_ = true;
                self->drag_point_ = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                SetCapture(window);
                ++self->interaction_generation_;
                return 0;
            case WM_LBUTTONUP:
                if (self->dragging_)
                {
                    self->dragging_ = false;
                    ReleaseCapture();
                    ++self->interaction_generation_;
                }
                return 0;
            case WM_MOUSEMOVE:
                if (self->dragging_)
                {
                    const POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                    RECT bounds{};
                    GetClientRect(window, &bounds);
                    const float scale = self->fov_ /
                        static_cast<float>(std::max(1L, bounds.right - bounds.left));
                    self->yaw_ -= static_cast<float>(point.x - self->drag_point_.x) *
                        scale * 0.01745329252F;
                    self->pitch_ = std::clamp(
                        self->pitch_ -
                            static_cast<float>(point.y - self->drag_point_.y) *
                                scale * 2.0F * 0.01745329252F,
                        -1.55334303F,
                        1.55334303F);
                    self->drag_point_ = point;
                    ++self->interaction_generation_;
                    self->render();
                }
                return 0;
            case WM_MOUSEWHEEL:
                if ((GetKeyState(VK_CONTROL) & 0x8000) == 0)
                {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                    self->fov_ = std::clamp(
                        self->fov_ * std::pow(0.9F, static_cast<float>(delta) /
                            static_cast<float>(WHEEL_DELTA)),
                        minimum_fov,
                        maximum_fov);
                    ++self->interaction_generation_;
                    self->render();
                }
                return 0;
            case frame_message:
                self->on_frames(static_cast<std::uint64_t>(lparam));
                return 0;
            case media_opened_message:
                self->on_media_opened(
                    static_cast<std::uint32_t>(wparam),
                    static_cast<std::uint64_t>(lparam));
                return 0;
            case media_failed_message:
                if (static_cast<std::uint64_t>(lparam) == self->generation_)
                {
                    self->failed_ = true;
                }
                return 0;
            case tracks_changed_message:
                self->select_video_track(
                    static_cast<std::uint32_t>(wparam),
                    static_cast<std::uint64_t>(lparam));
                return 0;
            case software_frame_message:
                self->on_software_frames(static_cast<std::uint64_t>(lparam));
                return 0;
            case software_failed_message:
                if (static_cast<std::uint64_t>(lparam) == self->generation_)
                {
                    self->record_failure(
                        failure_media_player,
                        static_cast<std::int32_t>(wparam));
                }
                return 0;
            case WM_ERASEBKGND:
                return 1;
            default:
                return DefWindowProcW(window, message, wparam, lparam);
            }
        }

    private:
        static void ensure_window_class()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                WNDCLASSEXW window_class{};
                window_class.cbSize = sizeof(window_class);
                window_class.style = CS_DBLCLKS;
                window_class.lpfnWndProc = window_proc;
                window_class.hInstance = GetModuleHandleW(nullptr);
                window_class.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
                window_class.lpszClassName = player_window_class;
                RegisterClassExW(&window_class);
            });
        }

        void reset_media() noexcept
        {
            stop_software_decoder();
            ready_ = false;
            failed_ = false;
            controller_started_ = false;
            opened_mask_ = 0;
            track_selected_mask_ = 0;
            frame_ready_mask_ = 0;
            pending_frame_mask_.store(0, std::memory_order_release);
            active_frame_generation_.store(0, std::memory_order_release);
            failure_kind_.store(0, std::memory_order_release);
            failure_hresult_.store(0, std::memory_order_release);
            duration_ticks_ = 0;
            video_width_ = 0;
            video_height_ = 0;
            open_started_tick_ = 0;
            for (auto& player : players_)
            {
                if (player != nullptr)
                {
                    try
                    {
                        player.Pause();
                        player.Source(nullptr);
                        player.Close();
                    }
                    catch (...)
                    {
                    }
                    player = nullptr;
                }
            }
            items_ = { nullptr, nullptr };
            streams_ = { nullptr, nullptr };
            controller_ = nullptr;
            for (auto& frame : frames_)
            {
                frame.reset();
            }
        }

        bool initialize_graphics(HWND parent, const RECT& bounds) noexcept
        {
            try
            {
                ensure_window_class();
                const int width = std::max(1L, bounds.right - bounds.left);
                const int height = std::max(1L, bounds.bottom - bounds.top);
                window_ = CreateWindowExW(
                    WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
                    player_window_class,
                    nullptr,
                    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                    bounds.left,
                    bounds.top,
                    width,
                    height,
                    parent,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    this);
                if (window_ == nullptr)
                {
                    return false;
                }

                winrt::check_hresult(D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    nullptr,
                    0,
                    D3D11_SDK_VERSION,
                    device_.put(),
                    nullptr,
                    context_.put()));

                auto dxgi_device = device_.as<IDXGIDevice>();
                winrt::com_ptr<IDXGIAdapter> adapter;
                winrt::check_hresult(dxgi_device->GetAdapter(adapter.put()));
                winrt::com_ptr<IDXGIFactory2> factory;
                winrt::check_hresult(adapter->GetParent(
                    __uuidof(IDXGIFactory2),
                    factory.put_void()));
                DXGI_SWAP_CHAIN_DESC1 swap_description{};
                swap_description.Width = static_cast<UINT>(width);
                swap_description.Height = static_cast<UINT>(height);
                swap_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                swap_description.SampleDesc.Count = 1;
                swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swap_description.BufferCount = 2;
                swap_description.Scaling = DXGI_SCALING_STRETCH;
                swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                swap_description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                winrt::check_hresult(factory->CreateSwapChainForHwnd(
                    device_.get(),
                    window_,
                    &swap_description,
                    nullptr,
                    nullptr,
                    swap_chain_.put()));
                factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

                const auto compile = [](const char* source, const char* target) {
                    winrt::com_ptr<ID3DBlob> shader;
                    winrt::com_ptr<ID3DBlob> errors;
                    winrt::check_hresult(D3DCompile(
                        source,
                        std::strlen(source),
                        nullptr,
                        nullptr,
                        nullptr,
                        "main",
                        target,
                        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                        0,
                        shader.put(),
                        errors.put()));
                    return shader;
                };
                const auto vertex_blob = compile(vertex_shader_source, "vs_4_0");
                const auto pixel_blob = compile(pixel_shader_source, "ps_4_0");
                winrt::check_hresult(device_->CreateVertexShader(
                    vertex_blob->GetBufferPointer(),
                    vertex_blob->GetBufferSize(),
                    nullptr,
                    vertex_shader_.put()));
                winrt::check_hresult(device_->CreatePixelShader(
                    pixel_blob->GetBufferPointer(),
                    pixel_blob->GetBufferSize(),
                    nullptr,
                    pixel_shader_.put()));

                D3D11_BUFFER_DESC constant_description{};
                constant_description.ByteWidth = sizeof(ShaderParameters);
                constant_description.Usage = D3D11_USAGE_DEFAULT;
                constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                winrt::check_hresult(device_->CreateBuffer(
                    &constant_description,
                    nullptr,
                    constant_buffer_.put()));
                D3D11_SAMPLER_DESC sampler_description{};
                sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
                winrt::check_hresult(device_->CreateSamplerState(
                    &sampler_description,
                    sampler_.put()));
                create_render_target();
                render();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void create_render_target()
        {
            render_target_ = nullptr;
            winrt::com_ptr<ID3D11Texture2D> back_buffer;
            winrt::check_hresult(swap_chain_->GetBuffer(
                0,
                __uuidof(ID3D11Texture2D),
                back_buffer.put_void()));
            winrt::check_hresult(device_->CreateRenderTargetView(
                back_buffer.get(),
                nullptr,
                render_target_.put()));
        }

        void resize_swap_chain(std::uint32_t width, std::uint32_t height) noexcept
        {
            if (swap_chain_ == nullptr || width == 0 || height == 0)
            {
                return;
            }
            try
            {
                context_->OMSetRenderTargets(0, nullptr, nullptr);
                render_target_ = nullptr;
                winrt::check_hresult(swap_chain_->ResizeBuffers(
                    0,
                    width,
                    height,
                    DXGI_FORMAT_UNKNOWN,
                    0));
                create_render_target();
                render();
            }
            catch (...)
            {
                record_failure(failure_resize, winrt::to_hresult().value);
            }
        }

        bool ensure_frame_resource(
            std::uint32_t index,
            std::uint32_t width,
            std::uint32_t height)
        {
            if (index >= frames_.size() || width == 0 || height == 0)
            {
                return false;
            }
            auto& frame = frames_[index];
            return ensure_frame_resource(frame, width, height);
        }

        bool ensure_frame_resource(
            FrameResource& frame,
            std::uint32_t width,
            std::uint32_t height)
        {
            if (width == 0 || height == 0)
            {
                return false;
            }
            if (frame.texture != nullptr && frame.width == width &&
                frame.height == height)
            {
                return true;
            }
            frame.reset();
            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                D3D11_BIND_RENDER_TARGET;
            winrt::check_hresult(device_->CreateTexture2D(
                &description,
                nullptr,
                frame.texture.put()));
            winrt::check_hresult(device_->CreateShaderResourceView(
                frame.texture.get(),
                nullptr,
                frame.view.put()));
            auto dxgi_surface = frame.texture.as<IDXGISurface>();
            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11SurfaceFromDXGISurface(
                dxgi_surface.get(),
                inspectable.put()));
            frame.surface = inspectable.as<IDirect3DSurface>();
            frame.width = width;
            frame.height = height;
            return true;
        }

        void sync_software_clock() noexcept
        {
            if (!software_active_ || controller_ == nullptr)
            {
                return;
            }
            try
            {
                software_clock_position_.store(
                    controller_.Position().count(),
                    std::memory_order_release);
            }
            catch (...)
            {
            }
        }

        bool start_software_decoder(std::uint32_t slot_mask = 3U) noexcept
        {
            if (software_active_)
            {
                return true;
            }
            stop_software_decoder();
            software_active_ = true;
            software_slot_mask_ = slot_mask;
            software_stop_.store(false, std::memory_order_release);
            software_suspended_.store(false, std::memory_order_release);
            software_pending_mask_.store(0, std::memory_order_release);
            software_seek_generation_.fetch_add(1, std::memory_order_acq_rel);
            software_generation_.store(generation_, std::memory_order_release);
            sync_software_clock();
            const auto generation = generation_;
            const auto path = source_path_.wstring();
            try
            {
                for (std::uint32_t slot = 0; slot < software_threads_.size(); ++slot)
                {
                    if ((slot_mask & (1U << slot)) == 0)
                    {
                        continue;
                    }
                    software_threads_[slot] = std::thread(
                        [this, slot, path, generation] {
                            decode_software_track(slot, path, generation);
                        });
                }
                return true;
            }
            catch (...)
            {
                stop_software_decoder();
                record_failure(failure_media_player, winrt::to_hresult().value);
                return false;
            }
        }

        void stop_software_decoder() noexcept
        {
            {
                std::scoped_lock lock(software_state_mutex_);
                software_stop_.store(true, std::memory_order_release);
                software_suspended_.store(false, std::memory_order_release);
            }
            software_seek_generation_.fetch_add(1, std::memory_order_acq_rel);
            software_condition_.notify_all();
            for (auto& thread : software_threads_)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
            software_active_ = false;
            software_slot_mask_ = 0;
            software_generation_.store(0, std::memory_order_release);
            software_pending_mask_.store(0, std::memory_order_release);
            {
                std::scoped_lock lock(software_frame_mutex_);
                software_pending_frames_ = {};
            }
            for (auto& frame : software_frames_)
            {
                frame.reset();
            }
        }

        void suspend_software_decoder() noexcept
        {
            if (!software_active_)
            {
                return;
            }
            sync_software_clock();
            std::scoped_lock lock(software_state_mutex_);
            software_suspended_.store(true, std::memory_order_release);
        }

        void resume_software_decoder() noexcept
        {
            if (!software_active_)
            {
                return;
            }
            sync_software_clock();
            software_seek_generation_.fetch_add(1, std::memory_order_acq_rel);
            software_pending_mask_.store(0, std::memory_order_release);
            {
                std::scoped_lock lock(software_frame_mutex_);
                software_pending_frames_ = {};
            }
            for (std::uint32_t slot = 0; slot < software_frames_.size(); ++slot)
            {
                if ((software_slot_mask_ & (1U << slot)) != 0)
                {
                    software_frames_[slot].reset();
                }
            }
            {
                std::scoped_lock lock(software_state_mutex_);
                software_suspended_.store(false, std::memory_order_release);
            }
            software_condition_.notify_all();
        }

        void decode_software_track(
            std::uint32_t slot,
            const std::wstring& path,
            std::uint64_t generation) noexcept
        {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool apartment_initialized = SUCCEEDED(apartment);
            bool media_foundation_started = false;
            try
            {
                winrt::check_hresult(apartment);
                winrt::check_hresult(MFStartup(MF_VERSION));
                media_foundation_started = true;

                winrt::com_ptr<IMFAttributes> attributes;
                winrt::check_hresult(MFCreateAttributes(attributes.put(), 2));
                winrt::check_hresult(attributes->SetUINT32(
                    MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                    TRUE));
                winrt::check_hresult(attributes->SetUINT32(
                    MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                    FALSE));
                winrt::com_ptr<IMFSourceReader> reader;
                winrt::check_hresult(MFCreateSourceReaderFromURL(
                    path.c_str(),
                    attributes.get(),
                    reader.put()));

                std::vector<DWORD> video_streams;
                std::optional<DWORD> selected_video_stream;
                for (DWORD index = 0; index < 16; ++index)
                {
                    winrt::com_ptr<IMFMediaType> native_type;
                    const HRESULT type_result = reader->GetNativeMediaType(
                        index,
                        0,
                        native_type.put());
                    if (type_result == MF_E_INVALIDSTREAMNUMBER)
                    {
                        break;
                    }
                    if (FAILED(type_result))
                    {
                        continue;
                    }
                    GUID major_type{};
                    if (SUCCEEDED(native_type->GetMajorType(&major_type)) &&
                        major_type == MFMediaType_Video)
                    {
                        video_streams.push_back(index);
                        BOOL selected{};
                        if (SUCCEEDED(reader->GetStreamSelection(index, &selected)) &&
                            selected)
                        {
                            selected_video_stream = index;
                        }
                    }
                }
                if (video_streams.size() < 2)
                {
                    winrt::throw_hresult(MF_E_INVALIDSTREAMNUMBER);
                }
                const DWORD primary_stream = selected_video_stream.value_or(
                    video_streams.back());
                const auto secondary = std::find_if(
                    video_streams.begin(),
                    video_streams.end(),
                    [primary_stream](DWORD stream) {
                        return stream != primary_stream;
                    });
                if (secondary == video_streams.end())
                {
                    winrt::throw_hresult(MF_E_INVALIDSTREAMNUMBER);
                }
                const DWORD stream_index = slot == 0
                    ? primary_stream
                    : *secondary;
                winrt::check_hresult(reader->SetStreamSelection(
                    static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),
                    FALSE));
                winrt::check_hresult(reader->SetStreamSelection(stream_index, TRUE));

                winrt::com_ptr<IMFMediaType> output_type;
                winrt::check_hresult(MFCreateMediaType(output_type.put()));
                winrt::check_hresult(output_type->SetGUID(
                    MF_MT_MAJOR_TYPE,
                    MFMediaType_Video));
                winrt::check_hresult(output_type->SetGUID(
                    MF_MT_SUBTYPE,
                    MFVideoFormat_RGB32));
                winrt::check_hresult(MFSetAttributeSize(
                    output_type.get(),
                    MF_MT_FRAME_SIZE,
                    software_frame_size,
                    software_frame_size));
                winrt::check_hresult(reader->SetCurrentMediaType(
                    stream_index,
                    nullptr,
                    output_type.get()));

                auto seek_reader = [&](std::int64_t position) {
                    PROPVARIANT value{};
                    value.vt = VT_I8;
                    value.hVal.QuadPart = position;
                    winrt::check_hresult(reader->SetCurrentPosition(GUID_NULL, value));
                };
                auto seek_generation = software_seek_generation_.load(
                    std::memory_order_acquire);
                seek_reader(software_clock_position_.load(std::memory_order_acquire));

                while (!software_stop_.load(std::memory_order_acquire) &&
                    generation == software_generation_.load(std::memory_order_acquire))
                {
                    {
                        std::unique_lock lock(software_state_mutex_);
                        software_condition_.wait(lock, [this, generation] {
                            return software_stop_.load(std::memory_order_acquire) ||
                                generation != software_generation_.load(
                                    std::memory_order_acquire) ||
                                !software_suspended_.load(std::memory_order_acquire);
                        });
                    }
                    if (software_stop_.load(std::memory_order_acquire) ||
                        generation != software_generation_.load(
                            std::memory_order_acquire))
                    {
                        break;
                    }
                    const auto requested_seek = software_seek_generation_.load(
                        std::memory_order_acquire);
                    if (requested_seek != seek_generation)
                    {
                        seek_reader(software_clock_position_.load(
                            std::memory_order_acquire));
                        seek_generation = requested_seek;
                    }

                    DWORD actual_stream{};
                    DWORD flags{};
                    LONGLONG timestamp{};
                    winrt::com_ptr<IMFSample> sample;
                    winrt::check_hresult(reader->ReadSample(
                        stream_index,
                        0,
                        &actual_stream,
                        &flags,
                        &timestamp,
                        sample.put()));
                    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                    {
                        break;
                    }
                    if (sample == nullptr)
                    {
                        continue;
                    }

                    SoftwareFrame frame;
                    frame.timestamp = timestamp;
                    frame.pixels.resize(
                        static_cast<std::size_t>(software_frame_size) *
                        software_frame_size * 4);
                    winrt::com_ptr<IMFMediaBuffer> buffer;
                    winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
                    BYTE* source{};
                    DWORD length{};
                    winrt::check_hresult(buffer->Lock(&source, nullptr, &length));
                    if (length < frame.pixels.size())
                    {
                        buffer->Unlock();
                        winrt::throw_hresult(MF_E_BUFFERTOOSMALL);
                    }
                    std::memcpy(frame.pixels.data(), source, frame.pixels.size());
                    buffer->Unlock();

                    bool discard = false;
                    while (!software_stop_.load(std::memory_order_acquire) &&
                        generation == software_generation_.load(
                            std::memory_order_acquire))
                    {
                        if (software_seek_generation_.load(std::memory_order_acquire) !=
                            seek_generation)
                        {
                            discard = true;
                            break;
                        }
                        if (software_suspended_.load(std::memory_order_acquire))
                        {
                            discard = true;
                            break;
                        }
                        const auto position = software_clock_position_.load(
                            std::memory_order_acquire);
                        if (frame.timestamp <= position + 500000)
                        {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    if (discard || software_stop_.load(std::memory_order_acquire) ||
                        generation != software_generation_.load(
                            std::memory_order_acquire))
                    {
                        continue;
                    }
                    {
                        std::scoped_lock lock(software_frame_mutex_);
                        software_pending_frames_[slot] = std::move(frame);
                    }
                    const auto bit = 1U << slot;
                    if (software_pending_mask_.fetch_or(
                            bit,
                            std::memory_order_acq_rel) == 0 &&
                        window_ != nullptr)
                    {
                        PostMessageW(
                            window_,
                            software_frame_message,
                            0,
                            static_cast<LPARAM>(generation));
                    }
                }
            }
            catch (...)
            {
                if (!software_stop_.load(std::memory_order_acquire) &&
                    generation == software_generation_.load(
                        std::memory_order_acquire) &&
                    window_ != nullptr)
                {
                    PostMessageW(
                        window_,
                        software_failed_message,
                        static_cast<WPARAM>(winrt::to_hresult().value),
                        static_cast<LPARAM>(generation));
                }
            }
            if (media_foundation_started)
            {
                MFShutdown();
            }
            if (apartment_initialized)
            {
                CoUninitialize();
            }
        }

        void on_software_frames(std::uint64_t generation) noexcept
        {
            const auto pending = software_pending_mask_.exchange(
                0,
                std::memory_order_acq_rel);
            if (!software_active_ || generation != generation_)
            {
                return;
            }
            try
            {
                std::array<std::optional<SoftwareFrame>, 2> frames;
                {
                    std::scoped_lock lock(software_frame_mutex_);
                    for (std::uint32_t slot = 0; slot < frames.size(); ++slot)
                    {
                        if ((pending & (1U << slot)) != 0)
                        {
                            frames[slot] = std::move(software_pending_frames_[slot]);
                            software_pending_frames_[slot].reset();
                        }
                    }
                }
                for (std::uint32_t slot = 0; slot < frames.size(); ++slot)
                {
                    if (!frames[slot].has_value() ||
                        !ensure_frame_resource(
                            software_frames_[slot],
                            software_frame_size,
                            software_frame_size))
                    {
                        continue;
                    }
                    context_->UpdateSubresource(
                        software_frames_[slot].texture.get(),
                        0,
                        nullptr,
                        frames[slot]->pixels.data(),
                        software_frame_size * 4,
                        0);
                    software_frames_[slot].valid = true;
                }
                render();
            }
            catch (...)
            {
                record_failure(failure_frame_copy, winrt::to_hresult().value);
            }
        }

        winrt::fire_and_forget open_media_async(
            std::wstring path,
            std::uint64_t generation,
            bool proxy_mode)
        {
            try
            {
                const auto file = co_await winrt::Windows::Storage::StorageFile::
                    GetFileFromPathAsync(path);
                auto stream0 = co_await file.OpenAsync(
                    winrt::Windows::Storage::FileAccessMode::Read);
                if (generation != generation_ ||
                    WaitForSingleObject(cancellation_event_, 0) == WAIT_OBJECT_0)
                {
                    co_return;
                }

                streams_[0] = stream0;
                controller_ = MediaTimelineController();
                active_frame_generation_.store(generation, std::memory_order_release);
                configure_player(0, 0, stream0, generation);
                if (!proxy_mode && !start_software_decoder(1U << 1U))
                {
                    PostMessageW(
                        window_,
                        media_failed_message,
                        0,
                        static_cast<LPARAM>(generation));
                }
            }
            catch (...)
            {
                failure_kind_.store(failure_open_async, std::memory_order_release);
                failure_hresult_.store(
                    winrt::to_hresult().value,
                    std::memory_order_release);
                if (generation == generation_ && window_ != nullptr)
                {
                    PostMessageW(
                        window_,
                        media_failed_message,
                        0,
                        static_cast<LPARAM>(generation));
                }
            }
        }

        void configure_player(
            std::uint32_t slot,
            std::uint32_t track,
            const IRandomAccessStream& stream,
            std::uint64_t generation)
        {
            auto source = winrt::Windows::Media::Core::MediaSource::CreateFromStream(
                stream,
                L"video/mp4");
            MediaPlaybackItem item(source);
            MediaPlayer player;
            player.CommandManager().IsEnabled(false);
            player.IsVideoFrameServerEnabled(true);
            player.TimelineController(controller_);
            player.Volume(static_cast<double>(volume_percent_) / 100.0);
            player.IsMuted(slot != 0 || muted_);
            item.VideoTracksChanged([this, slot, generation](auto const&, auto const&) {
                if (window_ != nullptr)
                {
                    PostMessageW(
                        window_,
                        tracks_changed_message,
                        slot,
                        static_cast<LPARAM>(generation));
                }
            });
            player.MediaOpened([this, slot, generation](auto const&, auto const&) {
                if (window_ != nullptr)
                {
                    PostMessageW(
                        window_,
                        media_opened_message,
                        slot,
                        static_cast<LPARAM>(generation));
                }
            });
            player.MediaFailed([this, generation](auto const&, auto const& args) {
                try
                {
                    failure_kind_.store(
                        failure_media_player +
                            static_cast<std::uint32_t>(args.Error()),
                        std::memory_order_release);
                    failure_hresult_.store(
                        args.ExtendedErrorCode().value,
                        std::memory_order_release);
                }
                catch (...)
                {
                }
                if (window_ != nullptr)
                {
                    PostMessageW(
                        window_,
                        media_failed_message,
                        0,
                        static_cast<LPARAM>(generation));
                }
            });
            player.VideoFrameAvailable([this, slot, generation](auto const&, auto const&) {
                if (window_ != nullptr &&
                    active_frame_generation_.load(std::memory_order_acquire) == generation)
                {
                    const auto bit = 1U << slot;
                    if (pending_frame_mask_.fetch_or(
                            bit,
                            std::memory_order_acq_rel) == 0)
                    {
                        PostMessageW(
                            window_,
                            frame_message,
                            0,
                            static_cast<LPARAM>(generation));
                    }
                }
            });
            items_[slot] = item;
            players_[slot] = player;
            player.Source(item);
            select_video_track(slot, generation, track);
        }

        void select_video_track(
            std::uint32_t slot,
            std::uint64_t generation,
            std::optional<std::uint32_t> requested_track = std::nullopt)
        {
            if (generation != generation_ || slot >= items_.size() ||
                items_[slot] == nullptr)
            {
                return;
            }
            try
            {
                const auto tracks = items_[slot].VideoTracks();
                const auto track = requested_track.value_or(0U);
                if (tracks.Size() > track &&
                    tracks.SelectedIndex() != static_cast<std::int32_t>(track))
                {
                    tracks.SelectedIndex(static_cast<std::int32_t>(track));
                }
                if (tracks.Size() > track)
                {
                    track_selected_mask_ |= 1U << slot;
                    prepare_frame_resource(slot, track);
                    update_ready();
                }
            }
            catch (...)
            {
                record_failure(
                    failure_track_selection,
                    winrt::to_hresult().value);
            }
        }

        void on_media_opened(std::uint32_t slot, std::uint64_t generation) noexcept
        {
            if (generation != generation_ || slot >= players_.size() ||
                players_[slot] == nullptr)
            {
                return;
            }
            try
            {
                select_video_track(slot, generation);
                const auto session = players_[slot].PlaybackSession();
                if ((frame_ready_mask_ & (1U << slot)) == 0)
                {
                    const auto width = session.NaturalVideoWidth();
                    const auto height = session.NaturalVideoHeight();
                    if (width != 0 && height != 0 &&
                        ensure_frame_resource(slot, width, height))
                    {
                        frame_ready_mask_ |= 1U << slot;
                        video_width_ = width;
                        video_height_ = height;
                    }
                }
                duration_ticks_ = std::max(
                    duration_ticks_,
                    session.NaturalDuration().count());
                opened_mask_ |= 1U << slot;
                update_ready();
            }
            catch (...)
            {
                record_failure(failure_media_opened, winrt::to_hresult().value);
            }
        }

        void prepare_frame_resource(std::uint32_t slot, std::uint32_t track)
        {
            if (slot >= items_.size() || items_[slot] == nullptr)
            {
                return;
            }
            const auto tracks = items_[slot].VideoTracks();
            if (tracks.Size() <= track)
            {
                return;
            }
            const auto properties = tracks.GetAt(track).GetEncodingProperties();
            const auto width = properties.Width();
            const auto height = properties.Height();
            if (width == 0 || height == 0 ||
                !ensure_frame_resource(slot, width, height))
            {
                return;
            }
            frame_ready_mask_ |= 1U << slot;
            video_width_ = width;
            video_height_ = height;
        }

        void on_frames(std::uint64_t generation) noexcept
        {
            const auto pending = pending_frame_mask_.exchange(
                0,
                std::memory_order_acq_rel);
            if (generation != generation_)
            {
                return;
            }
            try
            {
                for (std::uint32_t slot = 0; slot < players_.size(); ++slot)
                {
                    if ((pending & (1U << slot)) == 0 ||
                        players_[slot] == nullptr || frames_[slot].surface == nullptr)
                    {
                        continue;
                    }
                    players_[slot].CopyFrameToVideoSurface(frames_[slot].surface);
                    frames_[slot].valid = true;
                }
                sync_software_clock();
                render();
            }
            catch (...)
            {
                record_failure(failure_frame_copy, winrt::to_hresult().value);
            }
        }

        void update_ready() noexcept
        {
            if (ready_)
            {
                return;
            }
            constexpr std::uint32_t required_mask = 1U;
            if ((opened_mask_ & required_mask) != required_mask ||
                (track_selected_mask_ & required_mask) != required_mask ||
                (frame_ready_mask_ & required_mask) != required_mask)
            {
                return;
            }
            ready_ = true;
            if (resume_position_ticks_ > 0 && controller_ != nullptr)
            {
                try
                {
                    controller_.Position(winrt::Windows::Foundation::TimeSpan{
                        std::min(resume_position_ticks_, duration_ticks_) });
                }
                catch (...)
                {
                }
            }
            if (desired_playing_)
            {
                static_cast<void>(play());
            }
        }

        void render() noexcept
        {
            if (context_ == nullptr || render_target_ == nullptr ||
                swap_chain_ == nullptr)
            {
                return;
            }
            try
            {
                RECT bounds{};
                GetClientRect(window_, &bounds);
                const float width = static_cast<float>(std::max(1L, bounds.right));
                const float height = static_cast<float>(std::max(1L, bounds.bottom));
                const float clear_color[]{
                    static_cast<float>(GetRValue(visuals_.background_color)) / 255.0F,
                    static_cast<float>(GetGValue(visuals_.background_color)) / 255.0F,
                    static_cast<float>(GetBValue(visuals_.background_color)) / 255.0F,
                    1.0F };
                ID3D11RenderTargetView* render_targets[]{ render_target_.get() };
                context_->OMSetRenderTargets(1, render_targets, nullptr);
                context_->ClearRenderTargetView(render_target_.get(), clear_color);
                const FrameResource* first_frame{};
                const FrameResource* second_frame{};
                float source_mode{};
                if (proxy_mode_ && projected_view_)
                {
                    first_frame = &frames_[0];
                    second_frame = &frames_[0];
                    source_mode = 3.0F;
                }
                else if (proxy_mode_)
                {
                    first_frame = &software_frames_[0];
                    second_frame = &software_frames_[1];
                    source_mode = 2.0F;
                }
                else
                {
                    first_frame = &frames_[0];
                    second_frame = &software_frames_[1];
                    source_mode = projected_view_ ? 1.0F : 2.0F;
                }
                if (!first_frame->valid || !second_frame->valid)
                {
                    swap_chain_->Present(1, 0);
                    return;
                }

                const ShaderParameters parameters{
                    .aspect = width / height,
                    .fov_radians = fov_ * 0.01745329252F,
                    .yaw = yaw_,
                    .pitch = pitch_,
                    .source_mode = source_mode,
                    .fisheye_fov_radians = lens_fisheye_fov_ * 0.01745329252F,
                    .blend_overlap = std::sin(
                        lens_blend_overlap_ * 0.01745329252F) };
                context_->UpdateSubresource(
                    constant_buffer_.get(),
                    0,
                    nullptr,
                    &parameters,
                    0,
                    0);
                const D3D11_VIEWPORT viewport{
                    .TopLeftX = 0.0F,
                    .TopLeftY = 0.0F,
                    .Width = width,
                    .Height = height,
                    .MinDepth = 0.0F,
                    .MaxDepth = 1.0F };
                context_->RSSetViewports(1, &viewport);
                context_->IASetInputLayout(nullptr);
                context_->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
                context_->PSSetShader(pixel_shader_.get(), nullptr, 0);
                ID3D11Buffer* constants[]{ constant_buffer_.get() };
                context_->PSSetConstantBuffers(0, 1, constants);
                ID3D11SamplerState* samplers[]{ sampler_.get() };
                context_->PSSetSamplers(0, 1, samplers);
                ID3D11ShaderResourceView* views[]{
                    first_frame->view.get(),
                    second_frame->view.get() };
                context_->PSSetShaderResources(0, 2, views);
                context_->Draw(3, 0);
                ID3D11ShaderResourceView* empty_views[]{ nullptr, nullptr };
                context_->PSSetShaderResources(0, 2, empty_views);
                swap_chain_->Present(1, 0);
            }
            catch (...)
            {
                record_failure(failure_render, winrt::to_hresult().value);
            }
        }

        void record_failure(std::uint32_t kind, std::int32_t code) noexcept
        {
            failure_kind_.store(kind, std::memory_order_release);
            failure_hresult_.store(code, std::memory_order_release);
            failed_ = true;
        }

        HWND window_{};
        RECT bounds_{};
        PreviewVisuals visuals_{};
        HANDLE cancellation_event_{};
        std::uint64_t generation_{};
        std::uint64_t interaction_generation_{};
        ULONGLONG open_started_tick_{};
        std::array<MediaPlayer, 2> players_{ nullptr, nullptr };
        std::array<MediaPlaybackItem, 2> items_{ nullptr, nullptr };
        std::array<IRandomAccessStream, 2> streams_{ nullptr, nullptr };
        MediaTimelineController controller_{ nullptr };
        std::array<FrameResource, 2> frames_;
        std::array<FrameResource, 2> software_frames_;
        std::array<std::thread, 2> software_threads_;
        std::array<std::optional<SoftwareFrame>, 2> software_pending_frames_;
        std::mutex software_frame_mutex_;
        std::mutex software_state_mutex_;
        std::condition_variable software_condition_;
        winrt::com_ptr<ID3D11Device> device_;
        winrt::com_ptr<ID3D11DeviceContext> context_;
        winrt::com_ptr<IDXGISwapChain1> swap_chain_;
        winrt::com_ptr<ID3D11RenderTargetView> render_target_;
        winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
        winrt::com_ptr<ID3D11PixelShader> pixel_shader_;
        winrt::com_ptr<ID3D11Buffer> constant_buffer_;
        winrt::com_ptr<ID3D11SamplerState> sampler_;
        std::int64_t duration_ticks_{};
        std::int64_t resume_position_ticks_{};
        std::uint32_t video_width_{};
        std::uint32_t video_height_{};
        std::uint32_t volume_percent_{ 100 };
        std::uint32_t opened_mask_{};
        std::uint32_t track_selected_mask_{};
        std::uint32_t frame_ready_mask_{};
        std::uint32_t software_slot_mask_{};
        std::atomic_uint32_t pending_frame_mask_{};
        std::atomic_uint32_t software_pending_mask_{};
        std::atomic_uint64_t software_generation_{};
        std::atomic_uint64_t software_seek_generation_{};
        std::atomic_int64_t software_clock_position_{};
        std::atomic_bool software_stop_{ true };
        std::atomic_bool software_suspended_{};
        std::atomic_uint64_t active_frame_generation_{};
        std::atomic_uint32_t failure_kind_{};
        std::atomic_int32_t failure_hresult_{};
        std::uint64_t settings_generation_{};
        float fov_{ default_fov };
        float lens_fisheye_fov_{ dji_fisheye_fov };
        float lens_blend_overlap_{ dji_blend_overlap };
        float yaw_{};
        float pitch_{};
        POINT drag_point_{};
        std::filesystem::path source_path_;
        std::filesystem::path proxy_path_;
        bool proxy_mode_{};
        bool ready_{};
        bool failed_{};
        bool desired_playing_{};
        bool controller_started_{};
        bool muted_{};
        bool dragging_{};
        bool projected_view_{ true };
        bool software_active_{};
    };

    struct QueuedRequest
    {
        RequestHeader header;
        std::vector<std::byte> payload;
    };

    class RequestQueue final
    {
    public:
        void push(QueuedRequest request)
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(std::move(request));
        }

        std::optional<QueuedRequest> pop()
        {
            std::scoped_lock lock(mutex_);
            if (requests_.empty())
            {
                return std::nullopt;
            }
            auto request = std::move(requests_.front());
            requests_.pop_front();
            return request;
        }

    private:
        std::mutex mutex_;
        std::deque<QueuedRequest> requests_;
    };

    struct ProcessResponse
    {
        Status status{ Status::invalid_request };
        std::vector<std::byte> payload;
    };

    ProcessResponse process_request(
        const QueuedRequest& queued,
        PanoramaSession& session,
        HANDLE cancellation_event)
    {
        if (WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0 &&
            queued.header.command != Command::shutdown)
        {
            return { .status = Status::cancelled };
        }
        switch (queued.header.command)
        {
        case Command::open_document:
        {
            if (queued.payload.size() < sizeof(OpenRequest))
            {
                return {};
            }
            const auto* request = reinterpret_cast<const OpenRequest*>(
                queued.payload.data());
            const std::size_t path_bytes =
                static_cast<std::size_t>(request->path_characters) * sizeof(wchar_t);
            if (path_bytes != queued.payload.size() - sizeof(OpenRequest))
            {
                return {};
            }
            const auto* path_data = reinterpret_cast<const wchar_t*>(
                queued.payload.data() + sizeof(OpenRequest));
            const std::wstring path(path_data, request->path_characters);
            return { .status = session.open(
                path,
                reinterpret_cast<HWND>(request->parent_window),
                {
                    request->bounds.left,
                    request->bounds.top,
                    request->bounds.right,
                    request->bounds.bottom },
                request->visuals,
                cancellation_event) };
        }
        case Command::resize:
            if (queued.payload.size() == sizeof(ResizeRequest))
            {
                const auto& request = *reinterpret_cast<const ResizeRequest*>(
                    queued.payload.data());
                return { .status = session.resize({
                    request.bounds.left,
                    request.bounds.top,
                    request.bounds.right,
                    request.bounds.bottom }) };
            }
            return {};
        case Command::set_visuals:
            return queued.payload.size() == sizeof(PreviewVisuals)
                ? ProcessResponse{ .status = session.set_visuals(
                    *reinterpret_cast<const PreviewVisuals*>(queued.payload.data())) }
                : ProcessResponse{};
        case Command::media_play:
            return queued.payload.empty()
                ? ProcessResponse{ .status = session.play() }
                : ProcessResponse{};
        case Command::media_pause:
            return queued.payload.empty()
                ? ProcessResponse{ .status = session.pause() }
                : ProcessResponse{};
        case Command::media_seek:
        case Command::media_set_volume:
        case Command::media_set_muted:
        case Command::media_set_view_mode:
            if (queued.payload.size() == sizeof(MediaValueRequest))
            {
                const auto value = reinterpret_cast<const MediaValueRequest*>(
                    queued.payload.data())->value;
                if (queued.header.command == Command::media_seek)
                {
                    return { .status = session.seek(value) };
                }
                if (queued.header.command == Command::media_set_volume)
                {
                    return { .status = session.set_volume(value) };
                }
                if (queued.header.command == Command::media_set_view_mode)
                {
                    return { .status = session.set_view_mode(value) };
                }
                return { .status = session.set_muted(value) };
            }
            return {};
        case Command::media_query_state:
            if (queued.payload.empty())
            {
                const auto state = session.state();
                ProcessResponse response{ .status = Status::success };
                response.payload.resize(sizeof(state));
                std::memcpy(response.payload.data(), &state, sizeof(state));
                return response;
            }
            return {};
        case Command::media_set_settings:
            if (queued.payload.size() >= sizeof(MediaSettingsRequest))
            {
                const auto* request = reinterpret_cast<const MediaSettingsRequest*>(
                    queued.payload.data());
                const auto value_bytes = queued.payload.size() -
                    sizeof(MediaSettingsRequest);
                if (request->count <= value_bytes / sizeof(MediaSettingValue) &&
                    value_bytes == static_cast<std::size_t>(request->count) *
                        sizeof(MediaSettingValue))
                {
                    const auto* values = reinterpret_cast<const MediaSettingValue*>(
                        queued.payload.data() + sizeof(MediaSettingsRequest));
                    return { .status = session.set_settings(
                        std::span(values, request->count),
                        request->generation) };
                }
            }
            return {};
        case Command::unload:
        case Command::shutdown:
            session.unload();
            return { .status = Status::success };
        default:
            return {};
        }
    }
}

namespace glance::panorama
{
    int run_player_host(
        HANDLE request_pipe,
        HANDLE response_pipe,
        HANDLE cancellation_event)
    {
        if (request_pipe == nullptr || response_pipe == nullptr ||
            cancellation_event == nullptr)
        {
            return 2;
        }
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        const DWORD sta_thread = GetCurrentThreadId();
        MSG initial_message{};
        PeekMessageW(&initial_message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        RequestQueue queue;
        std::atomic_bool stopped{};
        std::thread reader([&] {
            while (!stopped.load(std::memory_order_acquire))
            {
                QueuedRequest request;
                if (!read_exact(request_pipe, &request.header, sizeof(request.header)) ||
                    request.header.magic != protocol_magic ||
                    request.header.version != protocol_version ||
                    request.header.payload_size > maximum_payload_size)
                {
                    break;
                }
                request.payload.resize(request.header.payload_size);
                if (!request.payload.empty() &&
                    !read_exact(
                        request_pipe,
                        request.payload.data(),
                        request.payload.size()))
                {
                    break;
                }
                queue.push(std::move(request));
                PostThreadMessageW(sta_thread, request_message, 0, 0);
            }
            PostThreadMessageW(sta_thread, WM_QUIT, 0, 0);
        });
        std::thread cancellation([&] {
            if (WaitForSingleObject(cancellation_event, INFINITE) == WAIT_OBJECT_0 &&
                !stopped.load(std::memory_order_acquire))
            {
                PostThreadMessageW(sta_thread, WM_QUIT, 0, 0);
            }
        });

        PanoramaSession session;
        bool running = true;
        MSG message{};
        while (running && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.message != request_message)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }
            while (const auto request = queue.pop())
            {
                const auto result = process_request(
                    *request,
                    session,
                    cancellation_event);
                const ResponseHeader response{
                    .status = result.status,
                    .payload_size = static_cast<std::uint32_t>(result.payload.size()) };
                if (!write_exact(response_pipe, &response, sizeof(response)) ||
                    (!result.payload.empty() &&
                     !write_exact(
                         response_pipe,
                         result.payload.data(),
                         result.payload.size())))
                {
                    running = false;
                    break;
                }
                if (request->header.command == Command::shutdown)
                {
                    running = false;
                    break;
                }
            }
        }

        session.unload();
        stopped.store(true, std::memory_order_release);
        static_cast<void>(CancelSynchronousIo(reader.native_handle()));
        CloseHandle(request_pipe);
        SetEvent(cancellation_event);
        if (reader.joinable())
        {
            reader.join();
        }
        if (cancellation.joinable())
        {
            cancellation.join();
        }
        CloseHandle(response_pipe);
        CloseHandle(cancellation_event);
        return 0;
    }
}
