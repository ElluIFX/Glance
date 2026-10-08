#include "pch.h"
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include "SettingsWindow.xaml.h"
#include "App.xaml.h"
#include "appearance_preferences.h"
#include "component_loader.h"
#include "dependencies/dependency_service.h"
#include "footer_preferences.h"
#include "localization.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "startup_registration.h"
#include "text_font_fallback.h"
#include "text_preferences.h"
#include "update_checker.h"
#include "webview_availability.h"
#include "window_size_store.h"
#include "glance/contracts/diagnostics.h"
#include "../../version.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <optional>
#include <ranges>
#include <string_view>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;
namespace Media = Microsoft::UI::Xaml::Media;
namespace Shapes = Microsoft::UI::Xaml::Shapes;

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::register_media_settings()
    {
        settings_registry_.register_page({L"media", L"", L"MediaPreviewNavigationItem.Content",
                                          L"MediaPreviewPageDescription.Text",
                                          glance::app::SettingsNavigationPosition::top});
        settings_registry_.register_section(
            {L"AudioVideoPreviewGroupTitle", L"media", L"AudioVideoPreviewGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DefaultAudioVolumeLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"DefaultAudioVolumeLabel.Text";
            definition.description_key = L"DefaultAudioVolumeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="8">
<NumberBox x:Name="DefaultAudioVolumeNumberBox" Width="184" Maximum="100" Minimum="0" SmallChange="5" SpinButtonPlacementMode="Inline" />
<TextBlock VerticalAlignment="Center" Text="%" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"DefaultAudioVolumeNumberBox",
                                        control.FindName(L"DefaultAudioVolumeNumberBox").as<FrameworkElement>());
                const auto weak = get_weak();
                DefaultAudioVolumeNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                DefaultAudioVolumeNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->DefaultAudioVolumeNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DefaultAudioVolumeNumberBox", [this] {
                settings_registry_.item(L"DefaultAudioVolumeLabel");
                return settings_registry_.control(L"DefaultAudioVolumeNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DefaultVideoVolumeLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"DefaultVideoVolumeLabel.Text";
            definition.description_key = L"DefaultVideoVolumeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="8">
<NumberBox x:Name="DefaultVideoVolumeNumberBox" Width="184" Maximum="100" Minimum="0" SmallChange="5" SpinButtonPlacementMode="Inline" />
<TextBlock VerticalAlignment="Center" Text="%" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"DefaultVideoVolumeNumberBox",
                                        control.FindName(L"DefaultVideoVolumeNumberBox").as<FrameworkElement>());
                const auto weak = get_weak();
                DefaultVideoVolumeNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                DefaultVideoVolumeNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->DefaultVideoVolumeNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DefaultVideoVolumeNumberBox", [this] {
                settings_registry_.item(L"DefaultVideoVolumeLabel");
                return settings_registry_.control(L"DefaultVideoVolumeNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AutoplayAudioLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"AutoplayAudioLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{AutoplayAudioToggle().IsOn()
                                                     ? L"AutoplayAudioEnabledDescription.Text"
                                                     : L"AutoplayAudioDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"AutoplayAudioToggle", control);
                const auto weak = get_weak();
                AutoplayAudioToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AutoplayAudioToggle", [this] {
                settings_registry_.item(L"AutoplayAudioLabel");
                return settings_registry_.control(L"AutoplayAudioToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AutoplayVideoLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"AutoplayVideoLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{AutoplayVideoToggle().IsOn()
                                                     ? L"AutoplayVideoEnabledDescription.Text"
                                                     : L"AutoplayVideoDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"AutoplayVideoToggle", control);
                const auto weak = get_weak();
                AutoplayVideoToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AutoplayVideoToggle", [this] {
                settings_registry_.item(L"AutoplayVideoLabel");
                return settings_registry_.control(L"AutoplayVideoToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"LoopPlaybackLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"LoopPlaybackLabel.Text";
            definition.description_key = L"LoopPlaybackDescription.Text";
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"LoopPlaybackToggle", control);
                const auto weak = get_weak();
                LoopPlaybackToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"LoopPlaybackToggle", [this] {
                settings_registry_.item(L"LoopPlaybackLabel");
                return settings_registry_.control(L"LoopPlaybackToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ReverseSeekWheelLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"ReverseSeekWheelLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{ReverseSeekWheelToggle().IsOn()
                                                     ? L"ReverseSeekWheelEnabledDescription.Text"
                                                     : L"ReverseSeekWheelDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"ReverseSeekWheelToggle", control);
                const auto weak = get_weak();
                ReverseSeekWheelToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"ReverseSeekWheelToggle", [this] {
                settings_registry_.item(L"ReverseSeekWheelLabel");
                return settings_registry_.control(L"ReverseSeekWheelToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"PreferFFmpegLabel";
            definition.parent = L"AudioVideoPreviewGroupTitle";
            definition.name_key = L"PreferFFmpegLabel.Text";
            definition.description_key = L"PreferFFmpegDescription.Text";
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"PreferFFmpegToggle", control);
                const auto weak = get_weak();
                PreferFFmpegToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"PreferFFmpegToggle", [this] {
                settings_registry_.item(L"PreferFFmpegLabel");
                return settings_registry_.control(L"PreferFFmpegToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section({L"ImagePreviewGroupTitle", L"media", L"ImagePreviewGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ImageZoomMapLabel";
            definition.parent = L"ImagePreviewGroupTitle";
            definition.name_key = L"ImageZoomMapLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{ImageZoomMapToggle().IsOn() ? L"ImageZoomMapEnabledDescription.Text"
                                                                             : L"ImageZoomMapDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"ImageZoomMapToggle", control);
                const auto weak = get_weak();
                ImageZoomMapToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"ImageZoomMapToggle", [this] {
                settings_registry_.item(L"ImageZoomMapLabel");
                return settings_registry_.control(L"ImageZoomMapToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }

        settings_registry_.register_section({L"MediaGeneralGroupTitle", L"media", L"MediaGeneralGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"MiddleClickGalleryModeLabel";
            definition.parent = L"MediaGeneralGroupTitle";
            definition.name_key = L"MiddleClickGalleryModeLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{MiddleClickGalleryModeToggle().IsOn()
                                                     ? L"MiddleClickGalleryModeEnabledDescription.Text"
                                                     : L"MiddleClickGalleryModeDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"MiddleClickGalleryModeToggle", control);
                const auto weak = get_weak();
                MiddleClickGalleryModeToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"MiddleClickGalleryModeToggle", [this] {
                settings_registry_.item(L"MiddleClickGalleryModeLabel");
                return settings_registry_.control(L"MiddleClickGalleryModeToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"LoopGalleryScrollingLabel";
            definition.parent = L"MediaGeneralGroupTitle";
            definition.name_key = L"LoopGalleryScrollingLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{LoopGalleryScrollingToggle().IsOn()
                                                     ? L"LoopGalleryScrollingEnabledDescription.Text"
                                                     : L"LoopGalleryScrollingDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"LoopGalleryScrollingToggle", control);
                const auto weak = get_weak();
                LoopGalleryScrollingToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"LoopGalleryScrollingToggle", [this] {
                settings_registry_.item(L"LoopGalleryScrollingLabel");
                return settings_registry_.control(L"LoopGalleryScrollingToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"GallerySameExtensionOnlyLabel";
            definition.parent = L"MediaGeneralGroupTitle";
            definition.name_key = L"GallerySameExtensionOnlyLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{GallerySameExtensionOnlyToggle().IsOn()
                                                     ? L"GallerySameExtensionOnlyEnabledDescription.Text"
                                                     : L"GallerySameExtensionOnlyDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"GallerySameExtensionOnlyToggle", control);
                const auto weak = get_weak();
                GallerySameExtensionOnlyToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MediaPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"GallerySameExtensionOnlyToggle", [this] {
                settings_registry_.item(L"GallerySameExtensionOnlyLabel");
                return settings_registry_.control(L"GallerySameExtensionOnlyToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
    }

    void SettingsWindow::set_media_volume(Controls::NumberBox const &control, double value, std::uint32_t &destination)
    {
        if (initializing_)
        {
            return;
        }

        if (!std::isfinite(value))
        {
            initializing_ = true;
            control.Value(destination);
            initializing_ = false;
            return;
        }

        const auto volume = static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 100L));
        destination = volume;
        if (std::abs(value - volume) > 0.001)
        {
            const bool was_initializing = initializing_;
            initializing_ = true;
            control.Value(volume);
            initializing_ = was_initializing;
        }
        glance::app::save_media_preview_preferences(media_preview_preferences_);
    }

    void SettingsWindow::DefaultAudioVolumeNumberBox_ValueChanged(IInspectable const &,
                                                                  Controls::NumberBoxValueChangedEventArgs const &args)
    {
        set_media_volume(DefaultAudioVolumeNumberBox(), args.NewValue(),
                         media_preview_preferences_.audio_volume_percent);
    }

    void SettingsWindow::DefaultVideoVolumeNumberBox_ValueChanged(IInspectable const &,
                                                                  Controls::NumberBoxValueChangedEventArgs const &args)
    {
        set_media_volume(DefaultVideoVolumeNumberBox(), args.NewValue(),
                         media_preview_preferences_.video_volume_percent);
    }

    void SettingsWindow::MediaPreferenceToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (initializing_)
        {
            return;
        }
        media_preview_preferences_.autoplay_audio = AutoplayAudioToggle().IsOn();
        media_preview_preferences_.autoplay_video = AutoplayVideoToggle().IsOn();
        media_preview_preferences_.loop_playback = LoopPlaybackToggle().IsOn();
        media_preview_preferences_.prefer_ffmpeg = PreferFFmpegToggle().IsOn();
        media_preview_preferences_.reverse_seek_wheel = ReverseSeekWheelToggle().IsOn();
        media_preview_preferences_.middle_click_gallery_mode = MiddleClickGalleryModeToggle().IsOn();
        media_preview_preferences_.loop_gallery_scrolling = LoopGalleryScrollingToggle().IsOn();
        media_preview_preferences_.gallery_same_extension_only = GallerySameExtensionOnlyToggle().IsOn();
        media_preview_preferences_.show_image_zoom_map = ImageZoomMapToggle().IsOn();
        glance::app::save_media_preview_preferences(media_preview_preferences_);
    }
} // namespace winrt::Glance::App::implementation
