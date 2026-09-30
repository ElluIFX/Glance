#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "dependencies/dependency_service.h"
#include "localization.h"
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

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

    void set_health(FontIcon const &icon, unsigned severity)
    {
        const auto key = severity == 0   ? L"ComponentStateHealthy"
                         : severity == 1 ? L"ComponentStateWarning"
                                         : L"ComponentStateError";
        icon.Glyph(severity == 0 ? L"\xE8FB" : severity == 1 ? L"\xE7BA" : L"\xE711");
        icon.FontSize(18);
        icon.Foreground(Media::SolidColorBrush(severity == 0   ? Windows::UI::Color{255, 16, 124, 16}
                                               : severity == 1 ? Windows::UI::Color{255, 157, 93, 0}
                                                               : Windows::UI::Color{255, 196, 43, 28}));
        ToolTipService::SetToolTip(icon, box_value(glance::app::localize(key)));
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
            if (!row.view.root)
            {
                row.icon = FontIcon();
                row.view = glance::app::make_settings_row(row.icon);
            }
            row.view.title.Text(status.display_name);
            row.view.description.Text(status.detail);
            const auto severity = status.state == glance::app::ComponentState::healthy   ? 0U
                                  : status.state == glance::app::ComponentState::warning ? 1U
                                                                                         : 2U;
            set_health(row.icon, severity);
            rows.push_back(row.view.root);
        }
        show_rows(ComponentStatusList(), rows);
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
                auto &row = source_rows_[std::wstring(status.GetNamedString(L"id"))];
                if (!row.view.root)
                {
                    row.icon = FontIcon();
                    row.view = glance::app::make_settings_row(row.icon);
                }
                row.view.title.Text(status.GetNamedString(L"name"));
                auto detail = status.GetNamedString(L"detail", L"");
                if (detail.empty() && status.GetNamedNumber(L"code", 0) == 1)
                    detail = glance::app::localize(L"SourceLoadError");
                row.view.description.Text(detail);
                set_health(row.icon, static_cast<unsigned>(status.GetNamedNumber(L"severity", 2)));
                rows.push_back(row.view.root);
            }
            show_rows(SourceStatusList(), rows);
        }
        catch (...)
        {
        }
    }
} // namespace winrt::Glance::App::implementation
