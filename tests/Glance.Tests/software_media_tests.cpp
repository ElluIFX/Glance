#include "pch.h"
#include "preview/software_media_source.h"
#include "dependencies/dependency_service.h"
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.System.h>
#include <DispatcherQueue.h>
#pragma comment(lib, "CoreMessaging.lib")
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#pragma comment(lib, "D3D11.lib")
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <tlhelp32.h>
#include <wil/resource.h>

namespace
{
    void check_active_cancellation(const wchar_t* path)
    {
        auto source = glance::app::SoftwareMediaSource::open(path);
        std::vector<wil::unique_handle> hosts;
        wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        winrt::check_bool(snapshot.get() != INVALID_HANDLE_VALUE);
        PROCESSENTRY32W entry{sizeof(entry)};
        if (Process32FirstW(snapshot.get(), &entry))
        {
            do
            {
                if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                    _wcsicmp(entry.szExeFile, L"Glance.MediaHost.exe") == 0)
                {
                    wil::unique_handle process(OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID));
                    winrt::check_bool(static_cast<bool>(process));
                    hosts.push_back(std::move(process));
                }
            } while (Process32NextW(snapshot.get(), &entry));
        }
        if (hosts.empty()) throw std::runtime_error("software media cancellation test found no host");
        winrt::Windows::Media::Playback::MediaPlayer player;
        player.IsMuted(true);
        player.Source(source->source());
        player.Play();
        Sleep(50);
        source->cancel();
        for (const auto& host : hosts)
            if (WaitForSingleObject(host.get(), 2000) != WAIT_OBJECT_0)
                throw std::runtime_error("cancelled software media host did not exit");
        player.Source(nullptr);
        player.Close();
        source.reset();
        std::wcout << L"PASS active software media cancellation and host exit\n";
    }
}

