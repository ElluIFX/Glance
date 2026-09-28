#include "pch.h"
#include "software_media_source.h"
#include "dependencies/dependency_service.h"
#include "glance/contracts/software_media_protocol.h"
#include "glance/contracts/diagnostics.h"
#include <mfapi.h>
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Dbghelp.lib")
#include <robuffer.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <wil/resource.h>
#include <deque>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

namespace glance::app
{
    namespace
    {
        namespace protocol = contracts::software_media;
        using namespace winrt::Windows::Media::Core;
        using namespace winrt::Windows::Media::MediaProperties;
        using winrt::Windows::Storage::Streams::IBuffer;
        using winrt::Windows::Foundation::TimeSpan;

        struct SampleBuffer : winrt::implements<SampleBuffer, IBuffer, ::Windows::Storage::Streams::IBufferByteAccess>
        {
            std::vector<unsigned char> bytes;
            std::function<void(std::vector<unsigned char>)> release;
            SampleBuffer(std::vector<unsigned char> value, std::function<void(std::vector<unsigned char>)> callback)
                : bytes(std::move(value)), release(std::move(callback)) {}
            ~SampleBuffer() { try { release(std::move(bytes)); } catch (...) {} }
            std::uint32_t Capacity() const { return static_cast<std::uint32_t>(bytes.size()); }
            std::uint32_t Length() const { return static_cast<std::uint32_t>(bytes.size()); }
            void Length(std::uint32_t value)
            {
                if (value > bytes.size()) throw winrt::hresult_invalid_argument();
                bytes.resize(value);
            }
            HRESULT __stdcall Buffer(unsigned char** value) noexcept override
            {
                if (!value) return E_POINTER;
                *value = bytes.data();
                return S_OK;
            }
        };

        struct Session final : SoftwareMediaSource, std::enable_shared_from_this<Session>
        {
            dependencies::Lease lease;
            wil::unique_handle process, job, request_pipe, response_pipe, mapping, cancellation;
            unsigned char* shared{};
            std::mutex queue_mutex, buffer_mutex;
            std::deque<std::function<void()>> requests;
            bool processing{};
            std::array<std::deque<std::vector<unsigned char>>, 2> buffers;
            std::atomic_bool cancelled{};
            std::atomic_uint64_t io_deadline{};
            std::thread watchdog;
            std::shared_ptr<std::atomic_bool> external_cancellation;
            std::array<std::atomic_uint64_t, 2> delivered_samples{};
            std::array<std::int64_t, 2> next_positions{};
            std::array<bool, 2> active_streams{};
            MediaStreamSource stream{nullptr};
            MediaSource media{nullptr};

