#include "view.h"
#include "client.h"
#include <robuffer.h>
#undef GetCurrentTime
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.h>

namespace glance::font
{
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace Resources = Microsoft::Windows::ApplicationModel::Resources;
namespace
{
struct Worker
{
    std::shared_ptr<Client> client = std::make_shared<Client>();
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopped{}, pending{};
    std::atomic_bool finished{};
    Request next;
    std::uint64_t generation{};
    void cancel()
    {
        {
            std::scoped_lock lock(mutex);
            stopped = true;
        }
        if (client) client->cancel();
        client.reset();
        condition.notify_all();
    }
    ~Worker()
    {
        cancel();
        if (thread.joinable()) thread.join();
    }
};
std::vector<std::shared_ptr<Worker>> workers;

struct View : std::enable_shared_from_this<View>
{
    contracts::components::ComponentViewHost view_host;
    Grid root, body;
    Grid controls, choices, adjustments, size_group, weight_group;
    StackPanel information;
    TextBlock title, subtitle, status;
    ComboBox faces;
    NumberBox size, weight;
    Border adjustment_separator;
    TextBlock size_label, weight_label;
    Border details;
    TextBlock metadata_title;
    ScrollViewer scroll, metadata_scroll;
    Canvas canvas;
    Image image;
    Button retry, install_user, install_system;
    TextBlock install_status;
    bool installing{}, install_supported{}, closed{};
    const wchar_t* install_message{};
    Resources::ResourceManager resources{nullptr};
    Resources::ResourceContext context{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    winrt::Windows::UI::ViewManagement::UISettings ui_settings;
    winrt::event_token contrast_token{};
    XamlRoot::Changed_revoker scale_changed;
    std::wstring path, language;
    std::shared_ptr<Metadata> metadata;
    std::shared_ptr<Worker> worker;
    bool updating{};
    std::uint64_t serial{};
    std::uint64_t run{};
    unsigned desired_face{};
    ~View()
    {
        close();
    }
    hstring text(const wchar_t *key)
    {
        auto candidate = resources.MainResourceMap().GetSubtree(L"Resources").GetValue(key, context);
        return candidate.ValueAsString();
    }
    TextBlock label(const wchar_t *key, bool heading = false)
    {
        TextBlock block;
        block.Text(text(key));
        block.TextWrapping(TextWrapping::Wrap);
        block.IsTextSelectionEnabled(true);
        if (heading)
        {
            block.FontSize(16);
            block.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
            block.Margin({0, 12, 0, 4});
        }
        return block;
    }
    void initialize(const std::wstring &file, const std::wstring &tag)
    {
        path = file;
        language = tag;
        resources = Resources::ResourceManager((directory() / L"resources.pri").wstring());
        context = resources.CreateResourceContext();
        context.QualifierValues().Insert(L"Language", language);
        dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        root.Padding({24, 16, 24, 16});
        for (auto height : {GridLength{1, GridUnitType::Auto}, GridLength{1, GridUnitType::Auto},
                            GridLength{1, GridUnitType::Star}, GridLength{1, GridUnitType::Auto},
                            GridLength{1, GridUnitType::Auto}})
        {
            RowDefinition row;
            row.Height(height);
            root.RowDefinitions().Append(row);
        }
        ColumnDefinition content_column;
        content_column.Width({1, GridUnitType::Star});
        root.ColumnDefinitions().Append(content_column);
        ColumnDefinition information_column;
        information_column.Width({280, GridUnitType::Pixel});
        root.ColumnDefinitions().Append(information_column);
        root.ColumnSpacing(24);
        StackPanel heading;
        heading.Spacing(4);
        title.FontSize(24);
        title.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
        title.Text(std::filesystem::path(path).filename().wstring());
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        subtitle.FontSize(12);
        subtitle.Opacity(0.7);
        heading.Children().Append(title);
        heading.Children().Append(subtitle);
        root.Children().Append(heading);
        Grid installation;
        installation.ColumnSpacing(12);
        installation.VerticalAlignment(VerticalAlignment::Center);
        for (unsigned i = 0; i < 2; ++i)
        {
            ColumnDefinition column; column.Width({1, GridUnitType::Star});
            installation.ColumnDefinitions().Append(column);
        }
        install_user.Content(box_value(text(L"InstallUser")));
        install_system.Content(box_value(text(L"InstallSystem")));
        for (auto button : {install_user, install_system})
        {
            button.IsTabStop(false); button.AllowFocusOnInteraction(false);
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.IsEnabled(false);
            installation.Children().Append(button);
        }
        Grid::SetColumn(install_system, 1);
        Grid::SetColumn(installation, 1); root.Children().Append(installation);
        const auto extension = std::filesystem::path(path).extension().wstring();
        install_supported = _wcsicmp(extension.c_str(), L".ttf") == 0 ||
            _wcsicmp(extension.c_str(), L".otf") == 0 || _wcsicmp(extension.c_str(), L".ttc") == 0 ||
            _wcsicmp(extension.c_str(), L".otc") == 0;
        refresh_installation();
        auto toolbar = Markup::XamlReader::Load(
            LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1"
                CornerRadius="8" Padding="16"/>)").as<Border>();
        toolbar.Margin({0, 16, 0, 20});
        toolbar.Child(controls);
        Grid::SetRow(toolbar, 1);
        root.Children().Append(toolbar);
        controls.RowSpacing(12);
        for (unsigned i = 0; i < 3; ++i)
        {
            ColumnDefinition column;
            column.Width({1, i == 0 ? GridUnitType::Star : GridUnitType::Auto});
            controls.ColumnDefinitions().Append(column);
        }
        controls.Children().Append(choices);
        Automation::AutomationProperties::SetName(faces, text(L"Face"));
        faces.MinWidth(60);
        faces.HorizontalAlignment(HorizontalAlignment::Stretch);
        faces.VerticalAlignment(VerticalAlignment::Bottom);
        faces.IsEnabled(false);
        choices.Children().Append(faces);
        for (unsigned i = 0; i < 3; ++i)
        {
            ColumnDefinition column;
            column.Width({1, GridUnitType::Auto});
            adjustments.ColumnDefinitions().Append(column);
        }
        auto face_separator = Markup::XamlReader::Load(
            LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource DividerStrokeColorDefaultBrush}"
                Width="1" Height="24" Margin="16,0" VerticalAlignment="Center"/>)").as<Border>();
        Grid::SetColumn(face_separator, 1); controls.Children().Append(face_separator);
        Grid::SetColumn(adjustments, 2);
        controls.Children().Append(adjustments);
        const auto add_adjustment = [&](Grid group, TextBlock label, NumberBox number,
                                        const wchar_t* key) {
            group.ColumnSpacing(12);
            group.HorizontalAlignment(HorizontalAlignment::Stretch);
            group.VerticalAlignment(VerticalAlignment::Bottom);
            for (unsigned i = 0; i < 2; ++i)
            {
                ColumnDefinition column;
                column.Width({i == 0 ? 1.0 : 88.0, i == 0 ? GridUnitType::Auto : GridUnitType::Pixel});
                group.ColumnDefinitions().Append(column);
            }
            label.Text(text(key));
            label.FontSize(14);
            label.VerticalAlignment(VerticalAlignment::Center);
            group.Children().Append(label);
            number.HorizontalAlignment(HorizontalAlignment::Stretch);
            number.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
            Grid::SetColumn(number, 1);
            group.Children().Append(number);
            Automation::AutomationProperties::SetName(number, text(key));
            adjustments.Children().Append(group);
        };
        add_adjustment(size_group, size_label, size, L"Size");
        add_adjustment(weight_group, weight_label, weight, L"Weight");
        Grid::SetColumn(weight_group, 2);
        adjustment_separator = Markup::XamlReader::Load(
            LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource DividerStrokeColorDefaultBrush}"
                Width="1" Height="24" Margin="16,0" VerticalAlignment="Center"/>)").as<Border>();
        Grid::SetColumn(adjustment_separator, 1);
        adjustments.Children().Append(adjustment_separator);
        size.Minimum(8);
        size.Maximum(512);
        size.Value(16);
        retry.IsTabStop(false); retry.AllowFocusOnInteraction(false);
        weight.IsEnabled(false);
        Grid::SetRow(body, 2);
        root.Children().Append(body);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(canvas);
        canvas.Children().Append(image);
        image.Stretch(Media::Stretch::Fill);
        body.Children().Append(scroll);
        scroll.Background(Media::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
        details = Markup::XamlReader::Load(
            LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1"
                CornerRadius="8"/>)").as<Border>();
        Grid metadata_panel;
        RowDefinition metadata_heading; metadata_heading.Height({1, GridUnitType::Auto});
        RowDefinition metadata_body; metadata_body.Height({1, GridUnitType::Star});
        RowDefinition metadata_feedback; metadata_feedback.Height({1, GridUnitType::Auto});
        metadata_panel.RowDefinitions().Append(metadata_heading);
        metadata_panel.RowDefinitions().Append(metadata_body);
        metadata_panel.RowDefinitions().Append(metadata_feedback);
        metadata_title.Text(text(L"Metadata"));
        metadata_title.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
        metadata_title.Margin({16, 16, 16, 12});
        metadata_panel.Children().Append(metadata_title);
        details.HorizontalAlignment(HorizontalAlignment::Stretch);
        details.VerticalAlignment(VerticalAlignment::Stretch);
        details.Margin({0, 16, 0, 0});
        metadata_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        metadata_scroll.Content(information);
        information.Spacing(6);
        information.Margin({12, 0, 12, 12});
        Grid::SetRow(metadata_scroll, 1);
        metadata_panel.Children().Append(metadata_scroll);
        install_status.TextWrapping(TextWrapping::Wrap);
        install_status.Margin({12, 0, 12, 12});
        install_status.Visibility(Visibility::Collapsed);
        Grid::SetRow(install_status, 2); metadata_panel.Children().Append(install_status);
        details.Child(metadata_panel);
        Grid::SetColumn(details, 1);
        Grid::SetRow(details, 1);
        Grid::SetRowSpan(details, 3);
        root.Children().Append(details);
        StackPanel footer;
        footer.Orientation(Orientation::Horizontal);
        footer.Spacing(12);
        Grid::SetRow(footer, 3);
        status.Text(text(L"Loading"));
        status.TextWrapping(TextWrapping::Wrap);
        status.VerticalAlignment(VerticalAlignment::Center);
        retry.Content(box_value(text(L"Retry")));
        retry.Visibility(Visibility::Collapsed);
        footer.Children().Append(status);
        footer.Children().Append(retry);
        root.Children().Append(footer);
        auto weak = weak_from_this();
        install_user.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->install(false); });
        install_system.Click([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->install(true); });
        root.SizeChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
            {
                self->schedule();
            }
        });
        root.ActualThemeChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->schedule();
        });
        root.Loaded([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
            {
                self->scale_changed = self->root.XamlRoot().Changed(auto_revoke, [weak](auto &&, auto &&) {
                    if (auto target = weak.lock())
                        target->schedule();
                });
                self->schedule();
            }
        });
        contrast_token = ui_settings.ColorValuesChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->dispatcher.TryEnqueue([weak] {
                    if (auto target = weak.lock())
                        target->schedule();
                });
        });
        body.SizeChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
            {
                self->schedule();
            }
        });
        faces.SelectionChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock(); self && !self->updating && self->faces.SelectedIndex() >= 0)
            {
                self->desired_face = static_cast<unsigned>(self->faces.SelectedIndex());
                self->metadata.reset();
                self->schedule();
            }
        });
        size.ValueChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock(); self && !self->updating)
            {
                self->updating = true;
                auto value = self->size.Value();
                if (!std::isfinite(value))
                    value = 16;
                value = std::clamp(value, self->size.Minimum(), self->size.Maximum());
                self->size.Value(value);
                self->updating = false;
                self->schedule();
            }
        });
        weight.ValueChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock(); self && !self->updating)
            {
                self->updating = true;
                auto value = self->weight.Value();
                if (!std::isfinite(value))
                    value = self->metadata ? self->metadata->weight : 400;
                value = std::clamp(value, self->weight.Minimum(), self->weight.Maximum());
                self->weight.Value(value);
                self->updating = false;
                self->schedule();
            }
        });
        scroll.ViewChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->schedule();
        });
        retry.Click([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->restart();
        });
        timer = dispatcher.CreateTimer();
        timer.Interval(std::chrono::milliseconds(34));
        timer.IsRepeating(false);
        timer.Tick([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->dispatch();
        });
        restart();
    }
    void refresh_installation()
    {
        install_user.Content(box_value(text(L"InstallUser")));
        install_system.Content(box_value(text(L"InstallSystem")));
        for (auto button : {install_user, install_system})
        {
            button.IsEnabled(install_supported && metadata && !installing);
            ToolTipService::SetToolTip(button, install_supported ? nullptr : box_value(text(L"InstallUnsupported")));
        }
        if (install_message) install_status.Text(text(install_message));
    }
    static fire_and_forget run_install(std::weak_ptr<View> weak, std::wstring file, HWND owner, bool system)
    {
        DWORD result = ERROR_FUNCTION_FAILED;
        try
        {
            const auto executable = (directory() / L"Glance.FontHost.exe").wstring();
            auto command = L"\"" + executable + L"\" " + (system ? L"--install-system" : L"--install-user") +
                L" \"" + file + L"\" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(owner));
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            {
                handle process_handle(process.hProcess), thread_handle(process.hThread);
                co_await resume_on_signal(process_handle.get());
                if (!GetExitCodeProcess(process_handle.get(), &result)) result = GetLastError();
            }
            else result = GetLastError();
        }
        catch (...) { result = ERROR_FUNCTION_FAILED; }
        if (auto self = weak.lock())
        {
            self->dispatcher.TryEnqueue([weak, result] {
                if (auto view = weak.lock(); view && !view->closed)
                {
                    view->installing = false;
                    view->install_message = result == ERROR_SUCCESS ? L"InstallRequested" :
                        result == ERROR_CANCELLED ? L"InstallCancelled" : L"InstallFailed";
                    view->refresh_installation();
                }
            });
        }
    }
    void install(bool system)
    {
        if (installing || !install_supported || !metadata || closed) return;
        installing = true; install_message = L"Installing";
        install_status.Visibility(Visibility::Visible);
        refresh_installation();
        run_install(weak_from_this(), path, view_host.owner, system);
    }
    void schedule()
    {
        if (!updating && timer)
        {
            ++serial;
            if (!timer.IsRunning())
                timer.Start();
        }
    }
    void dispatch()
    {
        if (scroll.ActualWidth() < 1 || scroll.ActualHeight() < 1)
            return;
        Request request;
        request.operation = Operation::render;
        request.face = desired_face;
        request.size = static_cast<float>(size.Value());
        request.weight = metadata ? static_cast<float>(weight.Value()) : 400;
        request.scale = root.XamlRoot() ? static_cast<float>(root.XamlRoot().RasterizationScale()) : 1;
        request.width =
            static_cast<unsigned>(std::clamp((scroll.ActualWidth() - 16) * request.scale, 1.0, 4096.0));
        request.height =
            static_cast<unsigned>(std::clamp(scroll.ActualHeight() * request.scale, 1.0, 4096.0));
        request.offset = static_cast<float>(scroll.VerticalOffset());
        request.color = root.ActualTheme() == ElementTheme::Dark ? 0xfff3f3f3 : 0xff202020;
        if (accessibility.HighContrast())
        {
            const auto color = winrt::Windows::UI::ViewManagement::UISettings().UIElementColor(
                winrt::Windows::UI::ViewManagement::UIElementType::WindowText);
            request.color = 0xff000000U | (unsigned(color.R) << 16) | (unsigned(color.G) << 8) | color.B;
        }
        {
            if (!worker) return;
            std::scoped_lock lock(worker->mutex);
            if (worker->stopped)
                return;
            worker->next = request;
            worker->pending = true;
            worker->generation = serial;
        }
        worker->condition.notify_one();
    }
    void restart()
    {
        close_worker();
        notify(contracts::components::ComponentViewState::loading);
        metadata.reset();
        status.Text(text(L"Loading"));
        status.Opacity(1);
        retry.Visibility(Visibility::Collapsed);
        std::erase_if(workers, [](const auto &item) { return item->finished.load(); });
        worker = std::make_shared<Worker>();
        workers.push_back(worker);
        auto state = worker.get();
        auto connection = state->client;
        auto weak = weak_from_this();
        auto queue = dispatcher;
        const auto file = path;
        const auto host = (directory() / L"Glance.FontHost.exe").wstring();
        const auto epoch = run;
        worker->thread = std::thread([state, connection, weak, queue, file, host, epoch]() mutable {
            struct Completion
            {
                Worker *state;
                std::shared_ptr<Client> &connection;
                ~Completion()
                {
                    connection.reset();
                    state->finished = true;
                }
            } completion{state, connection};
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment
                {
                    ~Apartment() { winrt::uninit_apartment(); }
                } apartment;
                connection->open(host, file);
                unsigned face = UINT_MAX;
                std::shared_ptr<Metadata> info;
                for (;;)
                {
                    Request request;
                    std::uint64_t version{};
                    {
                        std::unique_lock lock(state->mutex);
                        state->condition.wait(lock, [&] { return state->stopped || state->pending; });
                        if (state->stopped)
                            return;
                        request = state->next;
                        version = state->generation;
                        state->pending = false;
                    }
                    if (face != request.face)
                    {
                        info = connection->metadata(request.face);
                        face = request.face;
                        request.weight = info->weight;
                    }
                    auto raster = connection->render(request);
                    queue.TryEnqueue([weak, request, version, info, epoch,
                                      raster = std::move(raster)]() mutable {
                        if (auto self = weak.lock(); self && epoch == self->run && version == self->serial)
                        {
                            try
                            {
                                if (info && (!self->metadata || self->metadata->selected != info->selected))
                                    self->show_metadata(info);
                                self->show(request, raster.first, raster.second);
                            }
                            catch (...)
                            {
                                self->failure();
                            }
                        }
                    });
                }
            }
            catch (...)
            {
                queue.TryEnqueue([weak, epoch] {
                    if (auto self = weak.lock(); self && self->run == epoch)
                        self->failure();
                });
            }
        });
        schedule();
    }
    void show_metadata(const std::shared_ptr<Metadata> &info)
    {
        metadata = info;
        updating = true;
        title.Text(info->family);
        subtitle.Text(std::wstring(info->style) + L" · " + std::filesystem::path(path).extension().wstring());
        faces.Items().Clear();
        for (unsigned i = 0; i < info->count; ++i)
            faces.Items().Append(box_value(info->faces[i]));
        faces.SelectedIndex(static_cast<int>(info->selected));
        faces.IsEnabled(info->count > 1);
        weight.Minimum(info->variable ? info->minimum : 1);
        weight.Maximum(info->variable ? info->maximum : 1000);
        weight.Value(info->weight);
        weight.IsEnabled(info->variable && info->maximum > info->minimum);
        refresh_installation();
        information.Children().Clear();
        for (const auto group : {L"Identity", L"Properties", L"Legal"})
        {
            bool heading = false;
            for (unsigned i = 0; i < info->entry_count; ++i)
            {
                const auto &row = info->entries[i];
                const std::wstring_view key(row.key);
                const auto category =
                    (key == L"Copyright" || key == L"License") ? L"Legal"
                    : (key == L"Weight" || key == L"Stretch" || key == L"Glyphs" || key == L"Axes")
                        ? L"Properties"
                        : L"Identity";
                if (std::wstring_view(group) != category)
                    continue;
                if (!heading)
                {
                    information.Children().Append(label(group, true));
                    heading = true;
                }
                auto name = label(row.key);
                name.Opacity(0.65);
                name.FontSize(12);
                information.Children().Append(name);
                TextBlock value;
                value.Text(row.value);
                value.TextWrapping(TextWrapping::Wrap);
                value.IsTextSelectionEnabled(true);
                value.Margin({0, 0, 0, 8});
                information.Children().Append(value);
            }
        }
        updating = false;
    }
    void show(const Request &request, const Response &response, const std::vector<std::byte> &pixels)
    {
        Media::Imaging::WriteableBitmap bitmap(static_cast<int>(response.width),
                                               static_cast<int>(response.height));
        auto access = bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
        BYTE *target{};
        check_hresult(access->Buffer(&target));
        memcpy(target, pixels.data(), pixels.size());
        bitmap.Invalidate();
        image.Source(bitmap);
        image.Width(response.width / request.scale);
        image.Height(response.height / request.scale);
        canvas.Width(image.Width());
        canvas.Height(response.content_height);
        Canvas::SetTop(image, request.offset);
        retry.Visibility(Visibility::Collapsed);
        std::wstring message;
        if (response.missing)
            message = text(L"Missing");
        status.Text(message);
        status.Opacity(1);
        notify(contracts::components::ComponentViewState::ready);
    }
    void notify(contracts::components::ComponentViewState state) noexcept
    {
        if (view_host.state_changed)
            view_host.state_changed(view_host.context, view_host.generation, state);
    }
    void failure()
    {
        status.Text(text(L"Failed"));
        status.Opacity(1);
        retry.Visibility(Visibility::Visible);
        notify(contracts::components::ComponentViewState::failed);
    }
    void close_worker()
    {
        ++run;
        ++serial;
        if (worker) worker->cancel();
        worker = nullptr;
    }
    void close()
    {
        closed = true;
        view_host.state_changed = nullptr;
        scale_changed.revoke();
        if (contrast_token.value)
        {
            ui_settings.ColorValuesChanged(contrast_token);
            contrast_token = {};
        }
        if (timer)
            timer.Stop();
        close_worker();
    }
    void set_language(const wchar_t *tag)
    {
        language = tag;
        context.QualifierValues().Insert(L"Language", language);
        updating = true;
        Automation::AutomationProperties::SetName(faces, text(L"Face"));
        size_label.Text(text(L"Size"));
        weight_label.Text(text(L"Weight"));
        metadata_title.Text(text(L"Metadata"));
        refresh_installation();
        retry.Content(box_value(text(L"Retry")));
        updating = false;
        if (metadata)
        {
            const auto current_weight = weight.Value();
            show_metadata(metadata);
            updating = true;
            weight.Value(current_weight);
            updating = false;
        }
        Automation::AutomationProperties::SetName(size, text(L"Size"));
        Automation::AutomationProperties::SetName(weight, text(L"Weight"));
        schedule();
    }
};
using Session = std::shared_ptr<View>;
HRESULT WINAPI create(const wchar_t *path, const wchar_t *language,
                      const contracts::components::ComponentViewHost* host, IUnknown **element,
                      std::uint64_t *token) noexcept
{
    if (!path || !language || !host || host->size < sizeof(*host) || !element || !token)
        return E_INVALIDARG;
    *element = nullptr;
    *token = 0;
    try
    {
        auto view = std::make_shared<View>();
        view->view_host = *host;
        view->initialize(path, language);
        auto session = std::make_unique<Session>(view);
        view->root.as<IUnknown>().copy_to(element);
        *token = reinterpret_cast<std::uint64_t>(session.release());
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}
void WINAPI language(std::uint64_t token, const wchar_t *value) noexcept
{
    try
    {
        if (token && value)
            (*reinterpret_cast<Session *>(token))->set_language(value);
    }
    catch (...)
    {
    }
}
void WINAPI close(std::uint64_t token) noexcept
{
    if (token)
    {
        std::unique_ptr<Session> session(reinterpret_cast<Session *>(token));
        (*session)->close();
    }
}
} // namespace
std::filesystem::path directory()
{
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&directory), &module);
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
const contracts::components::ComponentViewApi &view_api()
{
    static const contracts::components::ComponentViewApi api{
        .create = create, .set_language = language, .close = close};
    return api;
}
void shutdown_views() noexcept
{
    for (const auto &worker : workers) worker->cancel();
    workers.clear();
}
} // namespace glance::font
