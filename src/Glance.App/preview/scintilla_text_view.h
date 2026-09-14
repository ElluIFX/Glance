#pragma once

#include "text_preferences.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace glance::app
{
    class ScintillaTextView
    {
    public:
        using NearEndCallback = std::function<void()>;
        using FontZoomCallback = std::function<void(int)>;
        using DoubleClickCallback = std::function<bool()>;

        ScintillaTextView(
            HWND parent,
            NearEndCallback near_end_callback,
            FontZoomCallback font_zoom_callback,
            DoubleClickCallback double_click_callback);
        ~ScintillaTextView();

        ScintillaTextView(const ScintillaTextView&) = delete;
        ScintillaTextView& operator=(const ScintillaTextView&) = delete;

        [[nodiscard]] bool available() const noexcept;
        [[nodiscard]] std::wstring error() const;
        [[nodiscard]] bool should_load_more() const noexcept;

        void set_bounds(int x, int y, int width, int height) noexcept;
        void set_occlusions(std::span<const RECT> rectangles) noexcept;
        void set_visible(bool visible) noexcept;
        void clear() noexcept;
        void append_text(std::wstring_view text);
        void refresh_text(std::wstring_view text, bool replace, bool follow, bool has_more);
        void set_file_path(std::wstring_view path);
        void set_preferences(
            const TextPreferences& preferences,
            bool syntax_highlighting,
            bool dark);
        void set_word_wrap(bool enabled) noexcept;
        void set_line_numbers(bool enabled) noexcept;
        void set_syntax_highlighting(bool enabled);

    public:
        static LRESULT CALLBACK host_window_proc(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam) noexcept;
        static LRESULT CALLBACK editor_subclass(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam,
            UINT_PTR subclass_id,
            DWORD_PTR reference_data) noexcept;

    private:
        LRESULT call(
            UINT message,
            WPARAM wparam = 0,
            LPARAM lparam = 0) const noexcept;
        void configure();
        void update_lexer();
        void apply_theme(bool dark);
        void apply_lexer_styles();
        void update_line_number_width() noexcept;
        void request_near_end_check() noexcept;
        void handle_notification(const NMHDR& header) noexcept;
        void update_copy_shortcut() noexcept;

        HWND parent_{};
        HWND host_{};
        HWND editor_{};
        NearEndCallback near_end_callback_;
        FontZoomCallback font_zoom_callback_;
        DoubleClickCallback double_click_callback_;
        std::wstring error_;
        std::wstring path_;
        std::string lexer_name_;
        std::array<std::string, 3> lexer_keywords_;
        TextPreferences preferences_{};
        bool syntax_highlighting_{ true };
        bool dark_{};
        bool native_theme_initialized_{};
        bool visible_{};
        bool near_end_check_pending_{};
        int wheel_delta_{};
        struct RefreshPosition
        {
            LRESULT line{};
            LRESULT subline{};
            LRESULT x{};
            LRESULT anchor{};
            LRESULT caret{};
        };
        std::optional<RefreshPosition> refresh_position_;
    };
}
