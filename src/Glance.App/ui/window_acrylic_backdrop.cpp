#include "pch.h"
#include "window_acrylic_backdrop.h"

#include "appearance_preferences.h"

#include <algorithm>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Backdrops = Microsoft::UI::Composition::SystemBackdrops;

namespace
{
    void apply_acrylic_material(
        const Backdrops::DesktopAcrylicController& controller,
        const Backdrops::SystemBackdropConfiguration& configuration,
        bool dark,
        std::uint32_t opacity_percent)
    {
        const float strength =
            static_cast<float>(std::clamp(opacity_percent, 10U, 100U) - 10U) /
            90.0F;
        const auto interpolate = [strength](float minimum, float maximum) {
            return minimum + (maximum - minimum) * strength;
        };
        configuration.Theme(dark
                ? Backdrops::SystemBackdropTheme::Dark
                : Backdrops::SystemBackdropTheme::Light);
        controller.TintColor(dark
                ? Windows::UI::Color{ 255, 0x20, 0x20, 0x20 }
                : Windows::UI::Color{ 255, 0xF3, 0xF3, 0xF3 });
        controller.FallbackColor(dark
                ? Windows::UI::Color{ 255, 0x1C, 0x1C, 0x1C }
                : Windows::UI::Color{ 255, 0xEE, 0xEE, 0xEE });
        controller.TintOpacity(interpolate(
            dark ? 0.04F : 0.0F,
            dark ? 0.40F : 0.10F));
        controller.LuminosityOpacity(interpolate(
            dark ? 0.68F : 0.60F,
            dark ? 0.92F : 0.86F));
    }
}

namespace glance::app
{
    std::unique_ptr<WindowAcrylicBackdrop> WindowAcrylicBackdrop::create(
        const Window& window,
        const FrameworkElement& root,
        bool input_active,
        std::uint32_t opacity_percent) noexcept
    {
        if (!acrylic_material_supported())
        {
            return {};
        }

        try
        {
            auto backdrop = std::unique_ptr<WindowAcrylicBackdrop>(
                new WindowAcrylicBackdrop());
            return backdrop->initialize(
                window,
                root,
                input_active,
                opacity_percent)
                ? std::move(backdrop)
                : nullptr;
        }
        catch (...)
        {
            return {};
        }
    }

    WindowAcrylicBackdrop::~WindowAcrylicBackdrop()
    {
        try
        {
            if (root_ != nullptr && theme_changed_token_.value != 0)
            {
                root_.ActualThemeChanged(theme_changed_token_);
            }
            if (controller_ != nullptr)
            {
                controller_.RemoveAllSystemBackdropTargets();
                controller_.Close();
            }
        }
        catch (...)
        {
        }
    }

    bool WindowAcrylicBackdrop::initialize(
        const Window& window,
        const FrameworkElement& root,
        bool input_active,
        std::uint32_t opacity_percent)
    {
        target_ = window.try_as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
        if (target_ == nullptr)
        {
            return false;
        }

        root_ = root;
        opacity_percent_ = std::clamp(opacity_percent, 10U, 100U);
        configuration_ = Backdrops::SystemBackdropConfiguration();
        configuration_.IsInputActive(input_active);
        controller_ = Backdrops::DesktopAcrylicController();
        controller_.Kind(Backdrops::DesktopAcrylicKind::Thin);
        controller_.SetSystemBackdropConfiguration(configuration_);
        if (!controller_.AddSystemBackdropTarget(target_))
        {
            controller_.Close();
            controller_ = nullptr;
            return false;
        }

        apply_material();
        theme_changed_token_ = root_.ActualThemeChanged(
            [this](auto const&, auto const&) {
                apply_material();
            });
        return true;
    }

    void WindowAcrylicBackdrop::set_input_active(bool active) noexcept
    {
        try
        {
            if (configuration_ != nullptr)
            {
                configuration_.IsInputActive(active);
            }
        }
        catch (...)
        {
        }
    }

    void WindowAcrylicBackdrop::set_opacity(std::uint32_t opacity_percent) noexcept
    {
        opacity_percent_ = std::clamp(opacity_percent, 10U, 100U);
        apply_material();
    }

    void WindowAcrylicBackdrop::apply_material() noexcept
    {
        if (controller_ == nullptr || configuration_ == nullptr || root_ == nullptr)
        {
            return;
        }

        try
        {
            const bool dark = root_.ActualTheme() == ElementTheme::Dark;
            apply_acrylic_material(
                controller_,
                configuration_,
                dark,
                opacity_percent_);
        }
        catch (...)
        {
        }
    }

}
