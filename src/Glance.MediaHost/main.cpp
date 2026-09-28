#include "ffmpeg_api.h"
#include "glance/contracts/software_media_protocol.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    namespace protocol = glance::contracts::software_media;
    constexpr AVRational ticks{1, 10000000};

    bool read_exact(HANDLE handle, void* data, DWORD bytes)
    {
        auto* cursor = static_cast<unsigned char*>(data);
        while (bytes)
        {
            DWORD read{};
            if (!ReadFile(handle, cursor, bytes, &read, nullptr) || !read) return false;
            cursor += read;
            bytes -= read;
        }
        return true;
    }
    bool write_exact(HANDLE handle, const void* data, DWORD bytes)
    {
        const auto* cursor = static_cast<const unsigned char*>(data);
        while (bytes)
        {
            DWORD written{};
            if (!WriteFile(handle, cursor, bytes, &written, nullptr) || !written) return false;
            cursor += written;
            bytes -= written;
        }
        return true;
    }
    void check(int result) { if (result < 0) throw result; }
    std::string utf8(const std::wstring& value)
    {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (!size) throw AVERROR(EINVAL);
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    struct Stream
    {
        FFmpegApi& api;
        AVFormatContext* format{};
        AVIOContext* input{};
        HANDLE file{INVALID_HANDLE_VALUE};
        AVCodecContext* codec{};
        AVPacket* packet{};
        AVFrame* frame{};
        SwsContext* scale{};
        SwrContext* resample{};
        int index{-1};
        AVMediaType type;
        bool draining{};
        std::int64_t target{}, next_timestamp{}, origin{};
        unsigned width{}, height{}, rate{}, channels{};
        int scale_width{}, scale_height{}, scale_format{-1};
        HANDLE cancellation;

        Stream(FFmpegApi& library, AVMediaType media_type, HANDLE stop)
            : api(library), type(media_type), cancellation(stop) {}
        ~Stream()
        {
            if (scale) api.sws_freeContext(scale);
            if (resample) api.swr_free(&resample);
            if (frame) api.av_frame_free(&frame);
            if (packet) api.av_packet_free(&packet);
            if (codec) api.avcodec_free_context(&codec);
            if (format) api.avformat_close_input(&format);
            if (input) { api.av_free(input->buffer); api.avio_context_free(&input); }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
        void open(const std::wstring& path)
        {
            file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw AVERROR(EIO);
            auto* buffer = static_cast<unsigned char*>(api.av_malloc(65536));
            if (!buffer) throw AVERROR(ENOMEM);
            input = api.avio_alloc_context(buffer, 65536, 0, this,
                [](void* context, unsigned char* output, int size) -> int {
                    auto& self = *static_cast<Stream*>(context);
                    if (WaitForSingleObject(self.cancellation, 0) == WAIT_OBJECT_0) return AVERROR_EXIT;
                    DWORD read{};
                    if (!ReadFile(self.file, output, static_cast<DWORD>(size), &read, nullptr)) return AVERROR(EIO);
                    return read ? static_cast<int>(read) : AVERROR_EOF;
                }, nullptr,
                [](void* context, std::int64_t offset, int origin) -> std::int64_t {
                    auto& self = *static_cast<Stream*>(context);
                    LARGE_INTEGER result{};
                    if (origin == AVSEEK_SIZE) return GetFileSizeEx(self.file, &result) ? result.QuadPart : AVERROR(EIO);
                    origin &= ~AVSEEK_FORCE;
                    if (origin < SEEK_SET || origin > SEEK_END) return AVERROR(EINVAL);
                    LARGE_INTEGER distance{};
                    distance.QuadPart = offset;
                    return SetFilePointerEx(self.file, distance, &result, static_cast<DWORD>(origin)) ? result.QuadPart : AVERROR(EIO);
                });
            if (!input) { api.av_free(buffer); throw AVERROR(ENOMEM); }
            format = api.avformat_alloc_context();
            if (!format) throw AVERROR(ENOMEM);
            format->pb = input;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
            format->interrupt_callback = {[](void* value) -> int {
                return WaitForSingleObject(static_cast<Stream*>(value)->cancellation, 0) == WAIT_OBJECT_0;
            }, this};
            check(api.avformat_open_input(&format, utf8(path).c_str(), nullptr, nullptr));
            check(api.avformat_find_stream_info(format, nullptr));
            index = api.av_find_best_stream(format, type, -1, -1, nullptr, 0);
            if (index == AVERROR_STREAM_NOT_FOUND) return;
            check(index);
            const auto* parameters = format->streams[index]->codecpar;
            const auto* decoder = api.avcodec_find_decoder(parameters->codec_id);
            if (!decoder) throw AVERROR_DECODER_NOT_FOUND;
            codec = api.avcodec_alloc_context3(decoder);
            if (!codec) throw AVERROR(ENOMEM);
            check(api.avcodec_parameters_to_context(codec, parameters));
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            codec->thread_count = type == AVMEDIA_TYPE_VIDEO ? static_cast<int>(std::clamp(system.dwNumberOfProcessors / 2, 1UL, 8UL)) : 1;
            codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            check(api.avcodec_open2(codec, decoder, nullptr));
            packet = api.av_packet_alloc();
            frame = api.av_frame_alloc();
            if (!packet || !frame) throw AVERROR(ENOMEM);
            origin = format->start_time == AV_NOPTS_VALUE ? 0 : api.av_rescale_q(format->start_time, AVRational{1, AV_TIME_BASE}, ticks);
            if (type == AVMEDIA_TYPE_VIDEO)
            {
                if (codec->width <= 0 || codec->height <= 0 || codec->width > 8192 || codec->height > 8192) throw AVERROR(EINVAL);
                width = (static_cast<unsigned>(codec->width) + 1) & ~1U;
                height = (static_cast<unsigned>(codec->height) + 1) & ~1U;
            }
            else
            {
                rate = 48000;
                channels = codec->ch_layout.nb_channels == 1 ? 1 : 2;
                AVChannelLayout layout{};
                api.av_channel_layout_default(&layout, static_cast<int>(channels));
                const auto result = api.swr_alloc_set_opts2(&resample, &layout, AV_SAMPLE_FMT_S16, static_cast<int>(rate),
                    &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
                api.av_channel_layout_uninit(&layout);
                check(result);
                check(api.swr_init(resample));
            }
        }
        void seek(std::int64_t position)
        {
            if (!codec) return;
            const auto timestamp = api.av_rescale_q(position + origin, ticks, format->streams[index]->time_base);
            check(api.avformat_seek_file(format, index, std::numeric_limits<std::int64_t>::min(), timestamp, timestamp, 0));
            api.avcodec_flush_buffers(codec);
            api.av_packet_unref(packet);
            api.av_frame_unref(frame);
            if (resample) check(api.swr_init(resample));
            draining = false;
            target = position;
            next_timestamp = position;
        }
        bool decode()
        {
            if (!codec) return false;
            for (;;)
            {
                if (WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0) throw AVERROR_EXIT;
                const auto result = api.avcodec_receive_frame(codec, frame);
                if (result == 0) return true;
                if (result == AVERROR_EOF) return false;
                if (result != AVERROR(EAGAIN)) check(result);
                if (draining) return false;
                int read{};
                do
                {
                    api.av_packet_unref(packet);
                    read = api.av_read_frame(format, packet);
                } while (read >= 0 && packet->stream_index != index);
                if (read < 0)
                {
                    if (read != AVERROR_EOF) check(read);
                    draining = true;
                    check(api.avcodec_send_packet(codec, nullptr));
                }
                else check(api.avcodec_send_packet(codec, packet));
            }
        }
        protocol::Response sample(unsigned char* memory)
        {
            while (decode())
            {
                protocol::Response result;
                const auto time_base = format->streams[index]->time_base;
                result.timestamp = frame->best_effort_timestamp == AV_NOPTS_VALUE ? next_timestamp :
                    api.av_rescale_q(frame->best_effort_timestamp, time_base, ticks) - origin;
                if (type == AVMEDIA_TYPE_VIDEO)
                {
                    if (result.timestamp < target) { api.av_frame_unref(frame); continue; }
                    const auto frame_rate = format->streams[index]->avg_frame_rate;
                    result.duration = frame->duration > 0 ? api.av_rescale_q(frame->duration, time_base, ticks) :
                        frame_rate.num > 0 ? api.av_rescale_q(1, AVRational{frame_rate.den, frame_rate.num}, ticks) : 333333;
                    result.bytes = width * height * 3 / 2;
                    if (!VirtualAlloc(memory, result.bytes, MEM_COMMIT, PAGE_READWRITE)) throw AVERROR(ENOMEM);
                    const bool configure_scale = !scale || scale_width != frame->width || scale_height != frame->height || scale_format != frame->format;
                    if (configure_scale)
                    {
                        if (scale) api.sws_freeContext(scale);
                        scale = api.sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                            static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        if (!scale) throw AVERROR(ENOMEM);
                        const int space = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
                            frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
                        const int range = frame->color_range == AVCOL_RANGE_JPEG;
                        const auto coefficients = api.sws_getCoefficients(space);
                        check(api.sws_setColorspaceDetails(scale, coefficients, range, coefficients, range, 0, 1 << 16, 1 << 16));
                        scale_width = frame->width;
                        scale_height = frame->height;
                        scale_format = frame->format;
                    }
                    std::array<unsigned char*, 4> planes{memory, memory + width * height, nullptr, nullptr};
                    std::array<int, 4> strides{static_cast<int>(width), static_cast<int>(width), 0, 0};
                    check(api.sws_scale(scale, frame->data, frame->linesize, 0, frame->height, planes.data(), strides.data()));
                }
                else
                {
                    const auto delay = api.swr_get_delay(resample, codec->sample_rate);
                    result.timestamp -= api.av_rescale_q(delay, AVRational{1, codec->sample_rate}, ticks);
                    const auto samples = api.av_rescale_rnd(delay + frame->nb_samples,
                        rate, codec->sample_rate, AV_ROUND_UP);
                    if (samples <= 0 || samples > protocol::maximum_sample_bytes / (channels * 2)) throw AVERROR(EINVAL);
                    if (!VirtualAlloc(memory, static_cast<SIZE_T>(samples * channels * 2), MEM_COMMIT, PAGE_READWRITE)) throw AVERROR(ENOMEM);
                    auto* output = memory;
                    const auto count = api.swr_convert(resample, &output, static_cast<int>(samples),
                        const_cast<const unsigned char**>(frame->extended_data), frame->nb_samples);
                    check(count);
                    result.bytes = static_cast<unsigned>(count) * channels * 2;
                    result.duration = api.av_rescale_q(count, AVRational{1, static_cast<int>(rate)}, ticks);
                    if (result.timestamp + result.duration <= target) { api.av_frame_unref(frame); continue; }
                    if (result.timestamp < target)
                    {
                        const auto skip = std::min<std::int64_t>(count, api.av_rescale_rnd(target - result.timestamp, rate, 10000000, AV_ROUND_UP));
                        const auto bytes = static_cast<unsigned>(skip) * channels * 2;
                        result.bytes -= bytes;
                        std::memmove(memory, memory + bytes, result.bytes);
                        result.timestamp += api.av_rescale_q(skip, AVRational{1, static_cast<int>(rate)}, ticks);
                        result.duration = api.av_rescale_q(count - skip, AVRational{1, static_cast<int>(rate)}, ticks);
                    }
                }
                next_timestamp = result.timestamp + result.duration;
                api.av_frame_unref(frame);
                if (result.bytes) return result;
            }
            if (resample)
            {
                constexpr int capacity = 4096;
                if (!VirtualAlloc(memory, capacity * channels * 2, MEM_COMMIT, PAGE_READWRITE)) throw AVERROR(ENOMEM);
                auto* output = memory;
                const auto count = api.swr_convert(resample, &output, capacity, nullptr, 0);
                check(count);
                protocol::Response result;
                result.bytes = static_cast<unsigned>(count) * channels * 2;
                result.timestamp = next_timestamp;
                result.duration = api.av_rescale_q(count, AVRational{1, static_cast<int>(rate)}, ticks);
                next_timestamp += result.duration;
                return result;
            }
            return {};
        }
    };

    int run(HANDLE request_pipe, HANDLE response_pipe, HANDLE mapping, HANDLE cancellation,
        const std::filesystem::path& directory)
    {
        FFmpegApi api(directory);
        auto* memory = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, protocol::maximum_sample_bytes));
        if (!memory) return 3;
        const std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(memory, UnmapViewOfFile);
        Stream video(api, AVMEDIA_TYPE_VIDEO, cancellation), audio(api, AVMEDIA_TYPE_AUDIO, cancellation);
        for (;;)
        {
            protocol::Request request;
            if (!read_exact(request_pipe, &request, sizeof(request)) || request.signature != protocol::magic ||
                request.protocol != protocol::version || request.path_characters > 32767) return 4;
            protocol::Response result;
            try
            {
                if (request.command == protocol::Command::close) return 0;
                if (request.command == protocol::Command::open)
                {
                    if (video.format || audio.format || !request.path_characters) throw AVERROR(EINVAL);
                    std::wstring path(request.path_characters, L'\0');
                    if (!read_exact(request_pipe, path.data(), request.path_characters * sizeof(wchar_t))) return 5;
                    video.open(path);
                    audio.open(path);
                    if (!video.codec && !audio.codec) throw AVERROR_STREAM_NOT_FOUND;
                    result.width = video.width;
                    result.height = video.height;
                    result.sample_rate = audio.rate;
                    result.channels = audio.channels;
                    auto* format = video.codec ? video.format : audio.format;
                    result.duration = format->duration > 0 ? api.av_rescale_q(format->duration, AVRational{1, AV_TIME_BASE}, ticks) : 0;
                    if (video.codec)
                    {
                        const auto* stream = video.format->streams[video.index];
                        result.frame_rate_numerator = static_cast<unsigned>(std::max(1, stream->avg_frame_rate.num));
                        result.frame_rate_denominator = static_cast<unsigned>(std::max(1, stream->avg_frame_rate.den));
                        const auto aspect = stream->sample_aspect_ratio;
                        result.aspect_numerator = static_cast<unsigned>(std::max(1, aspect.num));
                        result.aspect_denominator = static_cast<unsigned>(std::max(1, aspect.den));
                        result.color_space = static_cast<unsigned>(stream->codecpar->color_space);
                        result.color_range = static_cast<unsigned>(stream->codecpar->color_range);
                        for (int i = 0; i < stream->codecpar->nb_coded_side_data; ++i)
                        {
                            const auto& data = stream->codecpar->coded_side_data[i];
                            if (data.type == AV_PKT_DATA_DISPLAYMATRIX && data.size >= 9 * sizeof(std::int32_t))
                            {
                                const auto angle = api.av_display_rotation_get(reinterpret_cast<const std::int32_t*>(data.data));
                                if (std::isfinite(angle)) result.rotation = static_cast<unsigned>((static_cast<int>(std::lround(-angle)) % 360 + 360) % 360);
                            }
                        }
                    }
                }
                else if (request.command == protocol::Command::sample_video) result = video.sample(memory);
                else if (request.command == protocol::Command::sample_audio) result = audio.sample(memory);
                else if (request.command == protocol::Command::seek) { video.seek(request.position); audio.seek(request.position); }
                else throw AVERROR(EINVAL);
            }
            catch (int error) { result.error = error; }
            catch (...) { result.error = AVERROR_UNKNOWN; }
            if (!write_exact(response_pipe, &result, sizeof(result))) return 6;
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{};
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 1;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> owner(arguments, LocalFree);
    if (count != 6) return 2;
    std::array<HANDLE, 4> handles{};
    for (std::size_t i = 0; i < handles.size(); ++i)
    {
        wchar_t* end{};
        handles[i] = reinterpret_cast<HANDLE>(wcstoull(arguments[i + 1], &end, 10));
        if (!handles[i] || end == arguments[i + 1] || *end) return 2;
    }
    try { return run(handles[0], handles[1], handles[2], handles[3], arguments[5]); }
    catch (...) { return 7; }
}
