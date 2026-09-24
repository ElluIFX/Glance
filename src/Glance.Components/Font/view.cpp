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
    TextBox editor;
    TextBlock size_label, weight_label;
    Expander details;
    ScrollViewer scroll, metadata_scroll;
    Canvas canvas;
    Image image;
    Button retry;
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
    std::shared_ptr<Client> client;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopped{}, pending{}, updating{}, composition{}, narrow{}, text_initialized{}, editing{};
    Request next;
    std::uint64_t generation{};
    std::uint64_t serial{};
    std::uint64_t run{};
    unsigned desired_face{};
    unsigned toolbar_layout{};
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
        for (unsigned i = 0; i < 2; ++i)
        {
            RowDefinition row;
            row.Height({1, GridUnitType::Auto});
            controls.RowDefinitions().Append(row);
        }
        controls.Children().Append(choices);
        faces.Header(box_value(text(L"Face")));
        faces.Width(200);
        faces.VerticalAlignment(VerticalAlignment::Bottom);
        faces.Visibility(Visibility::Collapsed);
        choices.Children().Append(faces);
        for (unsigned i = 0; i < 3; ++i)
        {
            ColumnDefinition column;
            column.Width({1, GridUnitType::Auto});
            adjustments.ColumnDefinitions().Append(column);
        }
        Grid::SetRow(adjustments, 1);
        controls.Children().Append(adjustments);
        const auto add_adjustment = [&](Grid group, TextBlock label, NumberBox number,
                                        const wchar_t* key) {
            group.ColumnSpacing(12);
            group.HorizontalAlignment(HorizontalAlignment::Stretch);
            group.VerticalAlignment(VerticalAlignment::Bottom);
            for (unsigned i = 0; i < 2; ++i)
            {
                ColumnDefinition column;
                column.Width({i == 0 ? 1.0 : 100.0, i == 0 ? GridUnitType::Auto : GridUnitType::Pixel});
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
                Width="1" Height="24" Margin="20,0" VerticalAlignment="Center"/>)").as<Border>();
        Grid::SetColumn(adjustment_separator, 1);
        adjustments.Children().Append(adjustment_separator);
        size.Minimum(8);
        size.Maximum(512);
        size.Value(32);
        weight.Visibility(Visibility::Collapsed);
        weight_group.Visibility(Visibility::Collapsed);
        Grid::SetRow(body, 2);
        root.Children().Append(body);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(canvas);
        canvas.Children().Append(image);
        image.Stretch(Media::Stretch::Fill);
        body.Children().Append(scroll);
        scroll.IsTabStop(true);
        scroll.Background(Media::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
        ToolTipService::SetToolTip(scroll, box_value(text(L"EditSample")));
        Automation::AutomationProperties::SetName(scroll, text(L"EditSample"));
        editor.AcceptsReturn(true);
        editor.TextWrapping(TextWrapping::Wrap);
        editor.MaxLength(static_cast<int>(std::size(Request{}.text) - 1));
        editor.FontSize(20);
        editor.PlaceholderText(text(L"SampleHint"));
        editor.VerticalAlignment(VerticalAlignment::Stretch);
        editor.Visibility(Visibility::Collapsed);
        ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
        Automation::AutomationProperties::SetName(editor, text(L"Sample"));
        body.Children().Append(editor);
        details.Header(box_value(text(L"Metadata")));
        details.IsExpanded(true);
        details.HorizontalAlignment(HorizontalAlignment::Stretch);
        details.VerticalAlignment(VerticalAlignment::Top);
        metadata_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        metadata_scroll.Content(information);
        information.Spacing(6);
        information.Margin({12, 0, 12, 12});
        details.Content(metadata_scroll);
        Grid::SetColumn(details, 1);
        Grid::SetRowSpan(details, 4);
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
        root.SizeChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
            {
                self->layout();
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
                self->layout();
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
                self->layout();
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
                    value = 32;
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
        editor.TextCompositionStarted([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
                self->composition = true;
        });
        editor.TextCompositionEnded([weak](auto &&, auto &&) {
            if (auto self = weak.lock())
            {
                self->composition = false;
                self->schedule();
            }
        });
        editor.TextChanged([weak](auto &&, auto &&) {
            if (auto self = weak.lock(); self && !self->composition)
                self->schedule();
        });
        scroll.Tapped([weak](auto&&, Input::TappedRoutedEventArgs const& args) {
            if (auto self = weak.lock())
            {
                self->begin_edit();
                args.Handled(true);
            }
        });
        scroll.KeyDown([weak](auto&&, Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter ||
                args.Key() == winrt::Windows::System::VirtualKey::F2)
            {
                if (auto self = weak.lock()) self->begin_edit();
                args.Handled(true);
            }
        });
        editor.LostFocus([weak](auto&&, auto&&) {
            if (auto self = weak.lock(); self && self->root.XamlRoot())
            {
                auto focus = Input::FocusManager::GetFocusedElement(self->root.XamlRoot())
                    .try_as<DependencyObject>();
                while (focus)
                {
                    if (focus == self->editor) return;
                    if (focus == self->root)
                    {
                        self->finish_edit();
                        return;
                    }
                    focus = Media::VisualTreeHelper::GetParent(focus);
                }
            }
        });
        root.Tapped([weak](auto&&, Input::TappedRoutedEventArgs const& args) {
            if (auto self = weak.lock(); self && self->editing)
            {
                auto source = args.OriginalSource().try_as<DependencyObject>();
                while (source)
                {
                    if (source == self->editor) return;
                    source = Media::VisualTreeHelper::GetParent(source);
                }
                self->finish_edit();
            }
        });
        editor.KeyDown([weak](auto&&, Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                if (auto self = weak.lock(); self && !self->composition)
                {
                    self->finish_edit();
                    self->scroll.Focus(FocusState::Programmatic);
                    args.Handled(true);
                }
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
    void layout()
    {
        const bool compact_layout = root.ActualWidth() < 820;
        const double content_width = root.ActualWidth() - 48 - (compact_layout ? 0 : 304);
        const bool variable = metadata && metadata->variable;
        const bool has_faces = faces.Visibility() == Visibility::Visible;
        choices.Visibility(has_faces ? Visibility::Visible : Visibility::Collapsed);
        const bool inline_controls = !has_faces || content_width - 32 >= 200 +
            (variable ? 390 : 180) + 24;
        const auto toolbar_key = 1U + unsigned(inline_controls);
        if (toolbar_layout != toolbar_key)
        {
            toolbar_layout = toolbar_key;
            controls.ColumnDefinitions().Clear();
            controls.RowDefinitions().Clear();
            for (unsigned i = 0; i < (inline_controls ? 1U : 2U); ++i)
            {
                RowDefinition row;
                row.Height({1, GridUnitType::Auto});
                controls.RowDefinitions().Append(row);
            }
            for (unsigned i = 0; i < (inline_controls ? 2U : 1U); ++i)
            {
                ColumnDefinition column;
                column.Width({1, inline_controls && i == 0 ? GridUnitType::Auto : GridUnitType::Star});
                controls.ColumnDefinitions().Append(column);
            }
            Grid::SetRow(adjustments, inline_controls ? 0 : 1);
            Grid::SetColumn(adjustments, inline_controls ? 1 : 0);
        }
        controls.ColumnSpacing(has_faces && inline_controls ? 24 : 0);
        weight_group.Visibility(variable ? Visibility::Visible : Visibility::Collapsed);
        adjustment_separator.Visibility(variable ? Visibility::Visible : Visibility::Collapsed);
        if (compact_layout != narrow)
        {
            narrow = compact_layout;
            details.IsExpanded(!compact_layout);
        }
        root.ColumnSpacing(compact_layout ? 0 : 24);
        root.ColumnDefinitions().GetAt(1).Width({compact_layout ? 0.0 : 280.0, GridUnitType::Pixel});
        Grid::SetColumn(details, compact_layout ? 0 : 1);
        Grid::SetRow(details, compact_layout ? 4 : 0);
        Grid::SetRowSpan(details, compact_layout ? 1 : 4);
        details.Margin({0, compact_layout ? 12.0f : 0.0f, 0, 0});
        metadata_scroll.MaxHeight(compact_layout ? 180 : std::max(64.0, root.ActualHeight() - 104));
    }
    void begin_edit()
    {
        if (!metadata || editing) return;
        editing = true;
        editor.Visibility(Visibility::Visible);
        scroll.Opacity(0);
        scroll.IsHitTestVisible(false);
        status.Text(text(L"EditingHint"));
        editor.Focus(FocusState::Programmatic);
    }
    void finish_edit()
    {
        if (!editing) return;
        editing = false;
        editor.Visibility(Visibility::Collapsed);
        scroll.Opacity(1);
        scroll.IsHitTestVisible(true);
        scroll.ChangeView(nullptr, 0.0, nullptr, true);
        schedule();
    }
    void schedule()
    {
        if (!updating && !composition && timer)
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
        request.custom_text = text_initialized;
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
        const auto text_value = editor.Text();
        wcsncpy_s(request.text, text_value.c_str(), _TRUNCATE);
        {
            std::scoped_lock lock(mutex);
            if (stopped)
                return;
            next = request;
            pending = true;
            generation = serial;
        }
        condition.notify_one();
    }
    void restart()
    {
        close_worker();
        notify(contracts::components::ComponentViewState::loading);
        metadata.reset();
        status.Text(text(L"Loading"));
        retry.Visibility(Visibility::Collapsed);
        {
            std::scoped_lock lock(mutex);
            stopped = false;
            pending = false;
        }
        client = std::make_shared<Client>();
        auto connection = client;
        auto weak = weak_from_this();
        auto queue = dispatcher;
        const auto file = path;
        const auto host = (directory() / L"Glance.FontHost.exe").wstring();
        const auto epoch = run;
        worker = std::thread([this, connection, weak, queue, file, host, epoch] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                connection->open(host, file);
                unsigned face = UINT_MAX;
                std::shared_ptr<Metadata> info;
                for (;;)
                {
                    Request request;
                    std::uint64_t version{};
                    {
                        std::unique_lock lock(mutex);
                        condition.wait(lock, [&] { return stopped || pending; });
                        if (stopped)
                            return;
                        request = next;
                        version = generation;
                        pending = false;
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
        faces.Visibility(info->count > 1 ? Visibility::Visible : Visibility::Collapsed);
        weight.Minimum(info->variable ? info->minimum : 1);
        weight.Maximum(info->variable ? info->maximum : 1000);
        weight.Value(info->weight);
        weight.Visibility(info->variable ? Visibility::Visible : Visibility::Collapsed);
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
        if (!text_initialized)
        {
            editor.Text(info->sample);
            text_initialized = true;
        }
        updating = false;
        layout();
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
        if (message.empty()) message = text(L"EditSample");
        status.Text(editing ? text(L"EditingHint") : hstring(message));
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
        retry.Visibility(Visibility::Visible);
        notify(contracts::components::ComponentViewState::failed);
    }
    void close_worker()
    {
        ++run;
        ++serial;
        {
            std::scoped_lock lock(mutex);
            stopped = true;
        }
        condition.notify_all();
        if (client)
            client->cancel();
        if (worker.joinable())
            worker.join();
        client.reset();
    }
    void close()
    {
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
        faces.Header(box_value(text(L"Face")));
        size_label.Text(text(L"Size"));
        weight_label.Text(text(L"Weight"));
        editor.PlaceholderText(text(L"SampleHint"));
        Automation::AutomationProperties::SetName(editor, text(L"Sample"));
        Automation::AutomationProperties::SetName(scroll, text(L"EditSample"));
        ToolTipService::SetToolTip(scroll, box_value(text(L"EditSample")));
        details.Header(box_value(text(L"Metadata")));
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
} // namespace glance::font
