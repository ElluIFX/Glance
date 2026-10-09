#include "pch.h"
#include "MainWindow.xaml.h"
#include "localization.h"
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <cmath>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;

namespace winrt::Glance::App::implementation
{
    void MainWindow::initialize_gallery_controls()
    {
        const auto weak = get_weak();
        GalleryPreviousButton().Click([weak](auto const&, auto const&) {
            if (const auto self = weak.get()) self->navigate_gallery(-1);
        });
        GalleryNextButton().Click([weak](auto const&, auto const&) {
            if (const auto self = weak.get()) self->navigate_gallery(1);
        });
        for (FrameworkElement const& element : {GalleryPreviousButton().as<FrameworkElement>(),
            GalleryNextButton().as<FrameworkElement>(), GalleryProgressPanel().as<FrameworkElement>()})
        {
            element.PointerEntered([weak](IInspectable const& sender, auto const&) {
                if (const auto self = weak.get()) self->emphasize_gallery_control(sender.as<FrameworkElement>(), true);
            });
            element.PointerExited([weak](IInspectable const& sender, auto const&) {
                if (const auto self = weak.get()) self->emphasize_gallery_control(sender.as<FrameworkElement>(), false);
            });
            element.GotFocus([weak](IInspectable const& sender, auto const&) {
                if (const auto self = weak.get()) self->emphasize_gallery_control(sender.as<FrameworkElement>(), true);
            });
            element.LostFocus([weak](IInspectable const& sender, auto const&) {
                if (const auto self = weak.get()) self->emphasize_gallery_control(sender.as<FrameworkElement>(), false);
            });
            element.SizeChanged([weak](auto const&, auto const&) {
                if (const auto self = weak.get()) self->update_gallery_popup_layout();
            });
        }
        PreviewCanvas().SizeChanged([weak](auto const&, auto const&) {
            if (const auto self = weak.get()) self->update_gallery_popup_layout();
        });
        GalleryPositionSlider().AddHandler(UIElement::PointerPressedEvent(), box_value<PointerEventHandler>(
            [weak](auto const&, PointerRoutedEventArgs const& args) {
                if (const auto self = weak.get(); self && args.GetCurrentPoint(self->GalleryPositionSlider()).Properties().IsLeftButtonPressed())
                {
                    auto source = args.OriginalSource().try_as<DependencyObject>();
                    while (source && source != self->GalleryPositionSlider())
                    {
                        if (source.try_as<Primitives::Thumb>()) { self->gallery_slider_dragging_ = true; break; }
                        source = Media::VisualTreeHelper::GetParent(source);
                    }
                }
            }), true);
        for (const auto& event : {UIElement::PointerReleasedEvent(), UIElement::PointerCaptureLostEvent()})
            GalleryPositionSlider().AddHandler(event, box_value<PointerEventHandler>([weak](auto const&, auto const&) {
                if (const auto self = weak.get()) self->finish_gallery_slider_drag();
            }), true);
        GalleryPositionSlider().ValueChanged([weak](auto const&, Primitives::RangeBaseValueChangedEventArgs const& args) {
            if (const auto self = weak.get(); self && !self->gallery_slider_updating_ &&
                self->gallery_mode_ == GalleryMode::active && self->gallery_total_known_)
            {
                self->gallery_slider_target_ = static_cast<std::uint32_t>(std::llround(args.NewValue()));
                self->update_gallery_position_text();
                if (!self->gallery_slider_dragging_) self->commit_gallery_slider();
            }
        });
    }

    void MainWindow::emphasize_gallery_control(FrameworkElement const& element, bool emphasized)
    {
        const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(element);
        const auto opacity = emphasized ? 1.0F : 0.6F;
        const auto initial = visual.Opacity();
        visual.StopAnimation(L"Opacity");
        if (!Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) { visual.Opacity(opacity); return; }
        auto animation = visual.Compositor().CreateScalarKeyFrameAnimation();
        animation.Duration(std::chrono::milliseconds(100));
        animation.InsertKeyFrame(0.0F, initial);
        animation.InsertKeyFrame(1.0F, opacity);
        visual.StartAnimation(L"Opacity", animation);
    }

