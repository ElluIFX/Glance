#pragma once
#include "glance/contracts/dependency_runtime.h"
#pragma warning(push, 0)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#pragma warning(pop)
#include <stdexcept>

#define GLANCE_FFMPEG_FUNCTIONS(X) \
    X(util, av_frame_alloc) X(util, av_frame_free) X(util, av_frame_unref) \
    X(util, av_rescale_q) X(util, av_rescale_rnd) X(util, av_channel_layout_default) \
    X(util, av_channel_layout_uninit) X(util, av_display_rotation_get) X(util, av_malloc) X(util, av_free) \
    X(codec, avcodec_find_decoder) X(codec, avcodec_alloc_context3) \
    X(codec, avcodec_parameters_to_context) X(codec, avcodec_open2) X(codec, avcodec_free_context) \
    X(codec, avcodec_send_packet) X(codec, avcodec_receive_frame) X(codec, avcodec_flush_buffers) \
    X(codec, av_packet_alloc) X(codec, av_packet_free) X(codec, av_packet_unref) \
    X(format, avformat_alloc_context) X(format, avformat_open_input) X(format, avformat_find_stream_info) \
    X(format, avformat_close_input) X(format, av_find_best_stream) X(format, av_read_frame) \
    X(format, avformat_seek_file) X(format, avio_alloc_context) X(format, avio_context_free) \
    X(scale, sws_getContext) X(scale, sws_freeContext) X(scale, sws_scale) \
    X(scale, sws_getCoefficients) X(scale, sws_setColorspaceDetails) \
    X(resample, swr_alloc_set_opts2) X(resample, swr_init) X(resample, swr_free) \
    X(resample, swr_convert) X(resample, swr_get_delay)

struct FFmpegApi
{
    using Library = glance::contracts::dependencies::Library;
    Library util, resample, codec, format, scale;
#define DECLARE(owner, name) decltype(&::name) name{};
    GLANCE_FFMPEG_FUNCTIONS(DECLARE)
#undef DECLARE
    explicit FFmpegApi(const std::filesystem::path& directory)
        : util(directory / L"avutil-60.dll"), resample(directory / L"swresample-6.dll"),
          codec(directory / L"avcodec-62.dll"), format(directory / L"avformat-62.dll"),
          scale(directory / L"swscale-9.dll")
    {
#define RESOLVE(owner, name) name = reinterpret_cast<decltype(name)>(owner.symbol(#name)); if (!name) throw std::runtime_error("Missing FFmpeg export: " #name);
        GLANCE_FFMPEG_FUNCTIONS(RESOLVE)
#undef RESOLVE
    }
};
#undef GLANCE_FFMPEG_FUNCTIONS