int run_software_media_tests(int count, wchar_t* arguments[])
{
    if (count < 4) return 2;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        namespace dependencies = glance::app::dependencies;
        dependencies::register_media_dependency();
        if (dependencies::host_api().query(L"ffmpeg", L"avcodec") != glance::contracts::dependencies::Availability::managed)
            dependencies::install_archive(L"ffmpeg", arguments[2]);
        const bool composition = std::wstring_view(arguments[1]) == L"--software-media-composition-tests";
        winrt::Windows::System::DispatcherQueueController dispatcher{nullptr};
        winrt::Windows::UI::Composition::Compositor compositor{nullptr};
        if (composition)
        {
            winrt::check_hresult(CreateDispatcherQueueController(
                {sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE},
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dispatcher))));
            compositor = winrt::Windows::UI::Composition::Compositor();
        }
        for (int i = 3; i < count; ++i)
        {
            const auto start = GetTickCount64();
            auto source = glance::app::SoftwareMediaSource::open(arguments[i]);
            struct State
            {
                std::atomic_uint frames{};
                std::atomic_bool opened{}, failed{}, ended{};
                std::mutex mutex;
                winrt::com_ptr<ID3D11Device> device;
                winrt::com_ptr<ID3D11DeviceContext> context;
                winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface{nullptr};
            };
            const auto state = std::make_shared<State>();
            auto& frames = state->frames;
            auto& opened = state->opened;
            auto& failed = state->failed;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, state->device.put(), nullptr, state->context.put()));
            winrt::Windows::Media::Playback::MediaPlayer player;
            player.IsVideoFrameServerEnabled(!composition);
            winrt::Windows::Media::Playback::MediaPlayerSurface presentation{nullptr};
            if (composition) presentation = player.GetSurface(compositor);
            player.IsMuted(true);
            if (std::wstring_view(arguments[1]) == L"--software-media-fallback-tests")
            {
                const auto failure = player.MediaFailed([state](auto const&, auto const&) { state->failed = true; });
                const auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(arguments[i]).get();
                player.Source(winrt::Windows::Media::Playback::MediaPlaybackItem(
                    winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file)));
                player.Play();
                const auto deadline = GetTickCount64() + 10000;
                while (!state->failed && GetTickCount64() < deadline) Sleep(10);
                player.MediaFailed(failure);
                if (!state->failed) throw std::runtime_error("expected system decoder failure was not observed");
                try { static_cast<void>(player.PlaybackSession().Position()); }
                catch (const winrt::hresult_error& error) {
                    std::wcout << L"Failed session Position HRESULT=" << std::hex << static_cast<HRESULT>(error.code()) << std::dec << L'\n';
                }
                state->failed = false;
            }
            const auto opened_token = player.MediaOpened([state](auto const&, auto const&) { state->opened = true; });
            const auto ended_token = player.MediaEnded([state](auto const&, auto const&) { state->ended = true; });
            const auto failed_token = player.MediaFailed([state](auto const&, auto const& args) {
                std::wcerr << L"Media failure " << std::hex << static_cast<HRESULT>(args.ExtendedErrorCode()) << L" " << args.ErrorMessage().c_str() << L'\n';
                state->failed = true;
            });
            const auto frame_token = player.VideoFrameAvailable([state](auto const& sender, auto const&) {
                try
                {
                    std::scoped_lock lock(state->mutex);
                    if (!state->surface)
                    {
                        const auto session = sender.PlaybackSession();
                        if (!session.NaturalVideoWidth() || !session.NaturalVideoHeight()) return;
                        D3D11_TEXTURE2D_DESC description{};
                        description.Width = session.NaturalVideoWidth();
                        description.Height = session.NaturalVideoHeight();
                        description.MipLevels = description.ArraySize = 1;
                        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        description.SampleDesc.Count = 1;
                        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        winrt::com_ptr<ID3D11Texture2D> texture;
                        winrt::check_hresult(state->device->CreateTexture2D(&description, nullptr, texture.put()));
                        winrt::check_hresult(CreateDirect3D11SurfaceFromDXGISurface(texture.as<IDXGISurface>().get(),
                            reinterpret_cast<IInspectable**>(winrt::put_abi(state->surface))));
                    }
                    sender.CopyFrameToVideoSurface(state->surface);
                    ++state->frames;
                }
                catch (...) { state->failed = true; }
            });
            player.Source(source->source());
            player.Play();
            while (!opened && !failed && GetTickCount64() - start < 15000) Sleep(10);
            if (!opened || failed) throw std::runtime_error("software media open failed");
            const auto opened_ms = GetTickCount64() - start;
            const auto session = player.PlaybackSession();
            const auto playback_started = GetTickCount64();
            auto previous_position = session.Position().count();
            auto previous_frames = frames.load();
            const auto playback_seconds = std::clamp(session.NaturalDuration().count() / 20000000, 2LL, 10LL);
            for (int second = 0; second < playback_seconds; ++second)
            {
                const auto until = GetTickCount64() + 1000;
                while (!failed && !state->ended && GetTickCount64() < until) Sleep(10);
                const auto position = session.Position().count();
                if (failed || state->ended || position < previous_position + 2500000 ||
                    position > static_cast<std::int64_t>(GetTickCount64() - playback_started + 1500) * 10000 ||
                    (!composition && session.NaturalVideoWidth() && frames.load() < previous_frames + 5))
                    throw std::runtime_error("software media stalled or jumped during continuous playback");
                previous_position = position;
                previous_frames = frames.load();
            }
            std::wcout << L"State=" << static_cast<int>(session.PlaybackState()) << L" position=" << session.Position().count()
                << L" frames=" << frames.load() << L" size=" << session.NaturalVideoWidth() << L"x" << session.NaturalVideoHeight()
                << L" duration=" << session.NaturalDuration().count() << L" rate=" << session.PlaybackRate() << L'\n';
            if (failed || session.Position().count() < 15000000 || (!composition && session.NaturalVideoWidth() && frames < 20))
                throw std::runtime_error("software media did not advance");
            const auto duration = session.NaturalDuration().count();
            const auto target = duration / 2;
            const auto before_seek_frames = frames.load();
            session.Position(winrt::Windows::Foundation::TimeSpan{target});
            const auto seek_until = GetTickCount64() + 5000;
            while (!failed && GetTickCount64() < seek_until && (session.Position().count() < target + 2500000 ||
                (!composition && session.NaturalVideoWidth() && frames < before_seek_frames + 2))) Sleep(10);
            if (failed || session.Position().count() < target + 2500000 ||
                (!composition && session.NaturalVideoWidth() && frames < before_seek_frames + 2)) throw std::runtime_error("software media seek failed");
            player.Pause();
            Sleep(150);
            const auto paused = session.Position().count();
            Sleep(250);
            if (std::abs(session.Position().count() - paused) > 1000000) throw std::runtime_error("software media pause failed");
            player.Play();
            const auto resume_started = GetTickCount64();
            while (!failed && GetTickCount64() - resume_started < 2000 && session.Position().count() < paused + 2000000) Sleep(10);
            std::wcout << L"Resume=" << GetTickCount64() - resume_started << L"ms paused=" << paused << L" position=" << session.Position().count() << L'\n';
            if (failed || session.Position().count() < paused + 2000000)
                throw std::runtime_error("software media resume failed");
            for (int round = 0; round < 12; ++round)
            {
                for (const auto fraction : {7, 2, 6, 1, 4})
                {
                    session.Position(winrt::Windows::Foundation::TimeSpan{duration * fraction / 10});
                    Sleep(30);
                }
                const auto before = frames.load();
                const auto deadline = GetTickCount64() + 5000;
                const auto target_position = duration * 4 / 10;
                while (!failed && !state->ended && GetTickCount64() < deadline &&
                    (session.Position().count() < target_position + 5000000 ||
                     (!composition && session.NaturalVideoWidth() && frames.load() < before + 5))) Sleep(10);
                if (failed || state->ended || session.Position().count() < target_position + 5000000 ||
                    (!composition && session.NaturalVideoWidth() && frames.load() < before + 5))
                    throw std::runtime_error("software media stalled after rapid seeks");
                std::wcout << L"PASS rapid seek round " << round + 1 << L" frames=" << frames.load() << std::endl;
            }
            Sleep(700);
            if (failed || session.Position().count() < duration * 4 / 10 || session.Position().count() > duration * 4 / 10 + 20000000)
                throw std::runtime_error("software media rapid seek failed");
            session.Position(winrt::Windows::Foundation::TimeSpan{duration - 5000000});
            const auto end_until = GetTickCount64() + 8000;
            while (!failed && !state->ended && GetTickCount64() < end_until) Sleep(10);
            if (failed || !state->ended) throw std::runtime_error("software media end of stream failed");
            std::wcout << L"PASS media file " << i - 2 << L" open=" << opened_ms << L"ms frames=" << frames.load()
                << L" duration=" << duration / 10000000.0 << L"s seek/pause/resume/rapid-seek/end\n";
            player.MediaOpened(opened_token);
            player.MediaEnded(ended_token);
            player.MediaFailed(failed_token);
            player.VideoFrameAvailable(frame_token);
            player.Source(nullptr);
            player.Close();
            source->cancel();
        }
        check_active_cancellation(arguments[3]);
        dependencies::shutdown();
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        std::wcerr << L"Software media HRESULT " << std::hex << static_cast<HRESULT>(error.code()) << L" " << error.message().c_str() << L'\n';
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    glance::app::dependencies::shutdown();
    return 1;
}
