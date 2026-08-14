#pragma once

#include "glance/contracts/component_api.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace glance::components::media_info
{
    [[nodiscard]] std::wstring localize_text(
        std::wstring_view key,
        const wchar_t* language_tag) noexcept;
    [[nodiscard]] std::filesystem::path find_ffprobe() noexcept;
    [[nodiscard]] bool validate_ffprobe(const std::filesystem::path& path) noexcept;
    [[nodiscard]] std::wstring query_media_info(
        const std::filesystem::path& ffprobe,
        std::wstring_view path,
        const wchar_t* language_tag,
        const glance::contracts::components::HoverInfoTextSink& sink) noexcept;
    [[nodiscard]] std::wstring query_media_json(
        const std::filesystem::path& ffprobe,
        std::wstring_view path,
        const glance::contracts::components::HoverInfoTextSink& sink) noexcept;
    [[nodiscard]] bool install_ffprobe(
        const std::filesystem::path& archive,
        const std::filesystem::path& storage,
        std::wstring& error_key) noexcept;
}
