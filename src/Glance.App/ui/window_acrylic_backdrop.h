#pragma once

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <memory>

namespace glance::app
{
    class WindowAcrylicBackdrop final
    {
    public:
        static std::unique_ptr<WindowAcrylicBackdrop> create(
            const winrt::Microsoft::UI::Xaml::Window& window,
            const winrt::Microsoft::UI::Xaml::FrameworkElement& root,
            bool input_active,
            std::uint32_t opacity_percent) noexcept;

        ~WindowAcrylicBackdrop();

        WindowAcrylicBackdrop(const WindowAcrylicBackdrop&) = delete;
        WindowAcrylicBackdrop& operator=(const WindowAcrylicBackdrop&) = delete;

        void set_input_active(bool active) noexcept;
        void set_opacity(std::uint32_t opacity_percent) noexcept;

    private:
        WindowAcrylicBackdrop() = default;
        bool initialize(
            const winrt::Microsoft::UI::Xaml::Window& window,
            const winrt::Microsoft::UI::Xaml::FrameworkElement& root,
            bool input_active,
            std::uint32_t opacity_percent);
        void apply_material() noexcept;

        winrt::Microsoft::UI::Xaml::FrameworkElement root_{ nullptr };
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop target_{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController controller_{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration configuration_{ nullptr };
        winrt::event_token theme_changed_token_{};
        std::uint32_t opacity_percent_{ 100 };
    };

}