            ~Session() override
            {
                cancel();
                if (watchdog.joinable()) watchdog.join();
                if (shared) UnmapViewOfFile(shared);
            }
            MediaSource source() const override { return media; }
            void cancel() noexcept override
            {
                if (cancelled.exchange(true)) return;
                if (cancellation) SetEvent(cancellation.get());
                if (job) TerminateJobObject(job.get(), ERROR_CANCELLED);
                try
                {
                    contracts::log_event(L"Software media closed: video samples=" + std::to_wstring(delivered_samples[0].load()) +
                        L", audio samples=" + std::to_wstring(delivered_samples[1].load()));
                }
                catch (...) {}
            }
            void start()
            {
                if (external_cancellation && external_cancellation->load()) throw winrt::hresult_canceled();
                lease = dependencies::acquire(L"ffmpeg", L"avcodec");
                std::wstring executable(32768, L'\0');
                executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
                const auto host = std::filesystem::path(executable).parent_path() / L"Glance.MediaHost.exe";
                SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
                HANDLE read{}, write{};
                winrt::check_bool(CreatePipe(&read, &write, &security, 0));
                wil::unique_handle child_read(read);
                request_pipe.reset(write);
                winrt::check_bool(SetHandleInformation(request_pipe.get(), HANDLE_FLAG_INHERIT, 0));
                winrt::check_bool(CreatePipe(&read, &write, &security, 0));
                response_pipe.reset(read);
                wil::unique_handle child_write(write);
                winrt::check_bool(SetHandleInformation(response_pipe.get(), HANDLE_FLAG_INHERIT, 0));
                mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE | SEC_RESERVE,
                    0, protocol::maximum_sample_bytes, nullptr));
                cancellation.reset(CreateEventW(&security, TRUE, FALSE, nullptr));
                winrt::check_bool(mapping && cancellation);
                shared = static_cast<unsigned char*>(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, protocol::maximum_sample_bytes));
                winrt::check_bool(shared != nullptr);
                std::array<HANDLE, 4> handles{child_read.get(), child_write.get(), mapping.get(), cancellation.get()};
                SIZE_T bytes{};
                InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
                std::vector<std::byte> storage(bytes);
                auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                winrt::check_bool(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes));
                const auto cleanup = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
                winrt::check_bool(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    handles.data(), sizeof(handles), nullptr, nullptr));
                auto command = contracts::dependencies::quote_argument(host.wstring());
                for (const auto handle : handles) command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
                command += L" " + contracts::dependencies::quote_argument(lease.path.parent_path().wstring());
                STARTUPINFOEXW startup{};
                startup.StartupInfo.cb = sizeof(startup);
                startup.lpAttributeList = attributes;
                job.reset(CreateJobObjectW(nullptr, nullptr));
                winrt::check_bool(static_cast<bool>(job));
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                winrt::check_bool(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
                PROCESS_INFORMATION child{};
                winrt::check_bool(CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child));
                process.reset(child.hProcess);
                wil::unique_handle thread(child.hThread);
                if (!AssignProcessToJobObject(job.get(), process.get()))
                {
                    const auto error = GetLastError();
                    TerminateProcess(process.get(), error);
                    winrt::throw_hresult(HRESULT_FROM_WIN32(error));
                }
                winrt::check_bool(ResumeThread(thread.get()) != static_cast<DWORD>(-1));
                watchdog = std::thread([this] {
                    while (WaitForSingleObject(cancellation.get(), 10) == WAIT_TIMEOUT)
                    {
                        const auto deadline = io_deadline.load();
                        if ((external_cancellation && external_cancellation->load()) || (deadline && GetTickCount64() >= deadline))
                        {
                            cancel();
                            break;
                        }
                    }
                });
            }
            protocol::Response exchange(protocol::Command command, std::int64_t position = 0, const std::wstring& path = {})
            {
                if (cancelled.load()) throw winrt::hresult_canceled();
                io_deadline.store(GetTickCount64() + 15000);
                const auto clear_deadline = wil::scope_exit([&] { io_deadline.store(0); });
                protocol::Request request{.command = command, .path_characters = static_cast<std::uint32_t>(path.size()), .position = position};
                DWORD written{};
                winrt::check_bool(WriteFile(request_pipe.get(), &request, sizeof(request), &written, nullptr) && written == sizeof(request));
                if (!path.empty()) winrt::check_bool(WriteFile(request_pipe.get(), path.data(), static_cast<DWORD>(path.size() * sizeof(wchar_t)), &written, nullptr));
                protocol::Response response;
                auto* cursor = reinterpret_cast<unsigned char*>(&response);
                DWORD remaining = sizeof(response);
                while (remaining)
                {
                    if (cancelled.load()) throw winrt::hresult_canceled();
                    DWORD count{};
                    winrt::check_bool(ReadFile(response_pipe.get(), cursor, remaining, &count, nullptr) && count);
                    cursor += count;
                    remaining -= count;
                }
                if (response.error)
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                        L"FFmpeg command " + std::to_wstring(static_cast<unsigned>(command)) +
                        L" failed: " + std::to_wstring(response.error));
                if (response.signature != protocol::magic || response.protocol != protocol::version ||
                    response.bytes > protocol::maximum_sample_bytes)
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                return response;
            }
            std::vector<unsigned char> take_buffer(bool video)
            {
                std::scoped_lock lock(buffer_mutex);
                const unsigned index = video ? 0 : 1;
                auto& pool = buffers[index];
                if (cancelled.load()) throw winrt::hresult_canceled();
                if (!pool.empty()) { auto value = std::move(pool.front()); pool.pop_front(); return value; }
                return {};
            }
            void return_buffer(std::vector<unsigned char> buffer, bool video)
            {
                std::scoped_lock lock(buffer_mutex);
                auto& pool = buffers[video ? 0 : 1];
                if (!cancelled.load() && pool.size() < 4) pool.push_back(std::move(buffer));
            }
            template<typename Request, typename Work>
            void enqueue(Request request, Work work)
            {
                const auto deferral = request.GetDeferral();
                try
                {
                    std::scoped_lock lock(queue_mutex);
                    requests.push_back([this, request, deferral, work] {
                        const auto finish = wil::scope_exit([&] { try { deferral.Complete(); } catch (...) {} });
                        if (!cancelled.load()) work(request);
                    });
                    if (processing) return;
                    processing = true;
                }
                catch (...)
                {
                    try { deferral.Complete(); } catch (...) {}
                    throw;
                }
                drain(shared_from_this());
            }
            static winrt::fire_and_forget drain(std::shared_ptr<Session> self)
            {
                try { co_await winrt::resume_background(); }
                catch (...) { self->fail_request(); }
                for (;;)
                {
                    std::function<void()> work;
                    {
                        std::scoped_lock lock(self->queue_mutex);
                        if (self->requests.empty()) { self->processing = false; co_return; }
                        work = std::move(self->requests.front());
                        self->requests.pop_front();
                    }
                    try { work(); }
                    catch (...) { self->fail_request(); }
                }
            }
            void provide(MediaStreamSourceSampleRequest const& request, bool video)
            {
                auto bytes = take_buffer(video);
                bool borrowed = true;
                const auto return_unused = wil::scope_exit([&] {
                    try { if (borrowed) return_buffer(std::move(bytes), video); } catch (...) {}
                });
                const auto response = exchange(video ? protocol::Command::sample_video : protocol::Command::sample_audio);
                if (response.bytes == 0) { request.Sample(nullptr); return; }
                bytes.resize(response.bytes);
                std::memcpy(bytes.data(), shared, response.bytes);
                next_positions[video ? 0 : 1] = response.timestamp + response.duration;
                const std::weak_ptr<Session> weak = shared_from_this();
                const auto buffer = winrt::make<SampleBuffer>(std::move(bytes), [weak, video](std::vector<unsigned char> value) {
                    if (const auto active = weak.lock()) active->return_buffer(std::move(value), video);
                });
                borrowed = false;
                auto sample = MediaStreamSample::CreateFromBuffer(buffer, TimeSpan{std::max<std::int64_t>(0, response.timestamp)});
                sample.Duration(TimeSpan{std::max<std::int64_t>(1, response.duration)});
                sample.KeyFrame(true);
                request.Sample(sample);
                ++delivered_samples[video ? 0 : 1];
            }
            void fail_request() noexcept
            {
                if (cancelled.load()) return;
                try
                {
                    contracts::log_event(L"Software media request failure: " + std::to_wstring(static_cast<HRESULT>(winrt::to_hresult())));
                    try { throw; }
                    catch (const winrt::hresult_error& error) { contracts::log_event(error.message().c_str()); }
                    catch (...) {}
                }
                catch (...) {}
                if (!cancelled.load())
                {
                    try { stream.NotifyError(MediaStreamSourceErrorStatus::Other); } catch (...) {}
                    cancel();
                }
            }
            void seek(MediaStreamSourceStartingRequest const& request)
            {
                const auto position = request.StartPosition();
                const auto target = position ? position.Value().count() : 0;
                if (position)
                {
                    exchange(protocol::Command::seek, target);
                    next_positions.fill(target);
                }
                const auto next = active_streams[0] && active_streams[1] ?
                    std::min(next_positions[0], next_positions[1]) :
                    next_positions[active_streams[0] ? 0 : 1];
                request.SetActualStartPosition(TimeSpan{position ? target : next});
            }
            void open_file(const std::wstring& path)
            {
                start();
                const auto info = exchange(protocol::Command::open, 0, path);
                VideoStreamDescriptor video{nullptr};
                AudioStreamDescriptor audio{nullptr};
                if (info.width && info.height)
                {
                    auto properties = VideoEncodingProperties::CreateUncompressed(MediaEncodingSubtypes::Nv12(), info.width, info.height);
                    properties.FrameRate().Numerator(info.frame_rate_numerator);
                    properties.FrameRate().Denominator(info.frame_rate_denominator);
                    properties.PixelAspectRatio().Numerator(info.aspect_numerator);
                    properties.PixelAspectRatio().Denominator(info.aspect_denominator);
                    properties.Properties().Insert(MF_MT_VIDEO_ROTATION, winrt::box_value(info.rotation));
                    properties.Properties().Insert(MF_MT_VIDEO_NOMINAL_RANGE, winrt::box_value(static_cast<std::uint32_t>(info.color_range == 2 ? 1 : 2)));
                    properties.Properties().Insert(MF_MT_YUV_MATRIX, winrt::box_value(static_cast<std::uint32_t>(info.color_space == 1 ? 1 : info.color_space == 9 ? 4 : 2)));
                    video = VideoStreamDescriptor(properties);
                }
                if (info.sample_rate && info.channels)
                    audio = AudioStreamDescriptor(AudioEncodingProperties::CreatePcm(info.sample_rate, info.channels, 16));
                if (video && audio) stream = MediaStreamSource(video, audio);
                else if (video) stream = MediaStreamSource(video);
                else if (audio) stream = MediaStreamSource(audio);
                else throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                active_streams = {video != nullptr, audio != nullptr};
                stream.CanSeek(info.duration > 0);
                if (info.duration > 0) stream.Duration(TimeSpan{info.duration});
                stream.BufferTime(std::chrono::milliseconds(100));
                const std::weak_ptr<Session> weak = shared_from_this();
                stream.SampleRequested([weak](auto const&, MediaStreamSourceSampleRequestedEventArgs const& args) {
                    if (const auto self = weak.lock())
                    {
                        try
                        {
                            const auto request = args.Request();
                            const bool video = request.StreamDescriptor().try_as<VideoStreamDescriptor>() != nullptr;
                            self->enqueue(request, [session = self.get(), video](auto const& value) { session->provide(value, video); });
                        }
                        catch (...) { self->fail_request(); }
                    }
                });
                stream.Starting([weak](auto const&, MediaStreamSourceStartingEventArgs const& args) {
                    if (const auto self = weak.lock())
                    {
                        try { self->enqueue(args.Request(), [session = self.get()](auto const& value) { session->seek(value); }); }
                        catch (...) { self->fail_request(); }
                    }
                });
                stream.Closed([weak](auto const&, auto const&) { if (const auto self = weak.lock()) self->cancel(); });
                media = MediaSource::CreateFromMediaStreamSource(stream);
            }
        };
    }

    std::shared_ptr<SoftwareMediaSource> SoftwareMediaSource::open(const std::wstring& path,
        std::shared_ptr<std::atomic_bool> cancellation)
    {
        auto session = std::make_shared<Session>();
        session->external_cancellation = std::move(cancellation);
        session->open_file(path);
        return session;
    }
}
