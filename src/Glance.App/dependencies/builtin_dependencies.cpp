#include "pch.h"
#include "dependency_service.h"

namespace glance::app::dependencies
{
    void register_media_dependency()
    {
        namespace api = contracts::dependencies;
        constexpr api::File files[]{
            {L"ffmpeg-8.1.2-full_build-shared/bin/avutil-60.dll", L"bin/avutil-60.dll", L"6f172b5d10224fcc3f729c8baa6fd36a97bb58042bb3b1417078d77d2da59b87"},
            {L"ffmpeg-8.1.2-full_build-shared/bin/swresample-6.dll", L"bin/swresample-6.dll", L"72e2721672c11fd37d983b05cc2370f612784e4e3218362a0cb4315d08c917fb"},
            {L"ffmpeg-8.1.2-full_build-shared/bin/avcodec-62.dll", L"bin/avcodec-62.dll", L"34f5b1baac01c4be3edf464309c79db05ffbd4a9c905c94b4a4651cd15370296"},
            {L"ffmpeg-8.1.2-full_build-shared/bin/avformat-62.dll", L"bin/avformat-62.dll", L"c04e6ed2f9f36d42325d4f4df5babb5d6ce7c55dbffeb7ef1007e25e97bcb716"},
            {L"ffmpeg-8.1.2-full_build-shared/bin/swscale-9.dll", L"bin/swscale-9.dll", L"3d07972cada6ba38c492e92b0f6c025a6835607fe00cdf83afc604c2fbdfe550"},
            {L"ffmpeg-8.1.2-full_build-shared/LICENSE", L"LICENSE", L"8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903"}};
        constexpr api::Entry entries[]{
            {L"avutil", api::EntryKind::library, L"bin/avutil-60.dll"},
            {L"swresample", api::EntryKind::library, L"bin/swresample-6.dll"},
            {L"avcodec", api::EntryKind::library, L"bin/avcodec-62.dll"},
            {L"avformat", api::EntryKind::library, L"bin/avformat-62.dll"},
            {L"swscale", api::EntryKind::library, L"bin/swscale-9.dll"}};
        const api::Declaration declaration{
            .id = L"ffmpeg", .version = L"8.1.2", .display_name = L"FFmpeg",
            .url = L"https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-full_build-shared.7z",
            .archive_name = L"ffmpeg-8.1.2-full_build-shared.7z",
            .sha256 = L"cba748035c21ce1431d0823c7a3a711f38616f89f87a265dceddf9b7f6749d2d",
            .archive_size = 59459100, .files = files, .file_count = static_cast<std::uint32_t>(std::size(files)),
            .entries = entries, .entry_count = static_cast<std::uint32_t>(std::size(entries)),
            .description_key = L"DependencyMediaDescription"};
        winrt::check_hresult(host_api().register_dependency(&declaration, L"video-preview"));
        winrt::check_hresult(host_api().register_dependency(&declaration, L"audio-preview"));
    }
}
