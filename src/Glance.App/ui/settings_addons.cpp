#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "dependencies/dependency_service.h"
#include "localization.h"
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace dependencies = glance::app::dependencies;
using Availability = glance::contracts::dependencies::Availability;

namespace
{
    void show_rows(StackPanel const &panel, std::vector<UIElement> const &rows)
    {
        for (std::uint32_t index = 0; index < rows.size(); ++index)
        {
            const auto row = rows[index].as<Grid>();
            row.Style(Application::Current().Resources().Lookup(box_value(L"SettingsListRowStyle")).as<Style>());
            row.BorderThickness(index + 1 == rows.size() ? Thickness{} : Thickness{0, 0, 0, 1});
            if (index < panel.Children().Size() && panel.Children().GetAt(index) == rows[index])
                continue;
            std::uint32_t previous{};
            if (panel.Children().IndexOf(rows[index], previous))
                panel.Children().RemoveAt(previous);
            panel.Children().InsertAt(index, rows[index]);
        }
        while (panel.Children().Size() > rows.size())
            panel.Children().RemoveAtEnd();
    }

    void set_health(FontIcon const &icon, unsigned severity, std::wstring_view detail = {})
    {
        const auto key = severity == 0   ? L"ComponentStateHealthy"
                         : severity == 1 ? L"ComponentStateWarning"
                                         : L"ComponentStateError";
        icon.Glyph(severity == 0 ? L"\xE8FB" : severity == 1 ? L"\xE7BA" : L"\xE711");
        icon.FontSize(18);
        icon.Foreground(Media::SolidColorBrush(severity == 0   ? Windows::UI::Color{255, 16, 124, 16}
                                               : severity == 1 ? Windows::UI::Color{255, 157, 93, 0}
                                                               : Windows::UI::Color{255, 196, 43, 28}));
        std::wstring text(glance::app::localize(key));
        if (!detail.empty() && detail != text) text += L"\n" + std::wstring(detail);
        ToolTipService::SetToolTip(icon, box_value(text));
    }
} // namespace

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::refresh_dependency_statuses()
    {
        if (!settings_registry_.page_created(L"components"))
            return;
        const auto components = glance::app::component_statuses(glance::app::current_ui_language());
        const auto weak = get_weak();
        std::vector<UIElement> rows;
        for (const auto &dependency : dependencies::snapshot())
        {
            const auto id = dependency.definition.id;
            auto &row = dependency_rows_[id];
            if (!row.view.root)
            {
                StackPanel actions;
                actions.Orientation(Orientation::Horizontal);
                actions.Spacing(8);
                row.status = TextBlock();
                row.status.VerticalAlignment(VerticalAlignment::Center);
                row.action = glance::app::make_settings_button();
                actions.Children().Append(row.status);
                actions.Children().Append(row.action);
                row.view = glance::app::make_settings_row(actions);
                row.progress = ProgressBar();
                row.progress.Maximum(100);
                row.view.text.Children().Append(row.progress);
                row.action.Click([weak, id](IInspectable const &, RoutedEventArgs const &) {
                    if (const auto self = weak.get())
                    {
                        self->dependency_errors_.erase(id);
                        if (const auto transfer = dependencies::transfer(id); transfer && !transfer->state().complete)
                            transfer->cancel();
                        else
                        {
                            const auto snapshot = dependencies::snapshot();
                            const auto current = std::ranges::find_if(
                                snapshot, [&](const auto &item) { return item.definition.id == id; });
                            if (current == snapshot.end())
                                return;
                            if (current->availability == Availability::managed)
                                self->uninstall_dependency(id);
                            else
                            {
                                try
                                {
                                    static_cast<void>(
                                        dependencies::begin_install(id, self->network_download_callback_));
                                }
                                catch (...)
                                {
                                    self->dependency_errors_[id] = L"DependencyFailed";
                                }
                            }
                        }
                        self->refresh_dependency_statuses();
                    }
                });
            }
            row.view.title.Text(dependency.definition.display_name);
            std::wstring description;
            for (const auto &consumer : dependency.consumers)
            {
                const auto found = std::ranges::find(components, consumer, &glance::app::ComponentStatus::id);
                if (!description.empty())
                    description += L", ";
                description +=
                    found != components.end()      ? found->display_name
                    : consumer == L"video-preview" ? std::wstring(glance::app::localize(L"DependencyVideoConsumer"))
                    : consumer == L"audio-preview" ? std::wstring(glance::app::localize(L"DependencyAudioConsumer"))
                                                   : consumer;
            }
            if (!dependency.definition.description_key.empty())
            {
                const auto owner = std::ranges::find_if(components, [&](const auto &component) {
                    return std::ranges::find(dependency.consumers, component.id) != dependency.consumers.end();
                });
                description = owner != components.end()
                                  ? glance::app::localize_component(owner->id, dependency.definition.description_key)
                                  : glance::app::localize(dependency.definition.description_key);
            }
            row.view.description.Text(description);
            const auto task = dependencies::transfer(id);
            const bool busy = task && !task->state().complete;
            const bool installed = dependency.availability == Availability::managed;
            const auto key =
                busy        ? (dependency.availability == Availability::installing ? L"DependencyInstalling"
                                                                                   : L"DependencyDownloading")
                : installed ? L"DependencyInstalled"
                : dependency.availability == Availability::external ? L"DependencyExternal"
                : task && FAILED(task->state().error) && task->state().error != HRESULT_FROM_WIN32(ERROR_CANCELLED)
                    ? L"DependencyFailed"
                    : L"DependencyMissing";
            row.status.Text(glance::app::localize(key));
            if (const auto error = dependency_errors_.find(id); error != dependency_errors_.end())
                row.status.Text(glance::app::localize(error->second));
            row.action.Content(box_value(glance::app::localize(busy        ? L"DependencyCancel"
                                                               : installed ? L"DependencyUninstall"
                                                                           : L"DependencyDownload")));
            row.progress.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
            if (busy)
            {
                const auto state = task->state();
                row.progress.IsIndeterminate(state.availability == Availability::installing);
                if (state.total)
                    row.progress.Value(100.0 * state.downloaded / state.total);
                const auto dispatcher = DispatcherQueue();
                if (row.observed_transfer.lock() != task)
                {
                    row.observed_transfer = task;
                    task->subscribe([weak, dispatcher, id, expected = std::weak_ptr(task)] {
                        dispatcher.TryEnqueue([weak, id, expected] {
                            const auto self = weak.get();
                            const auto active = expected.lock();
                            if (!self || !active || dependencies::transfer(id) != active)
                                return;
                            const auto state = active->state();
                            if (state.complete)
                            {
                                self->refresh_component_statuses();
                                return;
                            }
                            const auto found = self->dependency_rows_.find(id);
                            if (found == self->dependency_rows_.end())
                                return;
                            found->second.progress.IsIndeterminate(state.availability == Availability::installing);
                            if (state.total)
                                found->second.progress.Value(100.0 * state.downloaded / state.total);
                            found->second.status.Text(glance::app::localize(
                                state.availability == Availability::installing ? L"DependencyInstalling"
                                                                               : L"DependencyDownloading"));
                        });
                    });
                }
                if (task->state().complete)
                    DispatcherQueue().TryEnqueue([weak] {
                        if (const auto self = weak.get())
                            self->refresh_component_statuses();
                    });
            }
            rows.push_back(row.view.root);
        }
        show_rows(DependencyStatusList(), rows);
    }

    void SettingsWindow::refresh_component_statuses()
    {
        if (!settings_registry_.page_created(L"components"))
            return;
        refresh_dependency_statuses();
        const auto statuses = glance::app::component_statuses(glance::app::current_ui_language());
        ComponentEmptyState().Visibility(statuses.empty() ? Visibility::Visible : Visibility::Collapsed);
        ComponentStatusScroller().Visibility(statuses.empty() ? Visibility::Collapsed : Visibility::Visible);
        std::vector<UIElement> rows;
        for (const auto &status : statuses)
        {
            auto &row = component_rows_[status.id];
            initialize_addon_row(row);
            row.metadata = status.metadata;
            update_addon_metadata(row);
            row.view.title.Text(status.display_name);
            row.view.description.Text(status.id);
            const auto severity = status.state == glance::app::ComponentState::healthy   ? 0U
                                  : status.state == glance::app::ComponentState::warning ? 1U
                                                                                         : 2U;
            set_health(row.icon, severity, status.detail);
            rows.push_back(row.view.root);
        }
        show_rows(ComponentStatusList(), rows);
    }

    void SettingsWindow::initialize_addon_row(AddonRow& row)
    {
        if (row.view.root) return;
        row.icon = FontIcon();
        row.information = Button();
        row.information.Style(Application::Current().Resources().Lookup(box_value(L"IconButtonStyle")).as<Style>());
        FontIcon information;
        information.Glyph(L"\xE946");
        information.FontSize(16);
        row.information.Content(information);
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(12);
        actions.Children().Append(row.information);
        actions.Children().Append(row.icon);
        row.view = glance::app::make_settings_row(actions);
        row.information_flyout = Flyout();
        row.information_flyout.Placement(Primitives::FlyoutPlacementMode::RightEdgeAlignedTop);
        StackPanel panel;
        panel.Spacing(6);
        for (std::size_t index = 0; index < row.information_text.size(); ++index)
        {
            auto& text = row.information_text[index];
            text = TextBlock();
            text.FontSize(12);
            text.TextWrapping(TextWrapping::Wrap);
            text.IsTextSelectionEnabled(true);
            if (index % 2 == 0)
            {
                text.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                if (index != 0) text.Margin({0, 6, 0, 0});
            }
            panel.Children().Append(text);
        }
        ScrollViewer scroller;
        scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroller.HorizontalScrollMode(ScrollMode::Disabled);
        scroller.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.Content(panel);
        row.information_flyout.Content(scroller);
        const auto weak = get_weak();
        const auto target = &row;
        row.information.PointerEntered([weak, target](auto const&, auto const&) {
            if (const auto self = weak.get()) self->schedule_addon_metadata(target, true);
        });
        row.information.PointerExited([weak, target](auto const&, auto const&) {
            if (const auto self = weak.get()) self->schedule_addon_metadata(target, false);
        });
        panel.PointerEntered([weak](auto const&, auto const&) {
            if (const auto self = weak.get(); self && self->addon_metadata_timer_) self->addon_metadata_timer_.Stop();
        });
        panel.PointerExited([weak, target](auto const&, auto const&) {
            if (const auto self = weak.get()) self->schedule_addon_metadata(target, false);
        });
        row.information.Click([weak, target](auto const&, auto const&) {
            if (const auto self = weak.get()) self->show_addon_metadata(target, true);
        });
        row.information_flyout.Closed([weak, target](auto const&, auto const&) {
            if (const auto self = weak.get(); self && self->addon_metadata_row_ == target)
            {
                self->addon_metadata_row_ = nullptr;
                self->addon_metadata_pinned_ = false;
                if (self->addon_metadata_timer_) self->addon_metadata_timer_.Stop();
            }
        });
    }

    void SettingsWindow::update_addon_metadata(AddonRow& row)
    {
        const std::array strings{glance::app::localize(L"AddonMetadataSummary"), row.metadata.summary,
            glance::app::localize(L"AddonMetadataCapabilities"), row.metadata.capabilities,
            glance::app::localize(L"AddonMetadataDependencies"), row.metadata.dependencies};
        for (std::size_t index = 0; index < strings.size(); ++index)
            if (row.information_text[index].Text() != strings[index]) row.information_text[index].Text(strings[index]);
        ToolTipService::SetToolTip(row.information, nullptr);
        Automation::AutomationProperties::SetName(row.information, glance::app::localize(L"AddonMetadataShow"));
        row.information.IsEnabled(!row.metadata.summary.empty() && !row.metadata.capabilities.empty() &&
            !row.metadata.dependencies.empty());
    }

    void SettingsWindow::schedule_addon_metadata(AddonRow* row, bool show)
    {
        if (addon_metadata_pinned_) return;
        if (!show && addon_metadata_row_ != row) return;
        if (show && addon_metadata_row_ != row) close_addon_metadata();
        addon_metadata_row_ = row;
        addon_metadata_show_ = show;
        if (!addon_metadata_timer_)
        {
            addon_metadata_timer_ = DispatcherTimer();
            addon_metadata_timer_.Tick([weak = get_weak()](auto const&, auto const&) {
                if (const auto self = weak.get())
                {
                    self->addon_metadata_timer_.Stop();
                    if (self->addon_metadata_show_) self->show_addon_metadata(self->addon_metadata_row_, false);
                    else self->close_addon_metadata();
                }
            });
        }
        addon_metadata_timer_.Stop();
        addon_metadata_timer_.Interval(std::chrono::milliseconds(show ? 200 : 150));
        addon_metadata_timer_.Start();
    }

    void SettingsWindow::show_addon_metadata(AddonRow* target, bool pinned)
    {
        if (!target || !target->information.IsEnabled()) return;
        if (addon_metadata_row_ != target) close_addon_metadata();
        if (addon_metadata_timer_) addon_metadata_timer_.Stop();
        auto& row = *target;
        addon_metadata_row_ = target;
        addon_metadata_pinned_ = pinned;
        row.information_flyout.ShowMode(pinned ? Primitives::FlyoutShowMode::Standard : Primitives::FlyoutShowMode::Transient);
        const double width = std::max(180.0, std::min(420.0, RootGrid().ActualWidth() - 48.0));
        row.information_flyout.Content().as<ScrollViewer>().MaxWidth(width - 32.0);
        Style style(xaml_typename<FlyoutPresenter>());
        style.Setters().Append(Setter(FrameworkElement::MaxWidthProperty(), box_value(width)));
        style.Setters().Append(Setter(FrameworkElement::MaxHeightProperty(), box_value(std::max(120.0, RootGrid().ActualHeight() - 80.0))));
        style.Setters().Append(Setter(ScrollViewer::HorizontalScrollBarVisibilityProperty(), box_value(ScrollBarVisibility::Disabled)));
        style.Setters().Append(Setter(ScrollViewer::HorizontalScrollModeProperty(), box_value(ScrollMode::Disabled)));
        row.information_flyout.FlyoutPresenterStyle(style);
        if (!row.information_flyout.IsOpen()) row.information_flyout.ShowAt(row.information);
    }

    void SettingsWindow::close_addon_metadata()
    {
        if (addon_metadata_timer_) addon_metadata_timer_.Stop();
        const auto row = addon_metadata_row_;
        addon_metadata_row_ = nullptr;
        addon_metadata_pinned_ = false;
        if (row) row->information_flyout.Hide();
    }

    void SettingsWindow::HandleSourceStatuses(std::string_view payload)
    {
        if (!settings_registry_.page_created(L"components"))
            return;
        try
        {
            const auto sources = Windows::Data::Json::JsonObject::Parse(to_hstring(payload)).GetNamedArray(L"sources");
            SourceEmptyState().Visibility(sources.Size() == 0 ? Visibility::Visible : Visibility::Collapsed);
            SourceStatusScroller().Visibility(sources.Size() == 0 ? Visibility::Collapsed : Visibility::Visible);
            std::vector<UIElement> rows;
            for (std::uint32_t index = 0; index < sources.Size(); ++index)
            {
                const auto status = sources.GetObjectAt(index);
                const auto id = status.GetNamedString(L"id");
                auto &row = source_rows_[std::wstring(id)];
                initialize_addon_row(row);
                const auto metadata = status.GetNamedObject(L"metadata");
                row.metadata.summary = metadata.GetNamedString(L"summary");
                row.metadata.capabilities = metadata.GetNamedString(L"capabilities");
                row.metadata.dependencies = metadata.GetNamedString(L"dependencies");
                update_addon_metadata(row);
                row.view.title.Text(status.GetNamedString(L"name"));
                auto detail = status.GetNamedString(L"detail", L"");
                if (detail.empty() && status.GetNamedNumber(L"code", 0) == 1)
                    detail = glance::app::localize(L"SourceLoadError");
                row.view.description.Text(id);
                set_health(row.icon, static_cast<unsigned>(status.GetNamedNumber(L"severity", 2)), detail);
                rows.push_back(row.view.root);
            }
            show_rows(SourceStatusList(), rows);
        }
        catch (...)
        {
        }
    }
} // namespace winrt::Glance::App::implementation
