#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace glance::app
{
    struct WindowPreferences
    {
        std::uint32_t default_width{ 720 };
        std::uint32_t default_height{ 520 };
        bool remember_size{ true };
        bool auto_fit_media{ true };
        bool pause_auto_fit_when_topmost{ true };
        bool pause_auto_fit_in_gallery{};
        bool show_after_auto_fit{};
        bool dynamic_auto_fit{};
        std::uint32_t adaptive_minimum_percent{ 40 };
        std::uint32_t adaptive_maximum_percent{ 75 };
        std::wstring auto_fit_ignored_extensions;
        bool remember_position{};
        bool double_click_fullscreen{};
        bool right_click_close{};
    };

    [[nodiscard]] WindowPreferences load_window_preferences() noexcept;
    void save_window_preferences(const WindowPreferences& preferences) noexcept;
    struct FileListPreferences
    {
        std::uint32_t width{ 220 };
    };
    [[nodiscard]] FileListPreferences load_file_list_preferences() noexcept;
    void save_file_list_preferences(const FileListPreferences& preferences) noexcept;
    [[nodiscard]] bool auto_fit_ignores_path(
        const WindowPreferences& preferences,
        std::wstring_view path) noexcept;
}
