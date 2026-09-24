#include "view.h"
#include "client.h"
#include <filesystem>
#include <future>
#include <optional>
#include <map>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>

namespace glance::executable
{
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace
{
struct Task { std::shared_ptr<std::atomic_bool> cancel; std::future<void> future; };
std::vector<Task> tasks;
std::filesystem::path directory()
{
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&directory), &module);
    std::wstring path(32768, L'\0'); path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
TextBlock label(std::wstring_view value, double size = 14)
{
    TextBlock block; block.Text(hstring(value)); block.FontSize(size); block.TextWrapping(TextWrapping::Wrap);
    return block;
}

std::wstring clean(std::wstring_view value)
{
    constexpr std::wstring_view whitespace = L" \t\r\n\u00A0\u200B\uFEFF";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::wstring_view::npos) return {};
    return std::wstring(value.substr(first, value.find_last_not_of(whitespace) - first + 1));
}
fire_and_forget load_icon(Image image, std::vector<std::byte> bytes)
{
    try
    {
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        Windows::Storage::Streams::DataWriter writer(stream);
        writer.WriteBytes(array_view<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
            reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size()));
        co_await writer.StoreAsync(); writer.DetachStream(); stream.Seek(0);
        Media::Imaging::BitmapImage bitmap; bitmap.DecodePixelWidth(64);
        co_await bitmap.SetSourceAsync(stream);
        image.Source(bitmap); image.Visibility(Visibility::Visible);
    }
    catch (...) { image.Visibility(Visibility::Collapsed); }
}

