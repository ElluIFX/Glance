#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace glance::app
{
    struct ShellIconBitmap
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> pixels;
    };

    using ShellIconBitmapPtr = std::shared_ptr<const ShellIconBitmap>;

    [[nodiscard]] ShellIconBitmapPtr load_shell_icon(
        std::wstring_view path,
        bool is_folder,
        std::uint32_t pixel_size,
        bool use_file_attributes = false) noexcept;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Media::ImageSource create_shell_icon_source(
        const ShellIconBitmap& bitmap);
}