    void MainWindow::update_gallery_controls()
    {
        const bool active = gallery_mode_ == GalleryMode::active && gallery_total_count_ != 0;
        if (active && !gallery_popups_[0])
        {
            const std::array<FrameworkElement, 3> controls{GalleryPreviousButton(), GalleryNextButton(), GalleryProgressPanel()};
            for (std::size_t index = 0; index < controls.size(); ++index)
            {
                auto popup = Controls::Primitives::Popup();
                popup.XamlRoot(RootGrid().XamlRoot());
                popup.ShouldConstrainToRootBounds(false);
                GalleryControlStorage().Children().RemoveAt(0);
                popup.Child(controls[index]);
                RootGrid().Children().Append(popup);
                gallery_popups_[index] = std::move(popup);
            }
        }
        for (FrameworkElement const& element : {GalleryPreviousButton().as<FrameworkElement>(),
            GalleryNextButton().as<FrameworkElement>(), GalleryProgressPanel().as<FrameworkElement>()})
            element.Visibility(active ? Visibility::Visible : Visibility::Collapsed);
        const bool can_navigate = active && (!gallery_total_known_ || gallery_total_count_ > 1);
        GalleryPreviousButton().IsEnabled(can_navigate &&
            (!gallery_total_known_ || loop_gallery_enabled_ || gallery_desired_index_ > 0));
        GalleryNextButton().IsEnabled(can_navigate &&
            (!gallery_total_known_ || loop_gallery_enabled_ || gallery_desired_index_ + 1 < gallery_total_count_));
        ToolTipService::SetToolTip(GalleryPreviousButton(), box_value(glance::app::localize(L"GalleryPrevious")));
        ToolTipService::SetToolTip(GalleryNextButton(), box_value(glance::app::localize(L"GalleryNext")));
        gallery_slider_updating_ = true;
        GalleryPositionSlider().Maximum(active ? static_cast<double>(gallery_total_count_ - 1) : 1.0);
        if (!gallery_slider_dragging_ && !gallery_slider_target_) GalleryPositionSlider().Value(gallery_desired_index_);
        GalleryPositionSlider().IsEnabled(active && gallery_total_count_ > 1);
        GalleryPositionSlider().Visibility(active && gallery_total_known_ ? Visibility::Visible : Visibility::Collapsed);
        gallery_slider_updating_ = false;
        update_gallery_position_text();
        update_gallery_popup_layout();
        for (auto popup : gallery_popups_)
            if (popup) popup.IsOpen(active && visible_ && !xaml_modal_overlay_active_);
    }

    void MainWindow::update_gallery_popup_layout()
    {
        if (!gallery_popups_[2]) return;
        const auto canvas = PreviewCanvas();
        const auto origin = canvas.TransformToVisual(RootGrid()).TransformPoint({0, 0});
        const double width = canvas.ActualWidth();
        const double height = canvas.ActualHeight();
        gallery_popups_[0].HorizontalOffset(origin.X + 12.0);
        gallery_popups_[1].HorizontalOffset(origin.X + std::max(12.0, width - 52.0));
        for (auto popup : {gallery_popups_[0], gallery_popups_[1]})
            popup.VerticalOffset(origin.Y + std::max(0.0, (height - 48.0) / 2.0));
        const double progress_width = std::clamp(width - 128.0, 124.0, 420.0);
        GalleryProgressPanel().Width(progress_width);
        gallery_popups_[2].HorizontalOffset(origin.X + (width - progress_width) / 2.0);
        const double bottom = MediaPanel().Visibility() == Visibility::Visible ? 76.0 : 12.0;
        gallery_popups_[2].VerticalOffset(origin.Y + std::max(0.0,
            height - GalleryProgressPanel().ActualHeight() - bottom));
    }

    void MainWindow::update_gallery_position_text()
    {
        const auto index = gallery_slider_target_.value_or(gallery_desired_index_);
        GalleryPositionText().Text(glance::app::localize_format(gallery_total_known_ ? L"GalleryPosition" : L"GalleryUnknownPosition",
            {std::to_wstring(index + 1), std::to_wstring(gallery_total_count_)}));
        const auto item = gallery_items_.find(index);
        GalleryFileNameText().Text(item != gallery_items_.end() ? item->second.display_name :
            index == gallery_current_index_ && current_index_ < files_.size() ? files_[current_index_].display_name : L"");
    }

    void MainWindow::commit_gallery_slider()
    {
        if (!gallery_slider_target_ || gallery_mode_ != GalleryMode::active || !gallery_total_known_ || gallery_total_count_ == 0) return;
        const auto target = std::min(*std::exchange(gallery_slider_target_, std::nullopt), gallery_total_count_ - 1);
        if (target == gallery_desired_index_) return;
        gallery_desired_index_ = target;
        gallery_pending_navigation_steps_ = 0;
        request_gallery_selection(target);
        update_gallery_controls();
    }

    void MainWindow::finish_gallery_slider_drag()
    {
        if (!gallery_slider_dragging_) return;
        gallery_slider_dragging_ = false;
        commit_gallery_slider();
    }
}