struct View : std::enable_shared_from_this<View>
{
    contracts::components::ComponentViewHost host;
    Grid root;
    Border identity_header;
    ScrollViewer scroll;
    StackPanel content;
    TextBlock status;
    Button toggle;
    std::map<const Table*, std::vector<std::size_t>> detail_columns;
    ListView detail_list;
    DataTemplate detail_body_template{nullptr};
    DataTemplate detail_heading_template{nullptr};
    struct DetailRow { const Table* table{}; const Row* row{}; std::wstring heading; };
    std::vector<DetailRow> detail_rows;
    std::map<const Table*, bool> collapsed_sections;
    Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable> detail_items{nullptr};
    Microsoft::Windows::ApplicationModel::Resources::ResourceManager resources{nullptr};
    Microsoft::Windows::ApplicationModel::Resources::ResourceContext context{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    std::wstring path, language;
    std::optional<Result> result;
    std::optional<Result> detailed_result;
    std::shared_ptr<std::atomic_bool> cancellation;
    bool closed{};
    bool detailed{}, loading{};
    hstring text(std::wstring_view key)
    {
        const auto value = resources.MainResourceMap().GetSubtree(L"Resources").TryGetValue(hstring(key), context);
        return value ? value.ValueAsString() : hstring(key);
    }
    void notify(contracts::components::ComponentViewState state)
    {
        if (!closed && host.state_changed) host.state_changed(host.context, host.generation, state);
    }
    void set_status(hstring const& value)
    {
        status.Text(value);
        status.Visibility(value.empty() ? Visibility::Collapsed : Visibility::Visible);
    }
    void initialize(const wchar_t* file, const wchar_t* locale)
    {
        path = file; language = locale;
        resources = Microsoft::Windows::ApplicationModel::Resources::ResourceManager((directory() / L"resources.pri").wstring());
        context = resources.CreateResourceContext(); context.QualifierValues().Insert(L"Language", language);
        dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        root.Padding({24, 18, 24, 12});
        RowDefinition heading; heading.Height({1, GridUnitType::Auto}); root.RowDefinitions().Append(heading);
        RowDefinition main; main.Height({1, GridUnitType::Star}); root.RowDefinitions().Append(main);
        RowDefinition footer; footer.Height({1, GridUnitType::Auto}); root.RowDefinitions().Append(footer);
        content.Spacing(18); scroll.Content(content); scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto); Grid::SetRow(scroll, 1); root.Children().Append(scroll);
        status.Opacity(0.65); status.VerticalAlignment(VerticalAlignment::Center); status.TextWrapping(TextWrapping::Wrap);
        status.Margin({0, 12, 0, 0}); status.Visibility(Visibility::Collapsed);
        Grid header_bar; header_bar.ColumnSpacing(18); header_bar.Margin({0, 0, 0, 18});
        ColumnDefinition identity; identity.Width({1, GridUnitType::Star}); header_bar.ColumnDefinitions().Append(identity);
        ColumnDefinition action; action.Width({1, GridUnitType::Auto}); header_bar.ColumnDefinitions().Append(action);
        header_bar.Children().Append(identity_header);
        toggle.HorizontalAlignment(HorizontalAlignment::Right); toggle.VerticalAlignment(VerticalAlignment::Center);
        toggle.Content(box_value(text(L"ShowFullInformation")));
        toggle.IsTabStop(false); toggle.AllowFocusOnInteraction(false);
        toggle.IsEnabled(false); Grid::SetColumn(toggle, 1); header_bar.Children().Append(toggle);
        root.Children().Append(header_bar);
        Grid::SetRow(status, 2); root.Children().Append(status);
        detail_list.Visibility(Visibility::Collapsed); detail_list.SelectionMode(ListViewSelectionMode::None);
        detail_body_template = Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Border Background="{ThemeResource ExpanderContentBackground}" BorderBrush="{ThemeResource ExpanderContentBorderBrush}"/></DataTemplate>)").as<DataTemplate>();
        detail_heading_template = Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><ToggleButton Style="{StaticResource ExpanderHeaderDownStyle}" Background="{ThemeResource ExpanderHeaderBackground}" BorderBrush="{ThemeResource ExpanderHeaderBorderBrush}" BorderThickness="1" Padding="16,0,0,0" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" MinHeight="48" IsTabStop="False" AllowFocusOnInteraction="False"/></DataTemplate>)").as<DataTemplate>();
        detail_list.ItemContainerTransitions(Media::Animation::TransitionCollection{});
        detail_list.ItemTemplate(Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><StackPanel/></DataTemplate>)").as<DataTemplate>());
        detail_list.ItemContainerStyle(Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem"><Setter Property="HorizontalContentAlignment" Value="Stretch"/><Setter Property="Padding" Value="0"/><Setter Property="MinHeight" Value="0"/></Style>)").as<Style>());
        ScrollViewer::SetHorizontalScrollBarVisibility(detail_list, ScrollBarVisibility::Disabled);
        ScrollViewer::SetHorizontalScrollMode(detail_list, ScrollMode::Disabled);
        Grid::SetRow(detail_list, 1); root.Children().Append(detail_list);
        auto weak = weak_from_this();
        detail_list.ContainerContentChanging([weak](auto&&, ContainerContentChangingEventArgs const& args) {
            if (auto self = weak.lock())
            {
                if (args.InRecycleQueue()) return;
                self->realize(args); args.Handled(true);
            }
        });
        toggle.Click([weak](auto&&, auto&&) {
            if (auto self = weak.lock())
            {
                self->detailed = !self->detailed;
                self->toggle.Content(box_value(self->text(self->detailed ? L"BackToSummary" : L"ShowFullInformation")));
                self->scroll.Visibility(self->detailed ? Visibility::Collapsed : Visibility::Visible);
                self->detail_list.Visibility(self->detailed ? Visibility::Visible : Visibility::Collapsed);
                if (self->detailed && !self->detailed_result && !self->loading) self->load(true);
                else self->render();
            }
        });
        load(false);
    }
    void load(bool full)
    {
        auto weak = weak_from_this(); loading = true;
        set_status(text(L"Loading")); notify(contracts::components::ComponentViewState::loading);
        cancellation = std::make_shared<std::atomic_bool>(false); auto cancelled = cancellation;
        std::erase_if(tasks, [](Task& task) { return task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
        auto queue = dispatcher; const auto host_path = (directory() / L"Glance.ExecutableHost.exe").wstring();
        auto query = std::make_shared<Query>(); wcscpy_s(query->path, path.c_str()); query->detailed = full;
        tasks.push_back({cancelled, std::async(std::launch::async, [weak, queue, host_path, query, cancelled, full] {
            Result data;
            try { data = inspect(host_path, *query, *cancelled); }
            catch (const hresult_error& error) { data.table.state = L"Error"; data.table.detail = error.message(); }
            catch (...) { data.table.state = L"Error"; }
            if (*cancelled) return;
            queue.TryEnqueue([weak, full, data = std::move(data)]() mutable {
                if (auto self = weak.lock(); self && !self->closed)
                {
                    try
                    {
                        self->loading = false;
                        if (data.summary.title.empty()) { self->set_status(data.table.detail.empty() ? self->text(data.table.state) : hstring(data.table.detail)); self->notify(full ? contracts::components::ComponentViewState::ready : contracts::components::ComponentViewState::failed); return; }
                        if (full) self->detailed_result = std::move(data);
                        else self->result = std::move(data);
                        self->toggle.IsEnabled(true); self->render(); self->notify(contracts::components::ComponentViewState::ready);
                    }
                    catch (...) { self->set_status(self->text(L"Error")); self->notify(contracts::components::ComponentViewState::failed); }
                }
            });
        })});
    }
    Grid detail_grid(const Table& table, const Row* row)
    {
        Grid grid; grid.Padding({8, 6, 8, 6}); grid.ColumnSpacing(16);
        const auto& columns = detail_columns.at(&table);
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            const auto index = columns[i];
            ColumnDefinition column;
            column.Width({1, GridUnitType::Star});
            grid.ColumnDefinitions().Append(column);
            std::wstring value;
            if (!row) value = index < table.columns.size() ? std::wstring(text(table.columns[index])) : std::wstring(text(L"Value"));
            else if (index < row->cells.size()) value = clean(row->cells[index]);
            if (row && table.field_keys && index == 0) value = text(value);
            auto cell = label(value, row ? 13 : 12);
            cell.TextWrapping(TextWrapping::Wrap); cell.IsTextSelectionEnabled(row != nullptr);
            if (!row) { cell.Opacity(0.65); cell.FontWeight(Windows::UI::Text::FontWeight{600}); }
            Grid::SetColumn(cell, static_cast<int>(i)); grid.Children().Append(cell);
        }
        return grid;
    }
    void fill_detail_row(StackPanel const& panel, std::uint32_t index)
    {
        if (index >= detail_rows.size()) return;
        const auto& entry = detail_rows[index];
        panel.Children().Clear(); panel.Spacing(8); panel.Margin({0, 0, 0, 0});
        if (!entry.row)
        {
            auto heading = label(std::wstring(text(entry.heading)) + L" (" + std::to_wstring(entry.table->rows.size()) + L")", 18);
            heading.FontWeight(Windows::UI::Text::FontWeight{600});
            auto section = detail_heading_template.LoadContent().as<Primitives::ToggleButton>();
            section.Margin({0, 12, 0, 0}); section.Content(heading);
            StackPanel columns; columns.Spacing(8);
            if (entry.table->state != L"Complete") columns.Children().Append(label(text(entry.table->state)));
            columns.Children().Append(detail_grid(*entry.table, nullptr));
            const bool expanded = !collapsed_sections[entry.table];
            section.IsChecked(expanded);
            const bool has_rows = index + 1 < detail_rows.size() && detail_rows[index + 1].row;
            section.CornerRadius(expanded ? CornerRadius{4, 4, 0, 0} : CornerRadius{4, 4, 4, 4});
            auto column_body = detail_body_template.LoadContent().as<Border>();
            column_body.BorderThickness({1, 0, 1, has_rows ? 0.0 : 1.0});
            column_body.Padding({16, 16, 16, has_rows ? 0.0 : 16.0});
            if (!has_rows) column_body.CornerRadius({0, 0, 4, 4});
            column_body.Child(columns);
            column_body.Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
            const auto weak = weak_from_this();
            section.Click([weak, index, column_body](auto&& sender, auto&&) {
                const auto button = sender.template as<Primitives::ToggleButton>();
                const bool open = button.IsChecked().Value();
                button.CornerRadius(open ? CornerRadius{4, 4, 0, 0} : CornerRadius{4, 4, 4, 4});
                column_body.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
                if (const auto self = weak.lock(); self && !self->closed) self->set_section_expanded(index, open);
            });
            panel.Spacing(0);
            panel.Children().Append(section);
            panel.Children().Append(column_body);
            return;
        }
        auto body = detail_body_template.LoadContent().as<Border>();
        const bool last = index + 1 == detail_rows.size() || !detail_rows[index + 1].row;
        body.BorderThickness({1, 0, 1, last ? 1.0 : 0.0});
        body.Padding({16, 0, 16, last ? 16.0 : 0.0});
        if (last) body.CornerRadius({0, 0, 4, 4});
        body.Child(detail_grid(*entry.table, entry.row));
        panel.Children().Append(body);
    }
    void set_section_expanded(std::uint32_t index, bool expanded)
    {
        if (!detail_items || index >= detail_rows.size() || detail_rows[index].row) return;
        auto& collapsed = collapsed_sections[detail_rows[index].table];
        if (collapsed == !expanded) return;
        collapsed = !expanded;
        std::uint32_t position{};
        while (position < detail_items.Size() && unbox_value<std::uint32_t>(detail_items.GetAt(position)) != index) ++position;
        if (position == detail_items.Size()) return;
        ++position;
        if (expanded)
        {
            for (auto next = index + 1; next < detail_rows.size() && detail_rows[next].row; ++next)
                detail_items.InsertAt(position++, box_value(next));
        }
        else
        {
            while (position < detail_items.Size() && detail_rows[unbox_value<std::uint32_t>(detail_items.GetAt(position))].row)
                detail_items.RemoveAt(position);
        }
    }
    void realize(ContainerContentChangingEventArgs const& args)
    {
        if (const auto panel = args.ItemContainer().ContentTemplateRoot().try_as<StackPanel>())
            fill_detail_row(panel, unbox_value<std::uint32_t>(args.Item()));
    }
    void render_details()
    {
        if (!detailed_result) return;
        detail_list.ItemsSource(nullptr); detail_rows.clear(); detail_columns.clear();
        const auto append = [&](const std::wstring& name, const Table& table) {
            if (table.rows.empty() && table.state == L"Complete") return;
            collapsed_sections.try_emplace(&table, name != L"Overview");
            auto& columns = detail_columns[&table];
            for (std::size_t i = 0; i < table.columns.size(); ++i)
                if (std::any_of(table.rows.begin(), table.rows.end(), [i](const Row& row) {
                    return i < row.cells.size() && !clean(row.cells[i]).empty();
                })) columns.push_back(i);
            detail_rows.push_back({&table, nullptr, name});
            for (const auto& row : table.rows)
                if (std::any_of(row.cells.begin(), row.cells.end(), [](const auto& cell) { return !clean(cell).empty(); }))
                    detail_rows.push_back({&table, &row, {}});
        };
        append(L"Overview", detailed_result->summary.overview);
        for (const auto& [name, table] : detailed_result->details) append(name, table);
        std::vector<Windows::Foundation::IInspectable> items; items.reserve(detail_rows.size());
        for (std::uint32_t i = 0; i < detail_rows.size(); ++i)
            if (!detail_rows[i].row || !collapsed_sections[detail_rows[i].table]) items.push_back(box_value(i));
        detail_items = single_threaded_observable_vector(std::move(items));
        detail_list.ItemsSource(detail_items);
        set_status(detailed_result->table.state == L"Complete" ? L"" : text(detailed_result->table.state));
    }
    std::wstring field(std::wstring_view key)
    {
        for (const auto& row : result->summary.overview.rows) if (row.cells.size() >= 2 && row.cells[0] == key) return clean(row.cells[1]);
        return {};
    }
    void add(StackPanel panel, std::wstring_view key, const std::wstring& raw, const std::wstring& count = {})
    {
        const auto value = clean(raw);
        if (value.empty()) return;
        Grid entry; entry.ColumnSpacing(16); entry.Padding({0, 4, 0, 4});
        ColumnDefinition name_column; name_column.Width({132, GridUnitType::Pixel}); entry.ColumnDefinitions().Append(name_column);
        ColumnDefinition value_column; value_column.Width({1, GridUnitType::Star}); entry.ColumnDefinitions().Append(value_column);
        auto name = label(std::wstring(text(key)) + (count.empty() ? L"" : L" (" + count + L")"), 13);
        name.Opacity(0.6); entry.Children().Append(name);
        auto data = label(value); data.IsTextSelectionEnabled(true); data.MaxLines(3);
        data.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetColumn(data, 1); entry.Children().Append(data); panel.Children().Append(entry);
    }
    void render()
    {
        if (detailed)
        {
            if (loading) set_status(text(L"Loading"));
            render_details(); return;
        }
        if (!result) return;
        content.Children().Clear();
        const auto& summary = result->summary;
        const auto description = clean(summary.title);
        const auto heading = description.size() > 80
            ? (field(L"Product").empty() ? std::filesystem::path(path).filename().wstring() : field(L"Product"))
            : description;
        Grid header; header.ColumnSpacing(summary.icon.empty() ? 0 : 18);
        ColumnDefinition icon_column; icon_column.Width({1, GridUnitType::Auto}); header.ColumnDefinitions().Append(icon_column);
        ColumnDefinition title_column; title_column.Width({1, GridUnitType::Star}); header.ColumnDefinitions().Append(title_column);
        Image icon; icon.Width(64); icon.Height(64); icon.VerticalAlignment(VerticalAlignment::Top);
        icon.Visibility(Visibility::Collapsed); header.Children().Append(icon);
        if (!summary.icon.empty()) load_icon(icon, summary.icon);
        StackPanel titles; titles.Spacing(6); Grid::SetColumn(titles, 1); header.Children().Append(titles);
        auto title = label(heading, 24); title.FontWeight(Windows::UI::Text::FontWeight{600});
        title.MaxLines(2); title.TextTrimming(TextTrimming::CharacterEllipsis); titles.Children().Append(title);
        std::wstring subtitle_text;
        for (const auto& raw : {summary.type, summary.architecture, summary.managed ? std::wstring(L".NET") : std::wstring{}, summary.version})
        {
            const auto value = clean(raw); if (value.empty()) continue;
            if (!subtitle_text.empty()) subtitle_text += L" · ";
            subtitle_text += value;
        }
        if (!subtitle_text.empty()) { auto subtitle = label(subtitle_text, 13); subtitle.Opacity(0.65); titles.Children().Append(subtitle); }
        identity_header.Child(header);
        if (!description.empty() && heading != description) content.Children().Append(label(description));
        StackPanel basic; basic.Spacing(2);
        for (const auto key : {L"Product", L"Company", L"Original"}) add(basic, key, field(key));
        if (!summary.dll) { add(basic, L"Subsystem", field(L"Subsystem")); add(basic, L"Permission", field(L"Permission")); }
        wchar_t size[64]{}; swprintf_s(size, L"%.2f MiB", summary.identity.size / (1024.0 * 1024)); add(basic, L"Size", size);
        add(basic, L"Signer", field(L"Signer")); content.Children().Append(basic);
        StackPanel contents; contents.Spacing(2); unsigned capabilities{};
        for (const auto& row : result->table.rows)
        {
            if (row.cells.size() < 2 || clean(row.cells[1]).empty()) continue;
            const auto& key = row.cells[0];
            if (key.starts_with(L"Capability_"))
            {
                if (capabilities++ >= 3) continue;
            }
            else
            {
                if (capabilities && key == L"Summary_Interfaces") continue;
                if (summary.dll && capabilities && (key == L"Summary_Icons" || key == L"Summary_Strings")) continue;
            }
            add(contents, key, row.cells[1], row.cells.size() > 2 ? row.cells[2] : L"");
        }
        if (contents.Children().Size())
        {
            auto section = label(text(summary.dll ? L"Summary_Library" : L"Summary_Program"), 16);
            section.FontWeight(Windows::UI::Text::FontWeight{600}); content.Children().Append(section);
            content.Children().Append(contents);
            if (capabilities)
            {
                auto hint = label(text(summary.dll ? L"Summary_ExportEvidence" : L"Summary_ImportEvidence"), 12);
                hint.Opacity(0.6); content.Children().Append(hint);
            }
        }
        set_status(result->table.state == L"Complete" ? L"" : text(result->table.state));
    }
    void set_language(const wchar_t* value)
    {
        language = value; context.QualifierValues().Insert(L"Language", language);
        toggle.Content(box_value(text(detailed ? L"BackToSummary" : L"ShowFullInformation"))); render();
    }
    void close() { closed = true; if (cancellation) *cancellation = true; host.state_changed = nullptr; }
    ~View() { close(); }
};
using Session = std::shared_ptr<View>;
HRESULT WINAPI create(const wchar_t* path, const wchar_t* language, const contracts::components::ComponentViewHost* host, IUnknown** element, std::uint64_t* token) noexcept
{
    if (!path || !language || !host || !element || !token) return E_INVALIDARG;
    *element = nullptr; *token = 0;
    try
    {
        auto view = std::make_shared<View>(); view->host = *host; view->initialize(path, language);
        auto session = std::make_unique<Session>(view); view->root.as<IUnknown>().copy_to(element);
        *token = reinterpret_cast<std::uint64_t>(session.release()); return S_OK;
    }
    catch (...) { return to_hresult(); }
}
void WINAPI language(std::uint64_t token, const wchar_t* value) noexcept
{ try { if (token && value) (*reinterpret_cast<Session*>(token))->set_language(value); } catch (...) {} }
void WINAPI close(std::uint64_t token) noexcept
{ if (token) { std::unique_ptr<Session> session(reinterpret_cast<Session*>(token)); (*session)->close(); } }
}
const contracts::components::ComponentViewApi& view_api()
{
    static const contracts::components::ComponentViewApi api{.create = create, .set_language = language, .close = close}; return api;
}
void shutdown_views() noexcept
{
    for (auto& task : tasks) *task.cancel = true;
    tasks.clear();
}
}
