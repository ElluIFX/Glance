#include "pch.h"
#include "MainWindow.xaml.h"
#include "appearance_preferences.h"
#include "footer_preferences.h"
#include "fullscreen_interaction.h"
#include "generic_file_info.h"
#include "gallery_navigation.h"
#include "image_metadata_provider.h"
#include "localization.h"
#include "markdown_renderer.h"
#include "media_preview_preferences.h"
#include "component_loader.h"
#include "pan_interaction.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "shell_icon_provider.h"
#include "webview_availability.h"
#include "window_size_store.h"
#include "window_preferences.h"
#include "glance/contracts/diagnostics.h"
#include "glance/contracts/source_api.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Documents;
using namespace Microsoft::UI::Xaml::Input;

namespace
{
    winrt::hstring formatted_zoom_factor(double zoom)
    {
        std::wostringstream output;
        output << std::fixed << std::setprecision(2) << zoom << L'X';
        return winrt::hstring{ output.str() };
    }

    std::optional<std::uint64_t> parse_u64(const winrt::hstring& text)
    {
        try
        {
            std::size_t consumed{};
            const auto value = std::stoull(text.c_str(), &consumed);
            return consumed == text.size()
                ? std::optional<std::uint64_t>(value)
                : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    class AtomicCounterGuard final
    {
    public:
        explicit AtomicCounterGuard(std::atomic_uint32_t& counter) noexcept
            : counter_(counter)
        {
            counter_.fetch_add(1, std::memory_order_acq_rel);
        }

        ~AtomicCounterGuard()
        {
            counter_.fetch_sub(1, std::memory_order_acq_rel);
        }

        AtomicCounterGuard(const AtomicCounterGuard&) = delete;
        AtomicCounterGuard& operator=(const AtomicCounterGuard&) = delete;

    private:
        std::atomic_uint32_t& counter_;
    };

    struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferByteAccess : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Buffer(byte** value) = 0;
    };
    constexpr std::size_t text_chunk_bytes = 256U * 1024U;
    constexpr std::uint64_t maximum_preview_as_text_bytes = 8ULL * 1024ULL * 1024ULL;
    constexpr std::size_t retained_preview_buffer_limit_bytes = 8U * 1024U * 1024U;
    constexpr std::uint32_t folder_icon_pixel_size = 20;

    std::wstring compact_file_list_name(std::wstring_view name)
    {
        constexpr std::size_t maximum_units = 24;
        constexpr std::wstring_view ellipsis = L"...";
        const auto units = [](wchar_t character) noexcept
        {
            return character < 0x80 ? 1U : 2U;
        };

        std::size_t total_units{};
        for (const auto character : name)
        {
            total_units += units(character);
        }
        if (total_units <= maximum_units)
        {
            return std::wstring(name);
        }

        constexpr std::size_t prefix_units = 14;
        constexpr std::size_t suffix_units = maximum_units - prefix_units - ellipsis.size();
        std::size_t prefix_length{};
        std::size_t used_units{};
        while (prefix_length < name.size() &&
            used_units + units(name[prefix_length]) <= prefix_units)
        {
            used_units += units(name[prefix_length++]);
        }

        std::size_t suffix_start = name.size();
        used_units = 0;
        while (suffix_start > prefix_length &&
            used_units + units(name[suffix_start - 1]) <= suffix_units)
        {
            used_units += units(name[--suffix_start]);
        }

        std::wstring compacted(name.substr(0, prefix_length));
        compacted.append(ellipsis);
        compacted.append(name.substr(suffix_start));
        return compacted;
    }
    constexpr std::uint32_t folder_thumbnail_pixel_size = 32;
    constexpr std::size_t thumbnail_update_batch_size = 8;
    constexpr auto web_view_idle_timeout = std::chrono::minutes(1);
    constexpr auto fullscreen_chrome_interval = std::chrono::milliseconds(50);
    constexpr ULONGLONG fullscreen_chrome_hide_delay_ms = 500;
    constexpr int fullscreen_edge_reveal_dips = 4;
    constexpr int fullscreen_title_height_dips = 40;
    constexpr int fullscreen_footer_height_dips = 42;
    constexpr wchar_t web_double_click_fullscreen_enabled[] =
        L"glance:double-click-fullscreen:enabled";
    constexpr wchar_t web_double_click_fullscreen_disabled[] =
        L"glance:double-click-fullscreen:disabled";
    constexpr wchar_t web_toggle_fullscreen[] = L"glance:toggle-fullscreen";
    constexpr wchar_t web_double_click_fullscreen_script[] = LR"JS(
(() => {
  if (window.__glanceDoubleClickFullscreenInstalled) {
    return;
  }
  window.__glanceDoubleClickFullscreenInstalled = true;
  let enabled = false;
  const interactiveTags = new Set([
    "BUTTON", "INPUT", "TEXTAREA", "SELECT", "OPTION", "SUMMARY"
  ]);
  const interactiveRoles = new Set([
    "button", "checkbox", "combobox", "link", "menuitem", "radio",
    "slider", "spinbutton", "switch", "tab", "textbox"
  ]);
  window.chrome.webview.addEventListener("message", (event) => {
    if (event.data === "glance:double-click-fullscreen:enabled") {
      enabled = true;
    } else if (event.data === "glance:double-click-fullscreen:disabled") {
      enabled = false;
    }
  });
  window.addEventListener("dblclick", (event) => {
    if (!enabled || event.button !== 0) {
      return;
    }
    const interactive = event.composedPath().some((node) => {
      if (!(node instanceof Element)) {
        return false;
      }
      if (interactiveTags.has(node.tagName) || node.isContentEditable) {
        return true;
      }
      if (node.tagName === "A" && node.hasAttribute("href")) {
        return true;
      }
      return interactiveRoles.has((node.getAttribute("role") || "").toLowerCase());
    });
    if (interactive) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
    window.chrome.webview.postMessage("glance:toggle-fullscreen");
  }, true);
})();
)JS";

    std::optional<std::wstring> file_url_from_path(const std::wstring& path)
    {
        const auto initial_capacity = std::max<std::size_t>(512, path.size() * 3 + 16);
        if (initial_capacity > std::numeric_limits<DWORD>::max())
        {
            return std::nullopt;
        }

        std::wstring url(initial_capacity, L'\0');
        DWORD length = static_cast<DWORD>(url.size());
        HRESULT result = UrlCreateFromPathW(path.c_str(), url.data(), &length, 0);
        if (result == E_POINTER && length > url.size())
        {
            url.resize(length);
            result = UrlCreateFromPathW(path.c_str(), url.data(), &length, 0);
        }
        if (FAILED(result))
        {
            return std::nullopt;
        }

        url.resize(length);
        while (!url.empty() && url.back() == L'\0')
        {
            url.pop_back();
        }
        return url;
    }

    POINT window_position_for_center_offset(
        const RECT& monitor_area,
        const RECT& work_area,
        int width,
        int height,
        POINT center_offset) noexcept
    {
        const int centered_x = monitor_area.left + (monitor_area.right - monitor_area.left - width) / 2;
        const int centered_y = monitor_area.top + (monitor_area.bottom - monitor_area.top - height) / 2;
        return {
            std::clamp(
                centered_x + center_offset.x,
                work_area.left,
                std::max(work_area.left, work_area.right - width)),
            std::clamp(
                centered_y + center_offset.y,
                work_area.top,
                std::max(work_area.top, work_area.bottom - height)) };
    }

    enum class ArchiveColumnKind
    {
        name,
        type,
        modified_time,
        compressed_size,
        original_size,
    };

    struct ArchiveColumnSpec
    {
        ArchiveColumnKind kind;
        double width;
    };

    constexpr std::array folder_columns{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 110.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::modified_time, 150.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 90.0 },
    };
    constexpr std::array archive_columns_with_compressed_size{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 90.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::compressed_size, 110.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 110.0 },
    };
    constexpr std::array archive_columns_without_compressed_size{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 100.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 110.0 },
    };

    std::span<const ArchiveColumnSpec> archive_columns(
        bool is_directory,
        bool compressed_size_available) noexcept
    {
        if (is_directory)
        {
            return folder_columns;
        }
        return compressed_size_available
            ? std::span<const ArchiveColumnSpec>(archive_columns_with_compressed_size)
            : std::span<const ArchiveColumnSpec>(archive_columns_without_compressed_size);
    }

    void configure_archive_columns(
        const Grid& grid,
        std::span<const ArchiveColumnSpec> columns_to_add)
    {
        auto columns = grid.ColumnDefinitions();
        columns.Clear();
        for (const auto& spec : columns_to_add)
        {
            ColumnDefinition column;
            column.Width(spec.width == 0.0
                ? GridLength{ 1, GridUnitType::Star }
                : GridLength{ spec.width, GridUnitType::Pixel });
            columns.Append(column);
        }
    }

    int compare_case_insensitive(const std::wstring& left, const std::wstring& right) noexcept
    {
        return _wcsicmp(left.c_str(), right.c_str());
    }

    int compare_unsigned(std::uint64_t left, std::uint64_t right) noexcept
    {
        return left < right ? -1 : left > right ? 1 : 0;
    }

    void sort_folder_entries(
        std::vector<glance::app::ArchiveEntry>& entries,
        const glance::app::FolderPreviewPreferences& preferences)
    {
        std::ranges::sort(entries, [&preferences](const auto& left, const auto& right) {
            if (left.is_folder != right.is_folder)
            {
                return left.is_folder > right.is_folder;
            }

            int comparison{};
            switch (preferences.sort_field)
            {
            case glance::app::FolderSortField::type:
                comparison = compare_case_insensitive(left.type_name, right.type_name);
                break;
            case glance::app::FolderSortField::modified_time:
                comparison = compare_unsigned(left.modified_time, right.modified_time);
                break;
            case glance::app::FolderSortField::size:
                comparison = compare_unsigned(left.original_size, right.original_size);
                break;
            case glance::app::FolderSortField::name:
            default:
                comparison = compare_case_insensitive(left.name, right.name);
                break;
            }
            if (comparison == 0)
            {
                comparison = compare_case_insensitive(left.name, right.name);
            }
            return preferences.ascending ? comparison < 0 : comparison > 0;
        });
    }

    void copy_component_directory_value(
        const glance::app::FileDirectoryColumn& column,
        const glance::app::FileDirectoryValue& value,
        glance::app::ArchiveEntry& entry)
    {
        entry.values.push_back(glance::app::ArchiveEntryValue{
            .kind = static_cast<std::uint32_t>(value.kind),
            .unsigned_value = value.unsigned_value,
            .ratio_value = value.ratio_value,
            .text = value.text });
        if (column.id == L"type")
        {
            entry.type_name = value.text;
        }
        else if (column.id == L"modified")
        {
            entry.modified_time = value.unsigned_value;
        }
        else if (column.id == L"packed-size")
        {
            entry.compressed_size = value.unsigned_value;
            entry.compressed_size_known =
                value.kind != glance::contracts::components::FileDirectoryValueKind::none;
        }
        else if (column.id == L"size")
        {
            entry.original_size = value.unsigned_value;
            entry.original_size_known =
                value.kind != glance::contracts::components::FileDirectoryValueKind::none;
        }
    }

    bool append_component_directory_tree(
        const std::shared_ptr<void>& session,
        const glance::app::FileDirectoryDescriptor& descriptor,
        std::uint64_t parent_node_id,
        std::size_t depth,
        std::vector<glance::app::ArchiveEntry>& destination,
        glance::app::ArchivePreview& preview,
        std::unordered_set<std::uint64_t>& visited)
    {
        constexpr std::size_t maximum_depth = 6;
        std::uint32_t offset{};
        while (preview.entry_count < glance::app::maximum_preview_entries)
        {
            auto page = glance::app::enumerate_component_file_directory(
                session,
                parent_node_id,
                offset,
                128);
            if (page.failed)
            {
                return false;
            }
            for (auto& source : page.entries)
            {
                if (preview.entry_count >= glance::app::maximum_preview_entries)
                {
                    preview.truncated = true;
                    preview.entry_limit_reached = true;
                    return true;
                }
                if (source.node_id == 0 || !visited.insert(source.node_id).second)
                {
                    return false;
                }
                glance::app::ArchiveEntry entry;
                entry.name = std::move(source.name);
                entry.path = source.icon_key;
                entry.is_folder = source.is_folder;
                const auto value_count = std::min(
                    source.values.size(),
                    descriptor.columns.size());
                entry.values.reserve(value_count);
                for (std::size_t index = 0; index < value_count; ++index)
                {
                    copy_component_directory_value(
                        descriptor.columns[index],
                        source.values[index],
                        entry);
                }
                ++preview.entry_count;
                if (!entry.is_folder)
                {
                    ++preview.file_count;
                }
                if (entry.is_folder && source.has_children)
                {
                    if (depth < maximum_depth)
                    {
                        if (!append_component_directory_tree(
                                session,
                                descriptor,
                                source.node_id,
                                depth + 1,
                                entry.children,
                                preview,
                                visited))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        preview.truncated = true;
                        preview.depth_limited = true;
                    }
                }
                destination.push_back(std::move(entry));
            }
            offset += static_cast<std::uint32_t>(page.entries.size());
            if (offset >= page.total)
            {
                return true;
            }
            if (page.entries.empty())
            {
                return false;
            }
        }
        preview.truncated = true;
        preview.entry_limit_reached = true;
        return true;
    }

    void apply_component_directory_summary(
        const glance::app::FileDirectoryDescriptor& descriptor,
        glance::app::ArchivePreview& preview)
    {
        preview.truncated = preview.truncated || descriptor.truncated;
        preview.depth_limited = preview.depth_limited || descriptor.depth_limited;
        for (const auto& field : descriptor.info_fields)
        {
            if (field.id == L"file-count")
            {
                preview.file_count = static_cast<std::size_t>(field.value.unsigned_value);
            }
            else if (field.id == L"packed-size")
            {
                preview.compressed_size = field.value.unsigned_value;
                preview.compressed_size_known = true;
            }
            else if (field.id == L"original-size")
            {
                preview.original_size = field.value.unsigned_value;
                preview.original_size_known = true;
            }
            else if (field.id == L"encrypted")
            {
                preview.encrypted = field.value.unsigned_value != 0;
            }
        }
        preview.entry_compressed_size_available = std::ranges::any_of(
            descriptor.columns,
            [](const auto& column) { return column.id == L"packed-size"; });
    }

    ScrollViewer find_scroll_viewer(const DependencyObject& root)
    {
        if (root == nullptr)
        {
            return nullptr;
        }
        if (const auto viewer = root.try_as<ScrollViewer>())
        {
            return viewer;
        }
        const int child_count =
            Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < child_count; ++index)
        {
            if (const auto viewer = find_scroll_viewer(
                    Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, index)))
            {
                return viewer;
            }
        }
        return nullptr;
    }

    Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap create_pdf_bitmap(
        const glance::app::PagedDocumentRenderResult& rendered)
    {
        Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap(
            static_cast<std::int32_t>(rendered.pixel_width),
            static_cast<std::int32_t>(rendered.pixel_height));
        auto access = bitmap.PixelBuffer().as<BufferByteAccess>();
        byte* destination{};
        winrt::check_hresult(access->Buffer(&destination));
        const std::size_t destination_stride =
            static_cast<std::size_t>(rendered.pixel_width) * 4U;
        for (std::uint32_t row = 0; row < rendered.pixel_height; ++row)
        {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * destination_stride,
                rendered.pixels.data() + static_cast<std::size_t>(row) * rendered.stride,
                destination_stride);
        }
        bitmap.Invalidate();
        return bitmap;
    }

    std::wstring format_media_bitrate(std::uint32_t bitrate)
    {
        if (bitrate == 0)
        {
            return {};
        }
        std::wostringstream output;
        if (bitrate >= 1000000)
        {
            output << std::fixed << std::setprecision(1) << bitrate / 1000000.0 << L"Mbps";
        }
        else
        {
            output << (bitrate + 500) / 1000 << L"kbps";
        }
        return output.str();
    }

    std::wstring format_media_subtype(winrt::hstring const& subtype)
    {
        std::wstring result(subtype);
        if (result.size() <= 16)
        {
            std::ranges::transform(result, result.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towupper(value));
            });
        }
        return result;
    }

    std::wstring format_media_frame_rate(
        const Windows::Media::MediaProperties::MediaRatio& ratio)
    {
        if (ratio == nullptr || ratio.Denominator() == 0 || ratio.Numerator() == 0)
        {
            return {};
        }
        const double frame_rate =
            static_cast<double>(ratio.Numerator()) / ratio.Denominator();
        std::wostringstream output;
        output << std::fixed << std::setprecision(
            std::abs(frame_rate - std::round(frame_rate)) < 0.01 ? 0 : 2)
               << frame_rate << L"fps";
        return output.str();
    }

    std::wstring format_image_metadata(const glance::app::ImageMetadata& metadata)
    {
        using glance::app::ImageMetadataSection;
        constexpr std::array sections{
            std::pair{ ImageMetadataSection::capture, std::wstring_view{ L"ImageMetadataCaptureSection" } },
            std::pair{ ImageMetadataSection::camera, std::wstring_view{ L"ImageMetadataCameraSection" } },
            std::pair{ ImageMetadataSection::image, std::wstring_view{ L"ImageMetadataImageSection" } },
            std::pair{ ImageMetadataSection::location, std::wstring_view{ L"ImageMetadataLocationSection" } },
            std::pair{ ImageMetadataSection::details, std::wstring_view{ L"ImageMetadataDetailsSection" } },
        };

        std::wstring result;
        for (const auto& [section, resource_key] : sections)
        {
            bool section_started{};
            for (const auto& entry : metadata.entries)
            {
                if (entry.section != section)
                {
                    continue;
                }
                if (!section_started)
                {
                    if (!result.empty())
                    {
                        result.append(L"\n\n");
                    }
                    result.append(glance::app::localize(resource_key));
                    section_started = true;
                }
                result.append(L"\n").append(entry.name).append(L": ").append(entry.value);
            }
        }
        return result;
    }

    std::wstring sanitize_metadata_json_value(std::wstring_view value)
    {
        std::wstring result;
        result.reserve(value.size());
        for (const auto character : value)
        {
            const bool directional_control =
                character == L'\u061c' ||
                character == L'\u200e' ||
                character == L'\u200f' ||
                (character >= L'\u202a' && character <= L'\u202e') ||
                (character >= L'\u2066' && character <= L'\u2069') ||
                character == L'\ufeff';
            if (!directional_control)
            {
                result.push_back(character);
            }
        }
        return result;
    }

    std::wstring pretty_print_json(std::wstring_view json)
    {
        std::wstring result;
        result.reserve(json.size() + json.size() / 4);
        std::size_t indentation{};
        bool in_string{};
        bool escaped{};
        const auto append_new_line = [&] {
            result.push_back(L'\n');
            result.append(indentation * 4, L' ');
        };

        for (std::size_t index = 0; index < json.size(); ++index)
        {
            const auto character = json[index];
            if (in_string)
            {
                result.push_back(character);
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == L'\\')
                {
                    escaped = true;
                }
                else if (character == L'"')
                {
                    in_string = false;
                }
                continue;
            }
            if (character == L'"')
            {
                in_string = true;
                result.push_back(character);
                continue;
            }
            if (std::iswspace(character))
            {
                continue;
            }
            if (character == L'{' || character == L'[')
            {
                result.push_back(character);
                const wchar_t closing = character == L'{' ? L'}' : L']';
                if (index + 1 < json.size() && json[index + 1] != closing)
                {
                    ++indentation;
                    append_new_line();
                }
                continue;
            }
            if (character == L'}' || character == L']')
            {
                const wchar_t opening = character == L'}' ? L'{' : L'[';
                if (index != 0 && json[index - 1] != opening)
                {
                    --indentation;
                    append_new_line();
                }
                result.push_back(character);
                continue;
            }
            if (character == L',')
            {
                result.push_back(character);
                append_new_line();
                continue;
            }
            if (character == L':')
            {
                result.append(L": ");
                continue;
            }
            result.push_back(character);
        }
        return result;
    }

    std::wstring format_image_metadata_json(const glance::app::ImageMetadata& metadata)
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject root;
        for (const auto& entry : metadata.entries)
        {
            if (!entry.canonical_name.empty() && !entry.raw_value.empty())
            {
                const auto raw_value = sanitize_metadata_json_value(entry.raw_value);
                IJsonValue json_value = JsonValue::CreateStringValue(raw_value);
                const auto first = std::ranges::find_if_not(raw_value, [](wchar_t character) {
                    return std::iswspace(character) != 0;
                });
                if (first != raw_value.end() && (*first == L'{' || *first == L'['))
                {
                    try
                    {
                        json_value = JsonValue::Parse(hstring(raw_value));
                    }
                    catch (const hresult_error&)
                    {
                    }
                }
                root.SetNamedValue(
                    entry.canonical_name,
                    json_value);
            }
        }
        return root.Size() == 0
            ? std::wstring{}
            : pretty_print_json(std::wstring(root.Stringify()));
    }

    void set_information_panel_text(
        const RichTextBlock& control,
        std::wstring_view text,
        bool sectioned)
    {
        control.Blocks().Clear();
        if (text.empty())
        {
            return;
        }

        Paragraph paragraph;
        bool section_start = true;
        std::size_t cursor{};
        while (cursor < text.size())
        {
            const auto line_end = text.find(L'\n', cursor);
            auto line = text.substr(
                cursor,
                line_end == std::wstring_view::npos
                    ? text.size() - cursor
                    : line_end - cursor);
            if (line.ends_with(L'\r'))
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                Run run;
                run.Text(hstring(line));
                if (sectioned && section_start)
                {
                    run.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                }
                paragraph.Inlines().Append(run);
                section_start = false;
            }
            else
            {
                section_start = true;
            }
            if (line_end == std::wstring_view::npos)
            {
                break;
            }
            paragraph.Inlines().Append(LineBreak{});
            cursor = line_end + 1;
        }
        control.Blocks().Append(paragraph);
    }

}

namespace winrt::Glance::App::implementation
{
    MainWindow::MainWindow()
    {
        glance::contracts::log_event(L"MainWindow InitializeComponent begin.");
        InitializeComponent();
        glance::contracts::log_event(L"MainWindow InitializeComponent complete.");
        folder_preview_preferences_ = glance::app::load_folder_preview_preferences();
        ApplyLocalizedResources();
        ApplyAppearancePreferences();
        text_preferences_ = glance::app::load_text_preferences();
        footer_preferences_ = glance::app::load_footer_preferences();
        ApplyWindowPreferences();
        PreviewContentHost().AddHandler(
            UIElement::DoubleTappedEvent(),
            box_value<Input::DoubleTappedEventHandler>(
                { this, &MainWindow::PreviewContentHost_DoubleTapped }),
            true);
        configure_window();
        glance::contracts::log_event(L"MainWindow native configuration complete.");
        fullscreen_chrome_timer_ = DispatcherTimer();
        fullscreen_chrome_timer_.Interval(fullscreen_chrome_interval);
        media_timer_ = DispatcherTimer();
        media_timer_.Interval(std::chrono::milliseconds(250));
        const auto weak = get_weak();
        Closed([weak](IInspectable const&, WindowEventArgs const&) {
            if (const auto self = weak.get())
            {
                if (self->fullscreen_chrome_timer_ != nullptr)
                {
                    self->fullscreen_chrome_timer_.Stop();
                }
                self->acrylic_backdrop_.reset();
            }
        });
        fullscreen_chrome_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get())
            {
                self->update_fullscreen_chrome();
            }
        });
        media_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get())
            {
                self->update_media_controls();
            }
        });
        Windows::Media::Playback::MediaPlayer media_player;
        MediaPreview().SetMediaPlayer(media_player);
        const auto dispatcher = DispatcherQueue();
        media_player.MediaOpened([weak, dispatcher](
                                     Windows::Media::Playback::MediaPlayer const& sender,
                                     IInspectable const&) {
            const auto item =
                sender.Source().try_as<Windows::Media::Playback::MediaPlaybackItem>();
            if (item == nullptr)
            {
                return;
            }
            static_cast<void>(dispatcher.TryEnqueue([weak, item] {
                if (const auto self = weak.get())
                {
                    self->update_media_playback_metadata(
                        item,
                        self->media_playback_generation_);
                }
            }));
        });
        web_view_idle_timer_ = DispatcherTimer();
        web_view_idle_timer_.Interval(web_view_idle_timeout);
        web_view_idle_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get())
            {
                self->web_view_idle_timer_.Stop();
                const bool active = self->visible_ && !self->basic_info_mode_ &&
                    self->markdown_preview_ &&
                    (self->current_kind_ == glance::app::PreviewKind::markdown ||
                     self->current_kind_ == glance::app::PreviewKind::web) &&
                    self->WebPreviewHost().Visibility() == Visibility::Visible;
                if (active)
                {
                    return;
                }
                self->release_web_view_control();
            }
        });
    }

    void MainWindow::InitializeSession(
        std::uint64_t instance_id,
        StateCallback callback,
        GalleryRequestCallback gallery_request_callback,
        ComponentActionCallback component_action_callback)
    {
        instance_id_ = instance_id;
        state_callback_ = std::move(callback);
        gallery_request_callback_ = std::move(gallery_request_callback);
        component_action_callback_ = std::move(component_action_callback);
    }

    bool MainWindow::send_gallery_request(
        std::wstring_view operation,
        std::uint64_t request_id,
        std::uint32_t page_start,
        std::uint32_t target_index,
        int navigation_steps)
    {
        if (!gallery_request_callback_)
        {
            return false;
        }
        using namespace winrt::Windows::Data::Json;
        JsonObject root;
        root.SetNamedValue(L"operation", JsonValue::CreateStringValue(operation));
        root.SetNamedValue(L"windowId", JsonValue::CreateStringValue(std::to_wstring(instance_id_)));
        root.SetNamedValue(L"requestId", JsonValue::CreateStringValue(std::to_wstring(request_id)));
        root.SetNamedValue(L"sessionId", JsonValue::CreateStringValue(std::to_wstring(gallery_session_id_)));
        root.SetNamedValue(
            L"sourceWindow",
            JsonValue::CreateStringValue(std::to_wstring(reinterpret_cast<std::uintptr_t>(source_window_))));
        root.SetNamedValue(L"sourceId", JsonValue::CreateStringValue(source_id_));
        root.SetNamedValue(L"pageStart", JsonValue::CreateNumberValue(page_start));
        root.SetNamedValue(L"pageCount", JsonValue::CreateNumberValue(64));
        root.SetNamedValue(L"targetIndex", JsonValue::CreateNumberValue(target_index));
        root.SetNamedValue(L"navigationSteps", JsonValue::CreateNumberValue(navigation_steps));
        root.SetNamedValue(L"loop", JsonValue::CreateBooleanValue(loop_gallery_enabled_));
        if (operation == L"open" && current_index_ < files_.size())
        {
            root.SetNamedValue(L"currentPath", JsonValue::CreateStringValue(files_[current_index_].path));
            JsonArray extensions;
            if (gallery_same_extension_only_ || gallery_same_extension_override_)
            {
                const auto extension = glance::app::normalize_gallery_extension(
                    std::filesystem::path(files_[current_index_].path).extension().wstring());
                if (!extension.empty())
                {
                    extensions.Append(JsonValue::CreateStringValue(extension));
                }
            }
            else
            {
                for (const auto& extension : glance::app::gallery_extensions(gallery_media_kind_))
                {
                    extensions.Append(JsonValue::CreateStringValue(extension));
                }
            }
            root.SetNamedValue(L"extensions", extensions);
        }
        return gallery_request_callback_(winrt::to_string(root.Stringify()));
    }

    void MainWindow::open_gallery(bool preserve_navigation)
    {
        if (!gallery_source_available() || source_window_ == nullptr ||
            current_index_ >= files_.size() ||
            gallery_media_kind_ ==
                glance::contracts::components::GalleryMediaKind::none)
        {
            glance::contracts::log_event(
                L"Gallery request rejected before dispatch: source kind " +
                std::to_wstring(source_kind_) + L", source window " +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(source_window_)) + L".");
            leave_gallery(false, false);
            show_preview_notice(L"GalleryUnavailableNotice");
            return;
        }
        if (!preserve_navigation)
        {
            gallery_pending_navigation_steps_ = 0;
        }
        gallery_session_id_ = 0;
        gallery_page_request_id_ = 0;
        gallery_select_request_id_ = 0;
        gallery_total_count_ = 0;
        gallery_total_known_ = true;
        gallery_current_index_ = 0;
        gallery_desired_index_ = 0;
        gallery_pending_target_.reset();
        gallery_page_select_after_load_ = false;
        gallery_items_.clear();
        cancel_gallery_preloads();
        gallery_image_cache_.clear();
        pending_gallery_image_.reset();
        gallery_wheel_delta_ = 0;
        gallery_mode_ = GalleryMode::opening;
        GalleryModeButton().IsChecked(true);
        gallery_open_request_id_ = ++gallery_request_sequence_;
        if (!send_gallery_request(L"open", gallery_open_request_id_))
        {
            leave_gallery(false, false);
            show_preview_notice(L"GalleryUnavailableNotice");
            return;
        }
        if (!preserve_navigation)
        {
            show_preview_notice(L"GalleryScrollModeNotice");
        }
    }

    bool MainWindow::gallery_source_available() const noexcept
    {
        if (source_kind_ == 1)
        {
            return true;
        }
        const auto required =
            static_cast<std::uint64_t>(glance::contracts::sources::Capability::item_list) |
            static_cast<std::uint64_t>(glance::contracts::sources::Capability::focus_change);
        return source_kind_ == 3 && !source_id_.empty() &&
            (source_capabilities_ & required) == required;
    }

    void MainWindow::leave_gallery(bool show_notice, bool notify_core)
    {
        const bool was_gallery = gallery_mode_ != GalleryMode::inactive;
        if (was_gallery && notify_core && gallery_request_callback_)
        {
            const auto request_id = ++gallery_request_sequence_;
            static_cast<void>(send_gallery_request(L"close", request_id));
        }
        gallery_mode_ = GalleryMode::inactive;
        gallery_same_extension_override_ = false;
        GalleryModeButton().IsChecked(false);
        gallery_session_id_ = 0;
        gallery_open_request_id_ = 0;
        gallery_page_request_id_ = 0;
        gallery_select_request_id_ = 0;
        gallery_total_count_ = 0;
        gallery_total_known_ = true;
        gallery_current_index_ = 0;
        gallery_desired_index_ = 0;
        gallery_pending_target_.reset();
        gallery_page_select_after_load_ = false;
        gallery_pending_navigation_steps_ = 0;
        gallery_wheel_delta_ = 0;
        gallery_items_.clear();
        cancel_gallery_preloads();
        gallery_image_cache_.clear();
        pending_gallery_image_.reset();
        update_title_text();
        if (was_gallery && show_notice)
        {
            show_preview_notice(
                gallery_media_kind_ ==
                        glance::contracts::components::GalleryMediaKind::image
                    ? L"ImageWheelZoomModeNotice"
                    : L"MediaWheelControlModeNotice");
        }
    }

    void MainWindow::toggle_gallery_mode()
    {
        if (gallery_mode_ == GalleryMode::inactive)
        {
            gallery_same_extension_override_ = false;
            open_gallery();
            return;
        }
        leave_gallery(true);
    }

    void MainWindow::request_gallery_page(std::uint32_t target_index, bool select_after_load)
    {
        if (gallery_mode_ != GalleryMode::active || gallery_total_count_ == 0)
        {
            return;
        }
        gallery_page_select_after_load_ = select_after_load;
        if (select_after_load)
        {
            gallery_pending_target_ = target_index;
        }
        std::uint32_t page_start = target_index > 32 ? target_index - 32 : 0;
        if (gallery_total_count_ > 64)
        {
            page_start = std::min(page_start, gallery_total_count_ - 64);
        }
        gallery_page_request_id_ = ++gallery_request_sequence_;
        if (!send_gallery_request(L"page", gallery_page_request_id_, page_start))
        {
            leave_gallery(false, false);
            show_preview_notice(L"GalleryUnavailableNotice");
        }
    }

    void MainWindow::request_gallery_selection(std::uint32_t target_index)
    {
        if (gallery_mode_ != GalleryMode::active)
        {
            return;
        }
        if (!gallery_items_.contains(target_index))
        {
            request_gallery_page(target_index);
            return;
        }
        gallery_pending_target_ = target_index;
        gallery_select_request_id_ = ++gallery_request_sequence_;
        if (!send_gallery_request(L"select", gallery_select_request_id_, 0, target_index))
        {
            leave_gallery(false, false);
            show_preview_notice(L"GalleryUnavailableNotice");
        }
    }

    void MainWindow::navigate_gallery(int steps)
    {
        if (gallery_mode_ != GalleryMode::active ||
            gallery_total_count_ <= 1 || steps == 0)
        {
            return;
        }
        if (!gallery_total_known_)
        {
            gallery_pending_navigation_steps_ += steps;
            gallery_select_request_id_ = ++gallery_request_sequence_;
            if (!send_gallery_request(
                    L"navigate",
                    gallery_select_request_id_,
                    0,
                    0,
                    gallery_pending_navigation_steps_))
            {
                leave_gallery(false, false);
                show_preview_notice(L"GalleryUnavailableNotice");
            }
            return;
        }
        const auto target = glance::app::gallery_target_index(
            gallery_desired_index_,
            steps,
            gallery_total_count_,
            loop_gallery_enabled_);
        if (target == gallery_desired_index_)
        {
            return;
        }
        gallery_desired_index_ = target;
        gallery_pending_navigation_steps_ += steps;
        request_gallery_selection(gallery_desired_index_);
    }

    bool MainWindow::handle_gallery_wheel(int delta)
    {
        if (gallery_mode_ == GalleryMode::inactive)
        {
            return false;
        }
        gallery_wheel_delta_ += delta;
        const int notches = gallery_wheel_delta_ / WHEEL_DELTA;
        if (notches != 0)
        {
            gallery_wheel_delta_ -= notches * WHEEL_DELTA;
            const int steps = -notches;
            if (gallery_mode_ == GalleryMode::opening)
            {
                gallery_pending_navigation_steps_ += steps;
            }
            else
            {
                navigate_gallery(steps);
            }
        }
        return true;
    }

    void MainWindow::apply_gallery_file(std::uint32_t index, glance::app::PreviewFile file)
    {
        if (gallery_mode_ != GalleryMode::active || index != gallery_desired_index_)
        {
            return;
        }
        const auto kind = glance::app::resolve_preview_kind(file.path);
        if (kind == glance::app::PreviewKind::image)
        {
            const auto cached = gallery_image_cache_.find(gallery_image_cache_key(file));
            pending_gallery_image_ = cached != gallery_image_cache_.end()
                ? std::optional<GalleryImageCacheEntry>(cached->second)
                : std::nullopt;
        }
        else
        {
            pending_gallery_image_.reset();
        }
        files_.assign(1, std::move(file));
        current_index_ = 0;
        FileList().Items().Clear();
        update_preview_navigation_ui();
        present_file(0, kind);
        schedule_gallery_preloads();
    }

    void MainWindow::HandleGalleryResponse(std::string_view payload)
    {
        using namespace winrt::Windows::Data::Json;
        try
        {
            const auto root = JsonObject::Parse(winrt::to_hstring(payload));
            const std::wstring operation = root.GetNamedString(L"operation").c_str();
            const auto request_id = parse_u64(root.GetNamedString(L"requestId"));
            const auto session_id = parse_u64(root.GetNamedString(L"sessionId"));
            if (!request_id || !session_id)
            {
                return;
            }
            const bool success = root.GetNamedBoolean(L"success");
            const std::wstring error = root.GetNamedString(L"error", L"").c_str();
            if (operation == L"close")
            {
                return;
            }
            const bool current_request =
                (operation == L"open" && *request_id == gallery_open_request_id_) ||
                (operation == L"page" && *request_id == gallery_page_request_id_) ||
                ((operation == L"select" || operation == L"navigate") &&
                 *request_id == gallery_select_request_id_);
            if (!current_request || gallery_mode_ == GalleryMode::inactive)
            {
                return;
            }
            if (!success)
            {
                if ((operation == L"select" || operation == L"page") &&
                    error == L"stale")
                {
                    open_gallery(true);
                    return;
                }
                leave_gallery(false, false);
                show_preview_notice(L"GalleryUnavailableNotice");
                return;
            }

            if (operation == L"open")
            {
                gallery_session_id_ = *session_id;
                gallery_total_count_ = static_cast<std::uint32_t>(root.GetNamedNumber(L"totalCount"));
                gallery_total_known_ = root.GetNamedBoolean(L"totalKnown", true);
                gallery_current_index_ = static_cast<std::uint32_t>(root.GetNamedNumber(L"currentIndex"));
                gallery_desired_index_ = gallery_current_index_;
                gallery_mode_ = GalleryMode::active;
                update_title_text();
            }
            else if (*session_id != gallery_session_id_)
            {
                return;
            }

            if (operation == L"open" || operation == L"page" || operation == L"navigate")
            {
                if (operation == L"navigate")
                {
                    gallery_items_.clear();
                }
                const auto page_start = static_cast<std::uint32_t>(root.GetNamedNumber(L"pageStart"));
                const auto items = root.GetNamedArray(L"items");
                for (std::uint32_t offset = 0; offset < items.Size(); ++offset)
                {
                    const auto object = items.GetObjectAt(offset);
                    const auto size = parse_u64(object.GetNamedString(L"size"));
                    const auto creation_time = parse_u64(object.GetNamedString(L"creationTime"));
                    const auto last_write_time = parse_u64(object.GetNamedString(L"lastWriteTime"));
                    if (!size || !creation_time || !last_write_time)
                    {
                        continue;
                    }
                    glance::app::PreviewFile file;
                    file.display_name = object.GetNamedString(L"displayName").c_str();
                    file.path = object.GetNamedString(L"path").c_str();
                    file.parsing_name = object.GetNamedString(L"parsingName").c_str();
                    file.size = *size;
                    file.creation_time = *creation_time;
                    file.last_write_time = *last_write_time;
                    file.attributes = static_cast<std::uint32_t>(object.GetNamedNumber(L"attributes"));
                    file.is_filesystem = object.GetNamedBoolean(L"isFilesystem");
                    file.is_cloud_placeholder = object.GetNamedBoolean(L"isCloudPlaceholder");
                    const auto item_index = static_cast<std::uint32_t>(
                        object.GetNamedNumber(L"index", page_start + offset));
                    gallery_items_[item_index] = std::move(file);
                }
                if (operation == L"open" && gallery_pending_navigation_steps_ != 0)
                {
                    const int steps = std::exchange(gallery_pending_navigation_steps_, 0);
                    navigate_gallery(steps);
                }
                else if (gallery_page_select_after_load_ && gallery_pending_target_ &&
                    gallery_items_.contains(*gallery_pending_target_))
                {
                    gallery_page_select_after_load_ = false;
                    request_gallery_selection(*gallery_pending_target_);
                }
                else
                {
                    schedule_gallery_preloads();
                }
                if (operation != L"navigate")
                {
                    return;
                }
            }

            if (operation == L"select" || operation == L"navigate")
            {
                const auto index = static_cast<std::uint32_t>(root.GetNamedNumber(L"currentIndex"));
                const auto item = gallery_items_.find(index);
                if (item == gallery_items_.end())
                {
                    request_gallery_page(index);
                    return;
                }
                gallery_current_index_ = index;
                gallery_desired_index_ = index;
                gallery_pending_target_.reset();
                gallery_pending_navigation_steps_ = 0;
                apply_gallery_file(index, item->second);
            }
        }
        catch (...)
        {
        }
    }

    void MainWindow::HandleGalleryDisconnect()
    {
        if (gallery_mode_ == GalleryMode::inactive)
        {
            return;
        }
        leave_gallery(false, false);
        show_preview_notice(L"GalleryUnavailableNotice");
    }

    void MainWindow::update_title_text()
    {
        if (current_index_ >= files_.size())
        {
            TitleText().Text(L"");
            return;
        }
        auto title = files_[current_index_].display_name;
        if (gallery_mode_ == GalleryMode::active &&
            gallery_total_known_ &&
            gallery_total_count_ != 0 &&
            gallery_current_index_ < gallery_total_count_)
        {
            title = L"(" + std::to_wstring(gallery_current_index_ + 1) + L"/" +
                std::to_wstring(gallery_total_count_) + L") " + title;
        }
        TitleText().Text(std::move(title));
    }

    std::wstring MainWindow::gallery_image_cache_key(
        const glance::app::PreviewFile& file) const
    {
        auto path = file.path;
        std::ranges::transform(
            path,
            path.begin(),
            [](wchar_t value) { return std::towlower(value); });
        return path + L"\n" + std::to_wstring(file.size) + L"\n" +
            std::to_wstring(file.last_write_time);
    }

    void MainWindow::cancel_gallery_preloads() noexcept
    {
        ++gallery_preload_generation_;
        for (const auto& operation : gallery_preload_operations_)
        {
            try
            {
                if (operation != nullptr &&
                    operation.Status() == Windows::Foundation::AsyncStatus::Started)
                {
                    operation.Cancel();
                }
            }
            catch (...)
            {
            }
        }
        gallery_preload_operations_.clear();
    }

    Windows::Foundation::IAsyncAction MainWindow::preload_gallery_image_async(
        glance::app::PreviewFile file,
        std::uint64_t generation)
    {
        constexpr std::uint64_t cache_entry_budget = 32ULL * 1024ULL * 1024ULL;
        const auto lifetime = get_strong();
        auto cancellation = co_await winrt::get_cancellation_token();
        cancellation.enable_propagation();
        try
        {
            const double raster_scale = lifetime->ImagePanel().XamlRoot() != nullptr
                ? lifetime->ImagePanel().XamlRoot().RasterizationScale()
                : 1.0;
            const double viewport_extent = std::max(
                lifetime->ImageScroller().ActualWidth(),
                lifetime->ImageScroller().ActualHeight()) * raster_scale;
            const auto storage_file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(file.path);
            const auto properties = co_await storage_file.Properties().GetImagePropertiesAsync();
            if (cancellation() || properties.Width() == 0 || properties.Height() == 0)
            {
                co_return;
            }

            const double width = properties.Width();
            const double height = properties.Height();
            const double memory_scale = std::min(
                1.0,
                std::sqrt(
                    static_cast<double>(cache_entry_budget / 4ULL) /
                    (width * height)));
            const double viewport_scale = viewport_extent > 0
                ? std::min(1.0, viewport_extent / std::max(width, height))
                : memory_scale;
            const double decode_scale = std::min(memory_scale, viewport_scale);
            const auto decode_width = std::max(
                1U,
                static_cast<std::uint32_t>(std::llround(width * decode_scale)));
            const auto decode_height = std::max(
                1U,
                static_cast<std::uint32_t>(std::llround(height * decode_scale)));

            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            if (decode_width < properties.Width())
            {
                bitmap.DecodePixelWidth(static_cast<int>(decode_width));
            }
            const auto stream = co_await storage_file.OpenReadAsync();
            co_await bitmap.SetSourceAsync(stream);
            if (cancellation() || generation != lifetime->gallery_preload_generation_ ||
                lifetime->gallery_mode_ != GalleryMode::active ||
                lifetime->gallery_media_kind_ !=
                    glance::contracts::components::GalleryMediaKind::image)
            {
                co_return;
            }
            GalleryImageCacheEntry entry;
            entry.file = std::move(file);
            entry.bitmap = std::move(bitmap);
            entry.pixel_width = properties.Width();
            entry.pixel_height = properties.Height();
            entry.decoded_bytes = static_cast<std::uint64_t>(decode_width) *
                decode_height * 4ULL;
            lifetime->gallery_image_cache_[lifetime->gallery_image_cache_key(entry.file)] =
                std::move(entry);
        }
        catch (const hresult_canceled&)
        {
        }
        catch (...)
        {
        }
    }

    void MainWindow::schedule_gallery_preloads()
    {
        cancel_gallery_preloads();
        if (gallery_mode_ != GalleryMode::active || gallery_total_count_ <= 1 ||
            gallery_media_kind_ !=
                glance::contracts::components::GalleryMediaKind::image)
        {
            return;
        }

        std::array<std::uint32_t, 2> neighbors{};
        std::size_t neighbor_count{};
        if (!gallery_total_known_)
        {
            for (const auto& [index, file] : gallery_items_)
            {
                static_cast<void>(file);
                if (index != gallery_current_index_ && neighbor_count < neighbors.size())
                {
                    neighbors[neighbor_count++] = index;
                }
            }
        }
        else if (gallery_current_index_ > 0)
        {
            neighbors[neighbor_count++] = gallery_current_index_ - 1;
        }
        else if (loop_gallery_enabled_)
        {
            neighbors[neighbor_count++] = gallery_total_count_ - 1;
        }
        if (gallery_total_known_ && gallery_current_index_ + 1 < gallery_total_count_)
        {
            neighbors[neighbor_count++] = gallery_current_index_ + 1;
        }
        else if (gallery_total_known_ && loop_gallery_enabled_)
        {
            neighbors[neighbor_count++] = 0;
        }
        const auto active_neighbors = std::span(neighbors).first(neighbor_count);
        for (const auto index : active_neighbors)
        {
            if (!gallery_items_.contains(index))
            {
                request_gallery_page(index, false);
                return;
            }
        }

        std::unordered_set<std::wstring> retained_keys;
        if (const auto current = gallery_items_.find(gallery_current_index_);
            current != gallery_items_.end() &&
            glance::app::resolve_preview_kind(current->second.path) ==
                glance::app::PreviewKind::image)
        {
            retained_keys.insert(gallery_image_cache_key(current->second));
        }
        for (const auto index : active_neighbors)
        {
            const auto& file = gallery_items_.at(index);
            if (glance::app::resolve_preview_kind(file.path) ==
                glance::app::PreviewKind::image)
            {
                retained_keys.insert(gallery_image_cache_key(file));
            }
        }
        std::erase_if(gallery_image_cache_, [&retained_keys](const auto& item) {
            return !retained_keys.contains(item.first);
        });

        const auto generation = gallery_preload_generation_;
        std::unordered_set<std::wstring> scheduled;
        for (const auto index : active_neighbors)
        {
            const auto& file = gallery_items_.at(index);
            if (glance::app::resolve_preview_kind(file.path) !=
                glance::app::PreviewKind::image)
            {
                continue;
            }
            const auto key = gallery_image_cache_key(file);
            if (gallery_image_cache_.contains(key) || !scheduled.insert(key).second)
            {
                continue;
            }
            gallery_preload_operations_.push_back(
                preload_gallery_image_async(file, generation));
        }
    }

    void MainWindow::ApplyAppearancePreferences()
    {
        const auto preferences = glance::app::load_appearance_preferences();
        RootGrid().RequestedTheme(glance::app::element_theme(preferences.theme));
        if (preferences.acrylic_enabled && glance::app::acrylic_material_supported())
        {
            if (acrylic_backdrop_ == nullptr)
            {
                acrylic_backdrop_ = glance::app::WindowAcrylicBackdrop::create(
                    *this,
                    RootGrid(),
                    true,
                    preferences.acrylic_opacity_percent);
            }
            else
            {
                acrylic_backdrop_->set_opacity(preferences.acrylic_opacity_percent);
            }
        }
        else
        {
            acrylic_backdrop_.reset();
        }
        const bool acrylic_enabled = acrylic_backdrop_ != nullptr;
        acrylic_enabled_ = acrylic_enabled;
        apply_background_surfaces(acrylic_enabled_);
        if (text_editor_ != nullptr)
        {
            text_editor_->set_preferences(
                text_preferences_,
                syntax_highlighting_,
                RootGrid().ActualTheme() == ElementTheme::Dark);
        }
        if (current_text_markdown_ && !current_text_has_more_ && !current_text_.empty())
        {
            render_markdown();
        }
    }

    void MainWindow::apply_background_surfaces(bool acrylic_enabled)
    {
        if (acrylic_enabled)
        {
            const auto transparent = Media::SolidColorBrush(
                Windows::UI::Color{ 0, 0, 0, 0 });
            RootGrid().Background(transparent);
            const auto overlay = Application::Current().Resources().Lookup(
                box_value(L"AcrylicInAppFillColorBaseBrush")).as<Media::Brush>();
            TextLoadingOverlay().Background(overlay);
            PasswordPromptOverlay().Background(overlay);
        }
        else
        {
            RootGrid().ClearValue(Controls::Panel::BackgroundProperty());
            TextLoadingOverlay().ClearValue(Controls::Panel::BackgroundProperty());
            PasswordPromptOverlay().ClearValue(Controls::Panel::BackgroundProperty());
        }
        update_media_surface_background();
        update_fullscreen_chrome_surfaces();
    }

    void MainWindow::update_media_surface_background()
    {
        if (acrylic_enabled_)
        {
            MediaPanel().Background(Media::SolidColorBrush(
                Windows::UI::Color{ 0, 0, 0, 0 }));
        }
        else if (media_is_audio_)
        {
            MediaPanel().Background(Application::Current().Resources().Lookup(
                box_value(L"SolidBackgroundFillColorBaseBrush")).as<Media::Brush>());
        }
        else
        {
            MediaPanel().Background(Media::SolidColorBrush(
                Windows::UI::Color{ 255, 0, 0, 0 }));
        }
    }

    void MainWindow::ApplyLocalizedResources()
    {
        const auto set_tooltip = [](const auto& control, wchar_t const* key) {
            ToolTipService::SetToolTip(control, box_value(glance::app::localize(key)));
        };

        set_tooltip(TopmostButton(), L"TopmostButton.ToolTipService.ToolTip");
        set_tooltip(PinButton(), L"PinButton.ToolTipService.ToolTip");
        set_tooltip(BackButton(), L"BackButton.ToolTipService.ToolTip");
        set_tooltip(ClosePreviewButton(), L"ClosePreviewButton.ToolTipService.ToolTip");
        update_fullscreen_button();
        update_preview_mode_button();
        set_tooltip(
            GenericAdvancedInfoButton(),
            L"GenericAdvancedInfoButton.ToolTipService.ToolTip");
        LoadCloudFileText().Text(glance::app::localize(L"LoadCloudFileText.Text"));
        PreviewAsTextText().Text(glance::app::localize(L"PreviewAsTextText.Text"));
        if (TextLoadingOverlay().Visibility() == Visibility::Visible)
        {
            TextLoadingText().Text(glance::app::localize(L"Loading"));
        }
        if (current_kind_ == glance::app::PreviewKind::component &&
            current_index_ < files_.size() &&
            component_loading_language_ != glance::app::current_ui_language())
        {
            ComponentLoadingText().Visibility(Visibility::Collapsed);
            refresh_component_loading_text_async(
                files_[current_index_].path,
                content_generation_);
        }
        if (preview_notice_active_)
        {
            if (!preview_notice_resource_key_.empty())
            {
                PreviewErrorInfoBar().Message(
                    glance::app::localize(preview_notice_resource_key_));
            }
        }
        else
        {
            PreviewErrorInfoBar().Title(glance::app::localize(L"PreviewErrorInfoBar.Title"));
        }
        set_tooltip(MediaPlayPauseButton(), L"MediaPlayPauseButton.ToolTipService.ToolTip");
        set_tooltip(MediaMuteButton(), L"MediaMuteButton.ToolTipService.ToolTip");
        set_tooltip(GalleryModeButton(), L"GalleryModeButton.ToolTipService.ToolTip");
        set_tooltip(PreviousPdfButton(), L"PreviousPdfButton.ToolTipService.ToolTip");
        set_tooltip(NextPdfButton(), L"NextPdfButton.ToolTipService.ToolTip");
        set_tooltip(PdfThumbnailsButton(), L"PdfThumbnailsButton.ToolTipService.ToolTip");
        set_tooltip(PdfOutlineButton(), L"PdfOutlineButton.ToolTipService.ToolTip");
        PasswordPromptTitle().Text(glance::app::localize(L"PasswordPromptTitle"));
        PasswordPromptInput().PlaceholderText(
            glance::app::localize(L"PasswordPromptInput.PlaceholderText"));
        PasswordPromptSubmitButton().Content(
            box_value(glance::app::localize(L"PasswordPromptSubmitButton.Content")));
        update_archive_header_state();
        SystemAnsiItem().Text(glance::app::localize(L"SystemAnsiItem.Text"));
        set_tooltip(SyntaxHighlightButton(), L"SyntaxHighlightButton.ToolTipService.ToolTip");
        set_tooltip(ImageZoomButton(), L"ImageZoomButton.ToolTipService.ToolTip");
        ImageZoomLabel().Text(glance::app::localize(L"ImageZoomLabel.Text"));
        set_tooltip(RotateButton(), L"RotateButton.ToolTipService.ToolTip");
        set_tooltip(FlipButton(), L"FlipButton.ToolTipService.ToolTip");
        set_tooltip(ImageExifButton(), L"ImageExifButton.ToolTipService.ToolTip");
        set_tooltip(WordWrapButton(), L"WordWrapButton.ToolTipService.ToolTip");
        set_tooltip(CopyPathButton(), L"CopyPathButton.ToolTipService.ToolTip");
        set_tooltip(OpenDefaultButton(), L"OpenDefaultButton.ToolTipService.ToolTip");
        set_tooltip(OpenFolderButton(), L"OpenFolderButton.ToolTipService.ToolTip");
        if (files_.empty())
        {
            EncodingSelector().Content(box_value(glance::app::localize(L"EncodingSelector.Content")));
        }
        update_line_number_visibility();
        update_generic_file_metadata();
        update_footer_metadata();
        rebuild_component_contributions();
    }

    void MainWindow::ApplyTextPreferences()
    {
        apply_text_preferences();
        if (current_kind_ == glance::app::PreviewKind::text ||
            current_kind_ == glance::app::PreviewKind::markdown ||
            current_kind_ == glance::app::PreviewKind::web)
        {
            update_text_layout();
        }
        update_preview_as_text_button();
    }

    void MainWindow::update_archive_header_state()
    {
        const std::array buttons{
            ArchiveNameHeaderButton(),
            ArchiveTypeHeaderButton(),
            ArchiveModifiedHeaderButton(),
            ArchiveSizeHeaderButton(),
        };
        const std::array glyphs{
            ArchiveNameSortGlyph(),
            ArchiveTypeSortGlyph(),
            ArchiveModifiedSortGlyph(),
            ArchiveSizeSortGlyph(),
        };
        const std::array headers{
            ArchiveNameHeader(),
            ArchiveTypeHeader(),
            ArchiveThirdHeader(),
            ArchiveFourthHeader(),
        };

        active_file_directory_columns_.clear();
        if (!archive_preview_is_directory_ &&
            !active_file_directory_descriptor_.columns.empty())
        {
            std::vector<ArchiveColumnSpec> columns;
            const auto count = std::min<std::size_t>(
                active_file_directory_descriptor_.columns.size(),
                buttons.size());
            columns.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto& column = active_file_directory_descriptor_.columns[index];
                active_file_directory_columns_.push_back(
                    static_cast<std::uint32_t>(index));
                columns.push_back(ArchiveColumnSpec{
                    .kind = ArchiveColumnKind::name,
                    .width = index == 0
                        ? 0.0
                        : static_cast<double>(column.width == 0 ? 110 : column.width) });
                headers[index].Text(column.title);
                buttons[index].Visibility(Visibility::Visible);
                buttons[index].HorizontalContentAlignment(
                    column.alignment ==
                            glance::contracts::components::FileDirectoryAlignment::right
                        ? HorizontalAlignment::Right
                        : HorizontalAlignment::Left);
            }
            for (std::size_t index = count; index < buttons.size(); ++index)
            {
                buttons[index].Visibility(Visibility::Collapsed);
            }
            configure_archive_columns(ArchiveHeaderGrid(), columns);
        }
        else
        {
            const auto columns = archive_columns(
                archive_preview_is_directory_,
                archive_entry_compressed_size_available_);
            configure_archive_columns(ArchiveHeaderGrid(), columns);
            ArchiveNameHeader().Text(glance::app::localize(L"ArchiveNameHeader.Text"));
            ArchiveTypeHeader().Text(glance::app::localize(L"ArchiveTypeHeader.Text"));
            ArchiveThirdHeader().Text(glance::app::localize(
                archive_preview_is_directory_
                    ? L"ArchiveModifiedHeader.Text"
                    : archive_entry_compressed_size_available_
                        ? L"ArchiveCompressedSizeHeader"
                        : L"ArchiveOriginalSizeHeader"));
            ArchiveFourthHeader().Text(glance::app::localize(
                archive_preview_is_directory_
                    ? L"ArchiveSizeHeader.Text"
                    : L"ArchiveOriginalSizeHeader"));
            ArchiveModifiedHeaderButton().HorizontalContentAlignment(
                archive_preview_is_directory_
                    ? HorizontalAlignment::Left
                    : HorizontalAlignment::Right);
            for (std::size_t index = 0; index < buttons.size(); ++index)
            {
                buttons[index].Visibility(
                    index < columns.size() ? Visibility::Visible : Visibility::Collapsed);
            }
        }

        for (const auto& button : buttons)
        {
            const bool interactive =
                archive_preview_is_directory_ && button.Visibility() == Visibility::Visible;
            button.IsHitTestVisible(interactive);
            button.IsTabStop(interactive);
        }
        for (const auto& glyph : glyphs)
        {
            glyph.Visibility(Visibility::Collapsed);
        }
        FolderEntryList().Visibility(
            archive_preview_is_directory_ ? Visibility::Visible : Visibility::Collapsed);
        ArchiveEntryTree().Visibility(
            archive_preview_is_directory_ ? Visibility::Collapsed : Visibility::Visible);
        if (!archive_preview_is_directory_)
        {
            return;
        }

        const auto field_index = static_cast<std::size_t>(folder_preview_preferences_.sort_field);
        if (field_index < glyphs.size())
        {
            glyphs[field_index].Glyph(folder_preview_preferences_.ascending ? L"\xE70E" : L"\xE70D");
            glyphs[field_index].Visibility(Visibility::Visible);
        }
    }

    void MainWindow::ApplyFooterPreferences()
    {
        footer_preferences_ = glance::app::load_footer_preferences();
        if (glance::app::footer_field_enabled(
                footer_preferences_, glance::app::FooterField::media_info) &&
            current_index_ < files_.size())
        {
            if (current_kind_ == glance::app::PreviewKind::image &&
                image_bits_per_pixel_ == 0)
            {
                load_image_media_info_async(files_[current_index_].path, content_generation_);
            }
            else if (current_kind_ == glance::app::PreviewKind::media &&
                     media_playback_item_ != nullptr)
            {
                update_media_playback_metadata(
                    media_playback_item_,
                    media_playback_generation_);
            }
        }
        update_footer_metadata();
        request_footer_access_if_needed();
    }

    void MainWindow::ApplyWindowPreferences()
    {
        double_click_fullscreen_enabled_ =
            glance::app::load_window_preferences().double_click_fullscreen;
        try
        {
            if (web_preview_ != nullptr)
            {
                if (const auto core = web_preview_.CoreWebView2())
                {
                    core.PostWebMessageAsString(
                        double_click_fullscreen_enabled_
                            ? web_double_click_fullscreen_enabled
                            : web_double_click_fullscreen_disabled);
                }
            }
        }
        catch (...)
        {
        }
    }

    void MainWindow::configure_window()
    {
        glance::contracts::log_event(L"Resolving the native window handle.");
        const auto window_native = this->try_as<::IWindowNative>();
        check_hresult(window_native->get_WindowHandle(&window_));

        if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_GLANCE_APP)))
        {
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }

        glance::contracts::log_event(L"Applying native window styles.");
        LONG_PTR extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        extended_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);

        LONG_PTR window_style = GetWindowLongPtrW(window_, GWL_STYLE);
        window_style &= ~(WS_SYSMENU | WS_MINIMIZEBOX);
        SetWindowLongPtrW(window_, GWL_STYLE, window_style);
        SetWindowSubclass(window_, window_subclass, 1, reinterpret_cast<DWORD_PTR>(this));

        if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(true);
            presenter.SetBorderAndTitleBar(true, false);
        }

        glance::contracts::log_event(L"Enabling the custom title bar.");
        ExtendsContentIntoTitleBar(true);
        glance::contracts::log_event(L"Assigning the title bar drag region.");
        SetTitleBar(TitleBarDragRegion());
        glance::contracts::log_event(L"Refreshing the native window frame.");
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        glance::contracts::log_event(L"Assigning the window title.");
        const std::wstring window_title = glance::app::localize(L"AppName");
        SetWindowTextW(window_, window_title.c_str());
    }

    void MainWindow::set_fullscreen(bool enabled) noexcept
    {
        if (window_ == nullptr || fullscreen_ == enabled)
        {
            update_fullscreen_button();
            return;
        }

        if (enabled)
        {
            fullscreen_restore_placement_ = WINDOWPLACEMENT{ sizeof(WINDOWPLACEMENT) };
            fullscreen_restore_placement_valid_ =
                GetWindowPlacement(window_, &fullscreen_restore_placement_) != FALSE;
            try
            {
                AppWindow().SetPresenter(
                    Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen);
            }
            catch (const hresult_error& error)
            {
                glance::contracts::log_event(
                    L"Unable to enter full screen: " + std::wstring(error.message()));
                fullscreen_restore_placement_valid_ = false;
                update_fullscreen_button();
                return;
            }

            fullscreen_ = true;
            Grid::SetRow(PreviewContentHost(), 0);
            Grid::SetRowSpan(PreviewContentHost(), 3);
            update_fullscreen_chrome_surfaces();
            set_fullscreen_chrome_visibility(false, false);
            fullscreen_title_hover_tick_ = 0;
            fullscreen_footer_hover_tick_ = 0;
            if (fullscreen_chrome_timer_ != nullptr)
            {
                fullscreen_chrome_timer_.Start();
            }
        }
        else
        {
            try
            {
                AppWindow().SetPresenter(
                    Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
                if (const auto presenter = AppWindow().Presenter().try_as<
                        Microsoft::UI::Windowing::OverlappedPresenter>())
                {
                    presenter.IsMinimizable(false);
                    presenter.IsMaximizable(true);
                    presenter.SetBorderAndTitleBar(true, false);
                }
            }
            catch (const hresult_error& error)
            {
                glance::contracts::log_event(
                    L"Unable to exit full screen: " + std::wstring(error.message()));
                update_fullscreen_button();
                return;
            }

            fullscreen_ = false;
            if (fullscreen_chrome_timer_ != nullptr)
            {
                fullscreen_chrome_timer_.Stop();
            }
            Grid::SetRow(PreviewContentHost(), 1);
            Grid::SetRowSpan(PreviewContentHost(), 1);
            set_fullscreen_chrome_visibility(true, true);
            update_fullscreen_chrome_surfaces();

            if (fullscreen_restore_placement_valid_)
            {
                auto placement = fullscreen_restore_placement_;
                if (!visible_)
                {
                    placement.showCmd = SW_HIDE;
                }
                static_cast<void>(SetWindowPlacement(window_, &placement));
            }
            fullscreen_restore_placement_valid_ = false;
            set_topmost(topmost_);
        }

        update_fullscreen_button();
        queue_text_editor_occlusion_update();
        static_cast<void>(DispatcherQueue().TryEnqueue([weak = get_weak()] {
            if (const auto self = weak.get())
            {
                self->update_text_editor_bounds();
            }
        }));
    }

    void MainWindow::update_fullscreen_button()
    {
        FullscreenButton().IsChecked(fullscreen_);
        FullscreenIcon().Glyph(fullscreen_ ? L"\xE73F" : L"\xE740");
        ToolTipService::SetToolTip(
            FullscreenButton(),
            box_value(glance::app::localize(
                fullscreen_ ? L"ExitFullscreenToolTip" : L"EnterFullscreenToolTip")));
    }

    void MainWindow::update_fullscreen_chrome_surfaces()
    {
        if (!fullscreen_)
        {
            PreviewTitleBar().ClearValue(Panel::BackgroundProperty());
            PreviewFooterBar().ClearValue(Panel::BackgroundProperty());
            return;
        }

        const auto resource_key = acrylic_enabled_
            ? L"AcrylicInAppFillColorBaseBrush"
            : L"SolidBackgroundFillColorBaseBrush";
        const auto brush = Application::Current().Resources().Lookup(
            box_value(resource_key)).as<Media::Brush>();
        PreviewTitleBar().Background(brush);
        PreviewFooterBar().Background(brush);
    }

    void MainWindow::set_fullscreen_chrome_visibility(
        bool title_visible,
        bool footer_visible)
    {
        if (!fullscreen_)
        {
            title_visible = true;
            footer_visible = true;
        }
        if (fullscreen_title_visible_ == title_visible &&
            fullscreen_footer_visible_ == footer_visible)
        {
            return;
        }

        fullscreen_title_visible_ = title_visible;
        fullscreen_footer_visible_ = footer_visible;
        PreviewTitleBar().Opacity(title_visible ? 1.0 : 0.0);
        PreviewTitleBar().IsHitTestVisible(title_visible);
        PreviewFooterBar().Opacity(footer_visible ? 1.0 : 0.0);
        PreviewFooterBar().IsHitTestVisible(footer_visible);
        queue_text_editor_occlusion_update();
    }

    void MainWindow::update_fullscreen_chrome() noexcept
    {
        if (!fullscreen_ || window_ == nullptr)
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        POINT cursor{};
        RECT client{};
        bool cursor_over_window = false;
        if (GetCursorPos(&cursor) && GetClientRect(window_, &client))
        {
            const HWND target = WindowFromPoint(cursor);
            cursor_over_window = target == window_ ||
                (target != nullptr &&
                    (IsChild(window_, target) ||
                     GetAncestor(target, GA_ROOTOWNER) == window_));
            if (cursor_over_window)
            {
                ScreenToClient(window_, &cursor);
                const int dpi = static_cast<int>(GetDpiForWindow(window_));
                const int edge = MulDiv(fullscreen_edge_reveal_dips, dpi, 96);
                const int title_height = MulDiv(fullscreen_title_height_dips, dpi, 96);
                const int footer_height = MulDiv(fullscreen_footer_height_dips, dpi, 96);
                const bool inside = cursor.x >= client.left && cursor.x < client.right &&
                    cursor.y >= client.top && cursor.y < client.bottom;
                if (inside &&
                    cursor.y <= (fullscreen_title_visible_ ? title_height : edge))
                {
                    fullscreen_title_hover_tick_ = now;
                }
                if (inside &&
                    cursor.y >= client.bottom -
                        (fullscreen_footer_visible_ ? footer_height : edge))
                {
                    fullscreen_footer_hover_tick_ = now;
                }
            }
        }

        const bool title_visible = fullscreen_title_hover_tick_ != 0 &&
            now - fullscreen_title_hover_tick_ <= fullscreen_chrome_hide_delay_ms;
        const bool footer_visible = fullscreen_footer_hover_tick_ != 0 &&
            now - fullscreen_footer_hover_tick_ <= fullscreen_chrome_hide_delay_ms;
        set_fullscreen_chrome_visibility(title_visible, footer_visible);
    }

    bool MainWindow::handle_preview_content_double_click()
    {
        if (!glance::app::can_toggle_preview_fullscreen(
                visible_,
                double_click_fullscreen_enabled_,
                password_prompt_target_ != PasswordPromptTarget::none,
                fullscreen_toggle_pending_))
        {
            return false;
        }

        fullscreen_toggle_pending_ = true;
        const bool queued = DispatcherQueue().TryEnqueue([weak = get_weak()] {
            if (const auto self = weak.get())
            {
                self->fullscreen_toggle_pending_ = false;
                if (self->visible_ && self->double_click_fullscreen_enabled_ &&
                    self->password_prompt_target_ == PasswordPromptTarget::none)
                {
                    self->set_fullscreen(!self->fullscreen_);
                }
            }
        });
        if (!queued)
        {
            fullscreen_toggle_pending_ = false;
        }
        return queued;
    }

    bool MainWindow::is_interactive_preview_source(IInspectable const& source)
    {
        auto current = source.try_as<DependencyObject>();
        const auto host = PreviewContentHost();
        while (current != nullptr && get_abi(current) != get_abi(host))
        {
            if (current.try_as<Controls::Primitives::ButtonBase>() != nullptr ||
                current.try_as<Controls::Primitives::RangeBase>() != nullptr ||
                current.try_as<Controls::Primitives::SelectorItem>() != nullptr ||
                current.try_as<TextBox>() != nullptr ||
                current.try_as<PasswordBox>() != nullptr ||
                current.try_as<RichEditBox>() != nullptr ||
                current.try_as<ToggleSwitch>() != nullptr ||
                current.try_as<TreeViewItem>() != nullptr)
            {
                return true;
            }
            current = Media::VisualTreeHelper::GetParent(current);
        }
        return false;
    }

    LRESULT CALLBACK MainWindow::window_subclass(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR,
        DWORD_PTR reference_data) noexcept
    {
        auto* self = reinterpret_cast<MainWindow*>(reference_data);
        if (message == WM_MOUSEACTIVATE)
        {
            if (self == nullptr || !self->password_prompt_activation_enabled_)
            {
                return MA_NOACTIVATE;
            }
            if (!self->password_prompt_focused_)
            {
                self->password_prompt_focused_ = true;
                self->update_state();
            }
        }
        if (message == WM_ACTIVATE && self != nullptr &&
            self->password_prompt_activation_enabled_)
        {
            const bool focused = LOWORD(wparam) != WA_INACTIVE;
            if (self->password_prompt_focused_ != focused)
            {
                self->password_prompt_focused_ = focused;
                self->update_state();
            }
        }
        if (message == WM_GETMINMAXINFO)
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize.x = MulDiv(480, static_cast<int>(dpi), 96);
            limits->ptMinTrackSize.y = MulDiv(320, static_cast<int>(dpi), 96);
            return 0;
        }
        if (message == WM_ENTERSIZEMOVE && self != nullptr)
        {
            self->tracking_move_size_ = GetWindowRect(window, &self->move_size_start_bounds_) != FALSE;
        }
        if (message == WM_EXITSIZEMOVE && self != nullptr)
        {
            RECT bounds{};
            if (self->tracking_move_size_ && GetWindowRect(window, &bounds))
            {
                self->user_sized_ = self->user_sized_ ||
                    bounds.right - bounds.left !=
                        self->move_size_start_bounds_.right - self->move_size_start_bounds_.left ||
                    bounds.bottom - bounds.top !=
                        self->move_size_start_bounds_.bottom - self->move_size_start_bounds_.top;
            }
            self->tracking_move_size_ = false;
            self->save_current_window_placement();
        }
        if (message == WM_SYSCOMMAND && self != nullptr &&
            (wparam & 0xFFF0U) == SC_MAXIMIZE)
        {
            self->user_sized_ = true;
        }
        if (message == WM_WINDOWPOSCHANGED && self != nullptr)
        {
            self->update_text_editor_bounds();
        }
        if (message == WM_DESTROY && self != nullptr)
        {
            self->acrylic_backdrop_.reset();
        }
        if (message == WM_NCDESTROY && self != nullptr)
        {
            glance::contracts::log_event(L"MainWindow received WM_NCDESTROY.");
            self->text_editor_.reset();
            self->release_web_view_control();
            RemoveWindowSubclass(window, window_subclass, 1);
            self->stop_detached_focus_monitor();
            if (self->fullscreen_chrome_timer_ != nullptr)
            {
                self->fullscreen_chrome_timer_.Stop();
            }
            if (self->media_timer_ != nullptr)
            {
                self->media_timer_.Stop();
            }
            self->state_ = glance::contracts::PreviewWindowState::closed;
            if (self->state_callback_)
            {
                self->state_callback_(self->instance_id_, self->state_);
            }
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    void MainWindow::ShowPreview(
        std::vector<glance::app::PreviewFile> files,
        std::uint32_t focused_index,
        std::uint32_t source_kind,
        HWND source_window,
        std::wstring source_id,
        std::uint64_t source_capabilities)
    {
        const bool new_session = !visible_;
        const bool replace_deferred_session = defer_auto_fit_show_;
        leave_gallery(false);
        stop_detached_focus_monitor();
        preview_navigation_.clear();
        pending_folder_selection_path_.clear();
        pending_folder_scroll_offset_valid_ = false;
        pending_folder_focus_restore_ = false;
        files_ = std::move(files);
        source_kind_ = source_kind;
        source_window_ = source_window;
        source_id_ = std::move(source_id);
        source_capabilities_ = source_capabilities;
        current_index_ = files_.empty() ? 0 : std::min<std::uint32_t>(focused_index, static_cast<std::uint32_t>(files_.size() - 1));
        if (new_session)
        {
            topmost_ = false;
            pinned_ = false;
            detached_ = false;
            user_sized_ = false;
            TopmostButton().IsChecked(false);
            PinButton().IsChecked(false);
            update_window_action_visibility();
        }
        const auto current_kind = glance::app::resolve_preview_kind(files_[current_index_].path);
        defer_auto_fit_show_ = (new_session || replace_deferred_session) &&
            should_defer_auto_fit_show(current_kind);
        visible_ = true;

        FileList().Items().Clear();
        for (const auto& file : files_)
        {
            FileList().Items().Append(box_value(compact_file_list_name(file.display_name)));
        }
        if (files_.size() > 1)
        {
            FileList().SelectedIndex(static_cast<int>(current_index_));
        }
        update_preview_navigation_ui();

        const bool position_window = !topmost_ && (new_session || !user_sized_);
        present_file(current_index_, current_kind);
        if (position_window)
        {
            position_initial_window();
            component_placement_generation_ =
                current_kind_ == glance::app::PreviewKind::component
                ? content_generation_
                : 0;
        }
        update_state();
    }

    bool MainWindow::IsPreviewingFile(const std::wstring& path) const noexcept
    {
        return visible_ &&
            current_index_ < files_.size() &&
            CompareStringOrdinal(
                files_[current_index_].path.c_str(),
                -1,
                path.c_str(),
                -1,
                TRUE) == CSTR_EQUAL;
    }

    void MainWindow::CloseForReplacement()
    {
        stop_detached_focus_monitor();
        clear_preview_content();
        Close();
    }

    void MainWindow::HidePreview()
    {
        if (!visible_ || detached_)
        {
            return;
        }
        visible_ = false;
        defer_auto_fit_show_ = false;
        ShowWindow(window_, SW_HIDE);
        set_fullscreen(false);
        clear_preview_content();
        reset_hidden_window_size();
        state_ = glance::contracts::PreviewWindowState::hidden;
        if (state_callback_)
        {
            state_callback_(instance_id_, state_);
        }
    }

    bool MainWindow::ActivateSelectedFolderEntry()
    {
        const auto* entry = selected_folder_entry();
        if (entry == nullptr || current_index_ >= files_.size())
        {
            return false;
        }

        glance::app::PreviewFile child;
        child.display_name = entry->name;
        child.path = entry->path;
        child.parsing_name = entry->path;
        child.size = entry->original_size_known ? entry->original_size : 0;
        child.creation_time = entry->creation_time;
        child.last_write_time = entry->modified_time;
        child.attributes = entry->attributes;
        child.is_filesystem = true;
        child.is_cloud_placeholder =
            (entry->attributes &
                (FILE_ATTRIBUTE_OFFLINE |
                 FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                 FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0;

        PreviewNavigationEntry parent{
            std::move(files_[current_index_]),
            entry->path };
        parent.window_bounds_valid =
            GetWindowRect(window_, &parent.window_bounds) != FALSE;
        if (const auto scroller = find_scroll_viewer(FolderEntryList()))
        {
            parent.folder_scroll_offset = scroller.VerticalOffset();
            parent.folder_scroll_offset_valid = true;
        }
        preview_navigation_.push_back(std::move(parent));
        files_[current_index_] = std::move(child);
        pending_folder_selection_path_.clear();
        pending_folder_scroll_offset_valid_ = false;
        pending_folder_focus_restore_ = false;
        update_preview_navigation_ui();
        present_file(current_index_);
        return true;
    }

    bool MainWindow::NavigateBack()
    {
        if (fullscreen_)
        {
            set_fullscreen(false);
            return true;
        }
        if (preview_navigation_.empty() || current_index_ >= files_.size())
        {
            return false;
        }

        auto parent = std::move(preview_navigation_.back());
        preview_navigation_.pop_back();
        files_[current_index_] = std::move(parent.file);
        pending_folder_selection_path_ = std::move(parent.selected_path);
        pending_folder_scroll_offset_ = parent.folder_scroll_offset;
        pending_folder_scroll_offset_valid_ = parent.folder_scroll_offset_valid;
        pending_folder_focus_restore_ = true;
        update_preview_navigation_ui();
        present_file(current_index_);
        if (parent.window_bounds_valid)
        {
            SetWindowPos(
                window_,
                nullptr,
                parent.window_bounds.left,
                parent.window_bounds.top,
                parent.window_bounds.right - parent.window_bounds.left,
                parent.window_bounds.bottom - parent.window_bounds.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return true;
    }

    void MainWindow::update_preview_navigation_ui()
    {
        BackButton().Visibility(
            preview_navigation_.empty() ? Visibility::Collapsed : Visibility::Visible);
        const bool show_file_list = preview_navigation_.empty() && files_.size() > 1;
        FileListColumn().Width(GridLength{
            show_file_list ? 220.0 : 0.0,
            GridUnitType::Pixel });
        FileList().Visibility(show_file_list ? Visibility::Visible : Visibility::Collapsed);
    }

    const glance::app::ArchiveEntry* MainWindow::selected_folder_entry() noexcept
    {
        if (!archive_preview_is_directory_ || archive_render_state_ == nullptr)
        {
            return nullptr;
        }

        const int selected_index = FolderEntryList().SelectedIndex();
        if (selected_index < 0 ||
            static_cast<std::size_t>(selected_index) >=
                archive_render_state_->preview.entries.size())
        {
            return nullptr;
        }
        return &archive_render_state_->preview.entries[
            static_cast<std::size_t>(selected_index)];
    }

    void MainWindow::clear_preview_content()
    {
        leave_gallery(false);
        if (shell_file_cancellation_)
        {
            shell_file_cancellation_->store(true, std::memory_order_release);
            shell_file_cancellation_.reset();
        }
        if (image_load_operation_ != nullptr)
        {
            try
            {
                image_load_operation_.Cancel();
            }
            catch (...)
            {
            }
            image_load_operation_ = nullptr;
        }
        defer_auto_fit_show_ = false;
        cancel_pdf_render();
        cancel_archive_icon_load();
        glance::app::cancel_text_preview_read(current_text_reader_);
        clear_web_view_content();
        active_component_web_preview_.reset();
        active_component_file_directory_.reset();
        active_file_directory_descriptor_ = {};
        active_file_directory_columns_.clear();
        active_component_preview_.reset();
        active_component_refinement_.reset();
        component_refinement_text_.clear();
        component_refinement_started_ = false;
        component_loading_language_.clear();
        ++content_generation_;
        stop_media_playback();

        ImagePreview().Source(nullptr);
        ImageMetadataText().Blocks().Clear();
        ImageMetadataOverlay().Visibility(Visibility::Collapsed);
        reset_component_hover_info();
        component_hover_info_text_.clear();
        component_hover_cache_component_id_.clear();
        component_hover_cache_info_id_.clear();
        MediaCoverImage().Source(nullptr);
        MediaTitleText().Text(L"");
        MediaAlbumText().Text(L"");
        MediaArtistText().Text(L"");
        MediaTimeText().Text(L"0:00 / 0:00");
        PdfPageImage().Source(nullptr);
        PdfLoadingOverlay().Visibility(Visibility::Collapsed);
        PdfPageText().Text(L"");
        PdfThumbnailList().Items().Clear();
        PdfOutlineTree().RootNodes().Clear();
        hide_password_prompt();
        if (text_editor_ != nullptr)
        {
            text_editor_->clear();
            text_editor_->set_visible(false);
        }
        set_text_loading(false);
        TextEncodingText().Text(L"");
        dismiss_preview_info_bar();
        if (font_size_overlay_timer_ != nullptr)
        {
            font_size_overlay_timer_.Stop();
        }
        TextFontSizeOverlay().Visibility(Visibility::Collapsed);
        archive_render_state_.reset();
        archive_preview_is_directory_ = false;
        archive_entry_compressed_size_available_ = false;
        update_archive_header_state();
        ArchiveEntryTree().RootNodes().Clear();
        FolderEntryList().Items().Clear();
        ArchiveStatusText().Text(L"");
        preview_navigation_.clear();
        pending_folder_selection_path_.clear();
        pending_folder_scroll_offset_valid_ = false;
        pending_folder_focus_restore_ = false;
        BackButton().Visibility(Visibility::Collapsed);
        FileList().Items().Clear();
        FileList().Visibility(Visibility::Collapsed);
        FileListColumn().Width(GridLength{ 0, GridUnitType::Pixel });

        TitleText().Text(L"");
        FooterMetadataText().Text(L"");
        FileNameText().Text(L"");
        FilePathText().Text(L"");
        FileMetadataText().Text(L"");
        GenericFileIconImage().Source(nullptr);
        GenericFileIconImage().Visibility(Visibility::Collapsed);
        GenericFileFallbackIcon().Visibility(Visibility::Visible);
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        LoadCloudFileButton().Visibility(Visibility::Collapsed);
        PreviewAsTextButton().Visibility(Visibility::Collapsed);
        GenericAdvancedInfoButton().Visibility(Visibility::Collapsed);
        ComponentStatusControls().Children().Clear();
        PreviewModeButton().Visibility(Visibility::Collapsed);
        PreviewModeButton().IsChecked(false);
        ErrorText().Text(L"");
        ErrorText().Visibility(Visibility::Collapsed);
        ComponentLoadingText().Text(L"");
        ComponentLoadingText().Visibility(Visibility::Collapsed);
        GenericPanel().Visibility(Visibility::Visible);
        TextPanel().Visibility(Visibility::Collapsed);
        ImagePanel().Visibility(Visibility::Collapsed);
        MediaPanel().Visibility(Visibility::Collapsed);
        PdfPanel().Visibility(Visibility::Collapsed);
        ArchivePanel().Visibility(Visibility::Collapsed);
        TextStatusControls().Visibility(Visibility::Collapsed);
        ImageStatusControls().Visibility(Visibility::Collapsed);
        SyntaxHighlightButton().Visibility(Visibility::Collapsed);
        WordWrapButton().Visibility(Visibility::Collapsed);
        LineNumbersButton().Visibility(Visibility::Collapsed);

        current_text_.clear();
        current_text_path_.clear();
        current_text_markdown_ = false;
        current_text_web_ = false;
        web_preview_available_ = false;
        current_text_reader_.reset();
        current_text_has_more_ = false;
        text_chunk_loading_ = false;
        image_metadata_.clear();
        image_metadata_json_.clear();
        image_taken_time_.clear();
        media_dimensions_.clear();
        media_playback_info_.clear();
        media_playback_item_ = nullptr;
        media_playback_generation_ = 0;
        footer_access_mode_.clear();
        footer_access_loaded_ = false;
        footer_access_requested_ = false;
        files_.clear();
        current_index_ = 0;
        source_kind_ = 0;
        source_window_ = nullptr;
        foreground_when_unpinned_ = nullptr;
        current_kind_ = glance::app::PreviewKind::generic;
        content_preview_kind_ = glance::app::PreviewKind::generic;
        basic_info_mode_ = false;
        generic_text_preview_allowed_ = false;
        media_is_audio_ = false;
        image_metadata_visible_ = false;
        image_panning_ = false;
        image_pixel_width_ = 0;
        image_pixel_height_ = 0;
        image_bits_per_pixel_ = 0;
        pdf_panning_ = false;
        pdf_page_index_ = 0;
        pdf_page_count_ = 0;
        pdf_thumbnail_items_built_ = 0;
        pdf_source_path_.clear();
        pdf_password_.clear();
        pdf_outline_.clear();
        pdf_thumbnail_images_.clear();
        pdf_wheel_delta_ = 0;
        release_large_preview_buffers();
    }

    void MainWindow::release_large_preview_buffers()
    {
        const auto release_string = [](std::wstring& value) {
            if (value.capacity() > retained_preview_buffer_limit_bytes / sizeof(wchar_t))
            {
                std::wstring{}.swap(value);
            }
        };
        const auto release_vector = []<typename T>(std::vector<T>& value) {
            if (value.capacity() > retained_preview_buffer_limit_bytes / sizeof(T))
            {
                std::vector<T>{}.swap(value);
            }
        };

        release_string(current_text_);
        release_vector(files_);
        release_vector(pdf_outline_);
        release_vector(pdf_thumbnail_images_);
    }

    void MainWindow::cancel_pdf_render() noexcept
    {
        if (pdf_render_client_ != nullptr)
        {
            pdf_render_client_->close_document();
        }
    }

    void MainWindow::reset_hidden_window_size() noexcept
    {
        if (window_ == nullptr)
        {
            return;
        }
        const UINT dpi = GetDpiForWindow(window_);
        const auto preferences = glance::app::load_window_preferences();
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            MulDiv(static_cast<int>(preferences.default_width), static_cast<int>(dpi), 96),
            MulDiv(static_cast<int>(preferences.default_height), static_cast<int>(dpi), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void MainWindow::position_initial_window(bool ignore_saved_size)
    {
        if (fullscreen_)
        {
            reveal_deferred_preview();
            return;
        }
        const auto preferences = glance::app::load_window_preferences();
        const auto storage_kind = basic_info_mode_ ? content_preview_kind_ : current_kind_;
        const HWND reference_window = source_window_ != nullptr
            ? source_window_
            : GetForegroundWindow();
        HMONITOR monitor = MonitorFromWindow(reference_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        GetMonitorInfoW(monitor, &info);

        const UINT dpi = reference_window != nullptr ? GetDpiForWindow(reference_window) : 96;
        const POINT saved_center_offset = preferences.remember_position
            ? glance::app::load_window_center_offset(storage_kind, media_is_audio_).value_or(POINT{})
            : POINT{};
        const POINT center_offset{
            MulDiv(saved_center_offset.x, static_cast<int>(dpi), 96),
            MulDiv(saved_center_offset.y, static_cast<int>(dpi), 96) };
        int desired_width = MulDiv(static_cast<int>(preferences.default_width), static_cast<int>(dpi), 96);
        int desired_height = MulDiv(static_cast<int>(preferences.default_height), static_cast<int>(dpi), 96);
        if (preferences.remember_size && !ignore_saved_size && !auto_fit_applies())
        {
            if (const auto saved_size = glance::app::load_window_size(storage_kind, media_is_audio_))
            {
                desired_width = MulDiv(saved_size->cx, static_cast<int>(dpi), 96);
                desired_height = MulDiv(saved_size->cy, static_cast<int>(dpi), 96);
            }
        }
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        const int minimum_width = MulDiv(480, static_cast<int>(dpi), 96);
        const int minimum_height = MulDiv(320, static_cast<int>(dpi), 96);
        const int width = std::clamp(desired_width, std::min(minimum_width, work_width), work_width);
        const int height = std::clamp(desired_height, std::min(minimum_height, work_height), work_height);
        const auto position = window_position_for_center_offset(
            info.rcMonitor,
            info.rcWork,
            width,
            height,
            center_offset);

        SetWindowPos(
            window_,
            HWND_TOPMOST,
            position.x,
            position.y,
            width,
            height,
            SWP_NOACTIVATE);
        if (!topmost_)
        {
            SetWindowPos(
                window_,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (!defer_auto_fit_show_)
        {
            show_prepared_window();
        }
    }

    bool MainWindow::should_defer_auto_fit_show(glance::app::PreviewKind kind) const noexcept
    {
        if (current_index_ >= files_.size())
        {
            return false;
        }
        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder ||
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        const auto preferences = glance::app::load_window_preferences();
        if (!preferences.auto_fit_media || !preferences.show_after_auto_fit ||
            glance::app::auto_fit_ignores_path(preferences, file.path))
        {
            return false;
        }

        return kind == glance::app::PreviewKind::image ||
            kind == glance::app::PreviewKind::document ||
            (kind == glance::app::PreviewKind::media &&
             glance::app::gallery_media_kind(file.path) !=
                 glance::contracts::components::GalleryMediaKind::audio);
    }

    void MainWindow::show_prepared_window() noexcept
    {
        if (window_ == nullptr)
        {
            return;
        }

        BOOL cloaked = TRUE;
        const bool cloak_applied = SUCCEEDED(
            DwmSetWindowAttribute(window_, DWMWA_CLOAK, &cloaked, sizeof(cloaked)));
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        if (cloak_applied)
        {
            static_cast<void>(DwmFlush());
            cloaked = FALSE;
            static_cast<void>(
                DwmSetWindowAttribute(window_, DWMWA_CLOAK, &cloaked, sizeof(cloaked)));
        }
    }

    void MainWindow::reveal_deferred_preview() noexcept
    {
        if (!defer_auto_fit_show_ || window_ == nullptr)
        {
            return;
        }
        defer_auto_fit_show_ = false;
        show_prepared_window();
    }

    bool MainWindow::auto_fit_applies(bool dynamic_update) const noexcept
    {
        const auto preferences = glance::app::load_window_preferences();
        if (fullscreen_ || topmost_ || user_sized_ || !preferences.auto_fit_media ||
            (dynamic_update && !preferences.dynamic_auto_fit) ||
            (current_index_ < files_.size() &&
                glance::app::auto_fit_ignores_path(preferences, files_[current_index_].path)))
        {
            return false;
        }
        return current_kind_ == glance::app::PreviewKind::image ||
            current_kind_ == glance::app::PreviewKind::document ||
            (current_kind_ == glance::app::PreviewKind::media && !media_is_audio_);
    }

    void MainWindow::auto_fit_window_to_content(
        double content_width,
        double content_height,
        bool dynamic_update) noexcept
    {
        if (window_ == nullptr)
        {
            return;
        }
        if (!auto_fit_applies(dynamic_update) ||
            content_width <= 0.0 || content_height <= 0.0)
        {
            reveal_deferred_preview();
            return;
        }

        RECT bounds{};
        if (!GetWindowRect(window_, &bounds))
        {
            reveal_deferred_preview();
            return;
        }
        HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            reveal_deferred_preview();
            return;
        }

        FrameworkElement panel = current_kind_ == glance::app::PreviewKind::image
            ? ImagePanel().as<FrameworkElement>()
            : current_kind_ == glance::app::PreviewKind::media
                ? MediaPanel().as<FrameworkElement>()
                : PdfPanel().as<FrameworkElement>();
        const int current_width = bounds.right - bounds.left;
        const int current_height = bounds.bottom - bounds.top;
        int horizontal_chrome = std::max(
            0,
            current_width - static_cast<int>(std::lround(panel.ActualWidth())));
        if (current_kind_ == glance::app::PreviewKind::document)
        {
            horizontal_chrome += static_cast<int>(std::lround(PdfNavigationColumn().ActualWidth()));
        }
        const int vertical_chrome = std::max(0, current_height - static_cast<int>(std::lround(panel.ActualHeight())));
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        const auto preferences = glance::app::load_window_preferences();
        const double maximum_fraction = preferences.adaptive_maximum_percent / 100.0;
        const double minimum_fraction = preferences.adaptive_minimum_percent / 100.0;
        const int maximum_width = std::max(
            1,
            static_cast<int>(std::floor(work_width * maximum_fraction)));
        const int maximum_height = std::max(
            1,
            static_cast<int>(std::floor(work_height * maximum_fraction)));
        const int adaptive_minimum_width = std::max(
            1,
            static_cast<int>(std::ceil(work_width * minimum_fraction)));
        const int adaptive_minimum_height = std::max(
            1,
            static_cast<int>(std::ceil(work_height * minimum_fraction)));
        const UINT dpi = GetDpiForWindow(window_);
        const int minimum_width = std::min(maximum_width, MulDiv(480, static_cast<int>(dpi), 96));
        const int minimum_height = std::min(maximum_height, MulDiv(320, static_cast<int>(dpi), 96));

        const double maximum_content_width = std::max(1, maximum_width - horizontal_chrome);
        const double maximum_content_height = std::max(1, maximum_height - vertical_chrome);
        const double minimum_content_width = std::max(1, minimum_width - horizontal_chrome);
        const double minimum_content_height = std::max(1, minimum_height - vertical_chrome);
        const double interface_lower_scale = std::max(
            minimum_content_width / content_width,
            minimum_content_height / content_height);
        const double adaptive_lower_scale = std::min(
            std::max(0, adaptive_minimum_width - horizontal_chrome) / content_width,
            std::max(0, adaptive_minimum_height - vertical_chrome) / content_height);
        const double lower_scale = std::max(interface_lower_scale, adaptive_lower_scale);
        const double upper_scale = std::min(
            maximum_content_width / content_width,
            maximum_content_height / content_height);
        const double scale = lower_scale > upper_scale
            ? upper_scale
            : std::clamp(1.0, lower_scale, upper_scale);
        const int width = std::clamp(
            static_cast<int>(std::lround(content_width * scale)) + horizontal_chrome,
            minimum_width,
            maximum_width);
        const int height = std::clamp(
            static_cast<int>(std::lround(content_height * scale)) + vertical_chrome,
            minimum_height,
            maximum_height);
        const auto storage_kind = basic_info_mode_ ? content_preview_kind_ : current_kind_;
        const POINT saved_center_offset = preferences.remember_position
            ? glance::app::load_window_center_offset(storage_kind, media_is_audio_).value_or(POINT{})
            : POINT{};
        const POINT center_offset{
            MulDiv(saved_center_offset.x, static_cast<int>(dpi), 96),
            MulDiv(saved_center_offset.y, static_cast<int>(dpi), 96) };
        const auto position = window_position_for_center_offset(
            info.rcMonitor,
            info.rcWork,
            width,
            height,
            center_offset);
        const bool reveal_after_resize = defer_auto_fit_show_;
        if (SetWindowPos(
            window_,
            nullptr,
            position.x,
            position.y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOZORDER))
        {
            if (reveal_after_resize)
            {
                reveal_deferred_preview();
            }
        }
        else if (reveal_after_resize)
        {
            reveal_deferred_preview();
        }
    }

    void MainWindow::save_current_window_placement() const noexcept
    {
        if (!visible_ || fullscreen_ || window_ == nullptr || IsZoomed(window_) || detached_ ||
            (pinned_ && topmost_))
        {
            return;
        }
        const auto preferences = glance::app::load_window_preferences();
        if (!preferences.remember_size && !preferences.remember_position)
        {
            return;
        }
        RECT bounds{};
        if (!GetWindowRect(window_, &bounds))
        {
            return;
        }
        const auto storage_kind = basic_info_mode_ ? content_preview_kind_ : current_kind_;
        if (preferences.remember_size && user_sized_)
        {
            const UINT dpi = GetDpiForWindow(window_);
            glance::app::save_window_size(
                storage_kind,
                SIZE{
                    MulDiv(bounds.right - bounds.left, 96, static_cast<int>(dpi)),
                    MulDiv(bounds.bottom - bounds.top, 96, static_cast<int>(dpi)) },
                media_is_audio_);
        }
        if (preferences.remember_position)
        {
            HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{ sizeof(MONITORINFO) };
            if (!GetMonitorInfoW(monitor, &info))
            {
                return;
            }
            const int window_center_x = bounds.left + (bounds.right - bounds.left) / 2;
            const int window_center_y = bounds.top + (bounds.bottom - bounds.top) / 2;
            const int monitor_center_x =
                info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left) / 2;
            const int monitor_center_y =
                info.rcMonitor.top + (info.rcMonitor.bottom - info.rcMonitor.top) / 2;
            const UINT dpi = GetDpiForWindow(window_);
            glance::app::save_window_center_offset(
                storage_kind,
                POINT{
                    MulDiv(window_center_x - monitor_center_x, 96, static_cast<int>(dpi)),
                    MulDiv(window_center_y - monitor_center_y, 96, static_cast<int>(dpi)) },
                media_is_audio_);
        }
    }

    void MainWindow::present_file(
        std::uint32_t index,
        std::optional<glance::app::PreviewKind> known_kind)
    {
        if (index >= files_.size())
        {
            return;
        }
        if (shell_file_cancellation_)
        {
            shell_file_cancellation_->store(true, std::memory_order_release);
            shell_file_cancellation_.reset();
        }
        if (image_load_operation_ != nullptr)
        {
            try
            {
                if (image_load_operation_.Status() == Windows::Foundation::AsyncStatus::Started)
                {
                    image_load_operation_.Cancel();
                }
            }
            catch (...)
            {
            }
            image_load_operation_ = nullptr;
        }
        cancel_gallery_preloads();
        auto previous_component_preview = std::move(active_component_preview_);
        auto previous_component_file_directory =
            std::move(active_component_file_directory_);
        auto previous_component_refinement = std::move(active_component_refinement_);
        active_component_web_preview_.reset();
        component_refinement_text_.clear();
        component_refinement_started_ = false;
        cancel_pdf_render();
        cancel_archive_icon_load();
        hide_password_prompt();
        current_index_ = index;
        component_loading_language_.clear();
        basic_info_mode_ = false;
        content_preview_kind_ = glance::app::PreviewKind::generic;
        generic_text_preview_allowed_ = false;
        archive_render_state_.reset();
        active_file_directory_descriptor_ = {};
        active_file_directory_columns_.clear();
        archive_preview_is_directory_ = false;
        archive_entry_compressed_size_available_ = false;
        update_archive_header_state();
        update_preview_mode_button();
        dismiss_preview_info_bar();
        pdf_thumbnail_selection_updating_ = true;
        PdfThumbnailList().Items().Clear();
        PdfOutlineTree().RootNodes().Clear();
        pdf_thumbnail_selection_updating_ = false;
        PdfThumbnailList().Visibility(Visibility::Visible);
        PdfOutlineTree().Visibility(Visibility::Collapsed);
        PdfThumbnailsButton().IsChecked(true);
        PdfOutlineButton().IsChecked(false);
        PdfOutlineButton().IsEnabled(false);
        pdf_thumbnail_images_.clear();
        pdf_thumbnail_items_built_ = 0;
        pdf_outline_.clear();
        pdf_panning_ = false;
        pdf_page_index_ = 0;
        pdf_page_count_ = 0;
        const auto& file = files_[index];
        gallery_media_kind_ = !file.is_filesystem || file.path.empty()
            ? glance::contracts::components::GalleryMediaKind::none
            : glance::app::gallery_media_kind(file.path);
        const auto media_preferences = glance::app::load_media_preview_preferences();
        middle_click_gallery_enabled_ = media_preferences.middle_click_gallery_mode;
        loop_gallery_enabled_ = media_preferences.loop_gallery_scrolling;
        gallery_same_extension_only_ = media_preferences.gallery_same_extension_only;
        const auto generation = ++content_generation_;
        update_title_text();
        image_pixel_width_ = 0;
        image_pixel_height_ = 0;
        image_bits_per_pixel_ = 0;
        image_taken_time_.clear();
        media_dimensions_.clear();
        media_playback_info_.clear();
        media_playback_item_ = nullptr;
        media_playback_generation_ = 0;
        footer_access_mode_.clear();
        footer_access_loaded_ = false;
        footer_access_requested_ = false;
        update_footer_metadata();
        request_footer_access_if_needed();

        const bool from_explorer = source_kind_ == 1;
        OpenFolderButton().Visibility(from_explorer ? Visibility::Collapsed : Visibility::Visible);
        const bool open_target_available = file.is_filesystem
            ? !file.path.empty()
            : !file.parsing_name.empty();
        OpenDefaultButton().Visibility(
            open_target_available ? Visibility::Visible : Visibility::Collapsed);

        if (file.is_cloud_placeholder)
        {
            current_kind_ = glance::app::PreviewKind::generic;
            present_generic(file);
            return;
        }

        if (file.path.empty())
        {
            current_kind_ = glance::app::PreviewKind::generic;
            present_generic(file);
            if (!file.is_filesystem &&
                !file.parsing_name.empty() &&
                (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                ComponentLoadingText().Text(glance::app::localize(L"LoadingDeviceFile"));
                ComponentLoadingText().Visibility(Visibility::Visible);
                shell_file_cancellation_ = std::make_shared<std::atomic_bool>(false);
                materialize_shell_file_async(
                    index,
                    file.parsing_name,
                    file.display_name,
                    file.shell_id_list,
                    generation,
                    shell_file_cancellation_);
            }
            return;
        }

        if ((file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            folder_preview_preferences_ = glance::app::load_folder_preview_preferences();
            archive_preview_is_directory_ = true;
            update_archive_header_state();
            content_preview_kind_ = glance::app::PreviewKind::archive;
            update_preview_mode_button();
            current_kind_ = glance::app::PreviewKind::archive;
            show_content_panel(current_kind_);
            ArchiveStatusText().Text(glance::app::localize(L"LoadingFolder"));
            ArchiveEntryTree().RootNodes().Clear();
            FolderEntryList().Items().Clear();
            load_directory_async(file.path, generation);
            return;
        }

        const auto kind = known_kind
            ? *known_kind
            : glance::app::resolve_preview_kind(file.path);
        if (kind == glance::app::PreviewKind::component)
        {
            present_generic(file, false, false);
            content_preview_kind_ = kind;
            current_kind_ = kind;
            update_preview_mode_button();
            load_component_async(file.path, generation);
            return;
        }
        present_resolved_file(file, kind, generation);
    }

    void MainWindow::present_resolved_file(
        const glance::app::PreviewFile& file,
        glance::app::PreviewKind kind,
        std::uint64_t generation)
    {
        content_preview_kind_ = kind;
        update_preview_mode_button();
        current_kind_ = kind;
        switch (kind)
        {
        case glance::app::PreviewKind::text:
            present_text(file, false);
            break;
        case glance::app::PreviewKind::markdown:
            present_text(file, true);
            break;
        case glance::app::PreviewKind::web:
            present_text(file, false, true);
            break;
        case glance::app::PreviewKind::image:
        {
            show_content_panel(kind);
            image_zoom_map_enabled_ =
                glance::app::load_media_preview_preferences().show_image_zoom_map;
            image_rotation_ = 0;
            image_scale_x_ = 1.0;
            image_scale_y_ = 1.0;
            image_panning_ = false;
            image_pixel_width_ = 0;
            image_pixel_height_ = 0;
            image_bits_per_pixel_ = 0;
            image_metadata_.clear();
            image_metadata_json_.clear();
            image_taken_time_.clear();
            image_metadata_visible_ = false;
            ImageTransform().Rotation(image_rotation_);
            ImageTransform().ScaleX(image_scale_x_);
            ImageTransform().ScaleY(image_scale_y_);
            update_image_transform_controls();
            ImagePreview().Source(nullptr);
            ImageExifButton().IsChecked(false);
            ImageMetadataText().Blocks().Clear();
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
            ImageZoomMapOverlay().Visibility(Visibility::Collapsed);
            fit_image_to_viewport();
            bool first_frame_presented{};
            if (pending_gallery_image_ &&
                gallery_image_cache_key(pending_gallery_image_->file) ==
                    gallery_image_cache_key(file))
            {
                image_pixel_width_ = pending_gallery_image_->pixel_width;
                image_pixel_height_ = pending_gallery_image_->pixel_height;
                ImagePreview().Source(pending_gallery_image_->bitmap);
                fit_image_to_viewport();
                update_footer_metadata();
                auto_fit_window_to_content(image_pixel_width_, image_pixel_height_);
                first_frame_presented = true;
            }
            pending_gallery_image_.reset();
            image_load_operation_ = load_image_async(
                file.path,
                generation,
                first_frame_presented);
            if (glance::app::footer_field_enabled(
                    footer_preferences_, glance::app::FooterField::media_info))
            {
                load_image_media_info_async(file.path, generation);
            }
            load_image_metadata_async(
                files_[current_index_].path,
                generation,
                active_component_preview_);
            break;
        }
        case glance::app::PreviewKind::media:
            show_content_panel(kind);
            media_is_audio_ = gallery_media_kind_ ==
                glance::contracts::components::GalleryMediaKind::audio;
            media_seek_wheel_delta_ = 0;
            media_volume_wheel_delta_ = 0;
            {
                const auto preferences = glance::app::load_media_preview_preferences();
                MediaVolumeSlider().Value(
                    media_is_audio_
                        ? preferences.audio_volume_percent
                        : preferences.video_volume_percent);
                reverse_media_seek_wheel_ = preferences.reverse_seek_wheel;
            }
            MediaCoverImage().Source(nullptr);
            MediaCoverImage().Visibility(Visibility::Collapsed);
            MediaCoverPlaceholder().Visibility(Visibility::Visible);
            MediaTitleText().Text(file.display_name);
            MediaAlbumText().Text(L"");
            MediaArtistText().Text(L"");
            media_dimensions_.clear();
            media_playback_info_.clear();
            media_playback_item_ = nullptr;
            media_playback_generation_ = 0;
            media_controls_idle_ticks_ = 0;
            show_media_controls();
            load_media_async(file.path, generation);
            break;
        case glance::app::PreviewKind::document:
            show_content_panel(kind);
            PdfPageImage().Source(nullptr);
            pdf_wheel_delta_ = 0;
            PdfPageText().Text(glance::app::localize(L"Loading"));
            PdfLoadingText().Text(glance::app::localize(L"LoadingPdf"));
            PdfLoadingText().Visibility(Visibility::Visible);
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_pdf_async(file.path, generation);
            break;
        default:
            present_generic(file, true, true);
            break;
        }
    }

    void MainWindow::present_generic(
        const glance::app::PreviewFile& file,
        bool allow_text_preview,
        bool allow_advanced_info)
    {
        auto previous_component_preview = std::move(active_component_preview_);
        auto previous_component_file_directory =
            std::move(active_component_file_directory_);
        auto previous_component_refinement = std::move(active_component_refinement_);
        active_component_web_preview_.reset();
        active_file_directory_descriptor_ = {};
        active_file_directory_columns_.clear();
        component_refinement_text_.clear();
        component_refinement_started_ = false;
        current_kind_ = glance::app::PreviewKind::generic;
        show_content_panel(glance::app::PreviewKind::generic);
        FileNameText().Text(file.display_name);
        FilePathText().Text(
            file.is_filesystem ? file.path : file.parsing_name);
        update_generic_file_metadata();
        GenericFileIconImage().Source(nullptr);
        GenericFileIconImage().Visibility(Visibility::Collapsed);
        GenericFileFallbackIcon().Visibility(Visibility::Visible);
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        generic_preview_preferences_ = glance::app::load_generic_preview_preferences();
        GenericAdvancedInfoButton().IsChecked(generic_preview_preferences_.show_advanced_info);
        LoadCloudFileButton().Visibility(file.is_cloud_placeholder ? Visibility::Visible : Visibility::Collapsed);
        generic_text_preview_allowed_ = allow_text_preview;
        update_preview_as_text_button();
        PreviewAsTextButton().IsEnabled(true);
        const bool advanced_info_available =
            allow_advanced_info && file.is_filesystem && !file.path.empty() &&
            !file.is_cloud_placeholder &&
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        GenericAdvancedInfoButton().Visibility(
            advanced_info_available ? Visibility::Visible : Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
        ComponentLoadingText().Visibility(Visibility::Collapsed);
        const auto icon_path = file.is_filesystem ? file.path : file.parsing_name;
        if (!icon_path.empty())
        {
            load_generic_icon_async(
                icon_path,
                (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                file.is_cloud_placeholder || !file.is_filesystem,
                content_generation_);
        }
        if (advanced_info_available && generic_preview_preferences_.show_advanced_info)
        {
            load_generic_file_info_async(file.path, content_generation_);
        }
        update_footer_metadata();
    }

    fire_and_forget MainWindow::materialize_shell_file_async(
        std::uint32_t index,
        std::wstring parsing_name,
        std::wstring display_name,
        std::vector<std::uint8_t> shell_id_list,
        std::uint64_t generation,
        std::shared_ptr<std::atomic_bool> cancellation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto result = glance::app::materialize_shell_file(
            parsing_name,
            display_name,
            shell_id_list,
            cancellation);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            index,
            generation,
            cancellation = std::move(cancellation),
            result = std::move(result)]() mutable {
            if (generation != lifetime->content_generation_ ||
                index >= lifetime->files_.size() ||
                cancellation != lifetime->shell_file_cancellation_)
            {
                return;
            }
            lifetime->shell_file_cancellation_.reset();
            if (result.cancelled)
            {
                return;
            }
            if (result.path.empty() || result.lease == nullptr)
            {
                glance::contracts::log_event(
                    L"Shell item materialization failed at " +
                    result.error_stage + L": " +
                    std::to_wstring(result.error));
                lifetime->ComponentLoadingText().Visibility(Visibility::Collapsed);
                lifetime->ErrorText().Text(
                    glance::app::localize(L"DeviceFileReadError"));
                lifetime->ErrorText().Visibility(Visibility::Visible);
                return;
            }

            auto& file = lifetime->files_[index];
            file.path = std::move(result.path);
            file.materialized_lease = std::move(result.lease);
            file.size = result.size;
            if (result.creation_time != 0)
            {
                file.creation_time = result.creation_time;
            }
            if (result.last_write_time != 0)
            {
                file.last_write_time = result.last_write_time;
            }
            lifetime->present_file(index);
        }));
    }

    fire_and_forget MainWindow::load_generic_icon_async(
        std::wstring path,
        bool is_folder,
        bool use_file_attributes,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto bitmap = glance::app::load_shell_icon(path, is_folder, 64, use_file_attributes);
        if (bitmap == nullptr)
        {
            co_return;
        }
        static_cast<void>(dispatcher.TryEnqueue([lifetime, bitmap = std::move(bitmap), generation]() {
            if (generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::generic)
            {
                return;
            }
            try
            {
                const auto source = glance::app::create_shell_icon_source(*bitmap);
                if (source == nullptr)
                {
                    return;
                }
                lifetime->GenericFileIconImage().Source(source);
                lifetime->GenericFileIconImage().Visibility(Visibility::Visible);
                lifetime->GenericFileFallbackIcon().Visibility(Visibility::Collapsed);
            }
            catch (...)
            {
            }
        }));
    }

    fire_and_forget MainWindow::load_generic_file_info_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto info = glance::app::load_generic_file_info(path);
        static_cast<void>(dispatcher.TryEnqueue([lifetime, generation, info = std::move(info)]() mutable {
            if (generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::generic ||
                !lifetime->generic_preview_preferences_.show_advanced_info ||
                info.empty())
            {
                return;
            }
            lifetime->GenericAdvancedInfoText().Text(std::move(info));
            lifetime->GenericAdvancedInfoScroller().Visibility(Visibility::Visible);
        }));
    }

    fire_and_forget MainWindow::load_footer_access_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto access = glance::app::load_file_access_mode(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, generation, access = std::move(access)]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->footer_access_requested_ = false;
                lifetime->footer_access_loaded_ = true;
                lifetime->footer_access_mode_ = access.value_or(L"--");
                lifetime->update_footer_metadata();
            }));
    }

    bool MainWindow::prepare_text_preview(
        const glance::app::PreviewFile& file,
        bool markdown,
        bool web)
    {
        clear_web_view_content();
        glance::app::cancel_text_preview_read(current_text_reader_);
        if (!ensure_text_editor())
        {
            set_text_loading(false);
            show_provider_error(
                text_editor_ != nullptr
                    ? text_editor_->error()
                    : L"Unable to initialize the text preview.",
                content_generation_);
            return false;
        }
        current_kind_ = web
            ? glance::app::PreviewKind::web
            : markdown
                ? glance::app::PreviewKind::markdown
                : glance::app::PreviewKind::text;
        show_content_panel(current_kind_);
        current_text_.clear();
        current_text_path_ = file.path;
        current_text_markdown_ = markdown;
        current_text_web_ = web;
        web_preview_available_ =
            (markdown || web) && glance::app::webview_runtime_available();
        if (web_preview_ != nullptr)
        {
            const bool dark = RootGrid().ActualTheme() == ElementTheme::Dark;
            web_preview_.DefaultBackgroundColor(
                web
                    ? Windows::UI::Color{ 255, 255, 255, 255 }
                    : dark
                        ? Windows::UI::Color{ 255, 32, 32, 32 }
                        : Windows::UI::Color{ 255, 255, 255, 255 });
        }
        current_text_reader_.reset();
        current_text_has_more_ = false;
        text_chunk_loading_ = true;
        current_text_encoding_ = glance::app::TextEncoding::automatic;
        markdown_preview_ = web_preview_available_;
        EncodingSelector().Content(box_value(glance::app::localize(L"EncodingDetecting")));
        apply_text_preferences();
        text_editor_->clear();
        text_editor_->set_file_path(file.path);
        text_editor_->set_preferences(
            text_preferences_,
            syntax_highlighting_,
            RootGrid().ActualTheme() == ElementTheme::Dark);
        set_text_loading(true);
        const bool component_web =
            web && active_component_web_preview_ != nullptr;
        MarkdownModeButtons().Visibility(
            web_preview_available_ && !component_web
                ? Visibility::Visible
                : Visibility::Collapsed);
        MarkdownPreviewButton().IsEnabled(web_preview_available_);
        MarkdownCodeButton().IsEnabled(true);
        set_markdown_preview_mode(web_preview_available_);
        if (web_preview_available_)
        {
            if (web_preview_ != nullptr)
            {
                web_preview_.Opacity(0.0);
            }
            if (markdown && !web_view_ready_)
            {
                initialize_markdown_web_view_async(content_generation_);
            }
        }
        return true;
    }

    void MainWindow::present_text(
        const glance::app::PreviewFile& file,
        bool markdown,
        bool web)
    {
        if (!prepare_text_preview(file, markdown, web))
        {
            return;
        }
        if (web && web_preview_available_)
        {
            render_web_document_async(file.path, content_generation_);
        }
        if (web && active_component_web_preview_ != nullptr)
        {
            set_text_loading(false);
            return;
        }
        load_text_async(file.path, markdown, web, content_generation_, current_text_encoding_);
    }

    fire_and_forget MainWindow::load_text_async(
        std::wstring path,
        bool markdown,
        bool web,
        std::uint64_t generation,
        glance::app::TextEncoding encoding,
        bool preview_as_text_attempt)
    {
        glance::app::cancel_text_preview_read(current_text_reader_);
        text_chunk_loading_ = true;
        current_text_has_more_ = false;
        current_text_reader_.reset();
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        const auto initial_bytes = markdown
            ? static_cast<std::size_t>(maximum_preview_as_text_bytes)
            : text_chunk_bytes;
        auto preview = glance::app::load_text_preview(path, initial_bytes, encoding);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), markdown, web, generation, preview_as_text_attempt]() mutable {
                lifetime->apply_text_preview(
                    std::move(preview),
                    markdown,
                    web,
                    generation,
                    preview_as_text_attempt);
            }));
    }

    Windows::Foundation::IAsyncAction MainWindow::load_image_async(
        std::wstring path,
        std::uint64_t generation,
        bool first_frame_presented)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        auto cancellation = co_await winrt::get_cancellation_token();
        cancellation.enable_propagation();
        try
        {
            const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto properties = co_await file.Properties().GetImagePropertiesAsync();
            const auto stream = co_await file.OpenReadAsync();
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            const auto width = properties.Width() != 0 ? properties.Width() : static_cast<std::uint32_t>(bitmap.PixelWidth());
            const auto height = properties.Height() != 0 ? properties.Height() : static_cast<std::uint32_t>(bitmap.PixelHeight());
            static_cast<void>(dispatcher.TryEnqueue([
                lifetime,
                bitmap,
                generation,
                width,
                height,
                first_frame_presented] {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->image_pixel_width_ = width;
                lifetime->image_pixel_height_ = height;
                lifetime->ImagePreview().Source(bitmap);
                lifetime->update_footer_metadata();
                if (!first_frame_presented)
                {
                    lifetime->fit_image_to_viewport();
                    lifetime->auto_fit_window_to_content(width, height);
                    const auto weak = lifetime->get_weak();
                    static_cast<void>(lifetime->DispatcherQueue().TryEnqueue(
                        Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                        [weak, generation] {
                            const auto self = weak.get();
                            if (self == nullptr ||
                                generation != self->content_generation_ ||
                                self->current_kind_ != glance::app::PreviewKind::image)
                            {
                                return;
                            }
                            self->fit_image_to_viewport();
                        }));
                }
                else
                {
                    lifetime->update_image_zoom_map();
                }
                lifetime->begin_component_refinement(generation);
            }));
        }
        catch (const hresult_canceled&)
        {
        }
        catch (const hresult_error& error)
        {
            const auto message = glance::app::localize_format(L"ImageDecodeError", { error.message() });
            static_cast<void>(dispatcher.TryEnqueue([lifetime, message, generation] {
                lifetime->show_provider_error(message, generation);
            }));
        }
    }

    fire_and_forget MainWindow::load_image_metadata_async(
        std::wstring path,
        std::uint64_t generation,
        std::shared_ptr<void> component_preview)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto metadata = glance::app::load_image_metadata(path);
        if (component_preview != nullptr)
        {
            glance::app::merge_component_image_metadata(
                metadata,
                glance::app::query_component_image_metadata(component_preview));
        }
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, metadata = std::move(metadata), generation]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->image_taken_time_ = std::move(metadata.taken_time);
                lifetime->image_metadata_ = format_image_metadata(metadata);
                lifetime->image_metadata_json_ = format_image_metadata_json(metadata);
                const auto display_text = lifetime->image_metadata_.empty()
                    ? glance::app::localize(L"NoImageMetadata")
                    : lifetime->image_metadata_;
                set_information_panel_text(
                    lifetime->ImageMetadataText(),
                    display_text,
                    !lifetime->image_metadata_.empty());
                lifetime->update_image_metadata_visibility();
                lifetime->update_footer_metadata();
            }));
    }

    fire_and_forget MainWindow::load_image_media_info_async(
        std::wstring path,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        const auto bit_depth = glance::app::load_image_bit_depth(path);
        static_cast<void>(dispatcher.TryEnqueue([lifetime, bit_depth, generation] {
            if (generation != lifetime->content_generation_ ||
                !glance::app::footer_field_enabled(
                    lifetime->footer_preferences_,
                    glance::app::FooterField::media_info))
            {
                return;
            }
            lifetime->image_bits_per_pixel_ = bit_depth;
            lifetime->update_footer_metadata();
        }));
    }

    fire_and_forget MainWindow::load_media_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
        const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
        const auto source = Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
        const Windows::Media::Playback::MediaPlaybackItem playback_item(source);
        if (generation != content_generation_ ||
            state_ == glance::contracts::PreviewWindowState::closed)
        {
            co_return;
        }

            media_is_audio_ = gallery_media_kind_ ==
                glance::contracts::components::GalleryMediaKind::audio;
            AudioMetadataPanel().Visibility(media_is_audio_ ? Visibility::Visible : Visibility::Collapsed);
            MediaPreview().Visibility(media_is_audio_ ? Visibility::Collapsed : Visibility::Visible);
            if (media_is_audio_)
            {
                MediaControlsOverlay().Background(Application::Current().Resources().Lookup(
                    box_value(L"LayerFillColorDefaultBrush")).as<Media::Brush>());
                MediaPlayPauseIcon().ClearValue(IconElement::ForegroundProperty());
                MediaMuteIcon().ClearValue(IconElement::ForegroundProperty());
                MediaTimeText().ClearValue(TextBlock::ForegroundProperty());
            }
            else
            {
                MediaControlsOverlay().Background(Media::SolidColorBrush(Windows::UI::Color{ 153, 0, 0, 0 }));
                const auto white = Media::SolidColorBrush(Windows::UI::Color{ 255, 255, 255, 255 });
                MediaPlayPauseIcon().Foreground(white);
                MediaMuteIcon().Foreground(white);
                MediaTimeText().Foreground(white);
            }
            update_media_surface_background();
            media_playback_item_ = playback_item;
            media_playback_generation_ = generation;
            MediaPreview().Source(playback_item);
            MediaPreview().MediaPlayer().IsMuted(false);
            MediaPreview().MediaPlayer().Volume(MediaVolumeSlider().Value() / 100.0);
            const auto preferences = glance::app::load_media_preview_preferences();
            if (media_is_audio_ ? preferences.autoplay_audio : preferences.autoplay_video)
            {
                MediaPreview().MediaPlayer().Play();
            }
            media_timer_.Start();

            if (media_is_audio_)
            {
                try
                {
                    const auto properties = co_await file.Properties().GetMusicPropertiesAsync();
                    if (generation != content_generation_ ||
                        state_ == glance::contracts::PreviewWindowState::closed)
                    {
                        co_return;
                    }
                    const std::wstring title = properties.Title().empty()
                        ? std::filesystem::path(path).stem().wstring()
                        : std::wstring(properties.Title());
                    std::wstring artist(properties.Artist());
                    if (artist.empty())
                    {
                        artist = std::wstring(properties.AlbumArtist());
                    }
                    MediaTitleText().Text(title);
                    MediaAlbumText().Text(properties.Album());
                    MediaArtistText().Text(artist);

                    const auto thumbnail = co_await file.GetThumbnailAsync(
                        Windows::Storage::FileProperties::ThumbnailMode::MusicView,
                        320);
                    if (generation == content_generation_ &&
                        state_ != glance::contracts::PreviewWindowState::closed &&
                        thumbnail != nullptr && thumbnail.Size() > 0)
                    {
                        Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
                        co_await bitmap.SetSourceAsync(thumbnail);
                        if (generation == content_generation_)
                        {
                            MediaCoverImage().Source(bitmap);
                            MediaCoverImage().Visibility(Visibility::Visible);
                            MediaCoverPlaceholder().Visibility(Visibility::Collapsed);
                        }
                    }
                }
                catch (const hresult_error&)
                {
                    MediaTitleText().Text(std::filesystem::path(path).stem().wstring());
                }
            }
            else
            {
                try
                {
                    const auto properties = co_await file.Properties().GetVideoPropertiesAsync();
                    if (generation != content_generation_ ||
                        current_index_ >= files_.size() ||
                        state_ == glance::contracts::PreviewWindowState::closed)
                    {
                        co_return;
                    }
                    if (properties.Width() > 0 && properties.Height() > 0)
                    {
                        auto_fit_window_to_content(properties.Width(), properties.Height());
                    }
                }
                catch (const hresult_error&)
                {
                }
                reveal_deferred_preview();
            }
        }
        catch (const hresult_error& error)
        {
            if (state_ != glance::contracts::PreviewWindowState::closed)
            {
                const auto message = glance::app::localize_format(L"MediaOpenError", { error.message() });
                lifetime->show_provider_error(message, generation);
            }
        }
    }

    void MainWindow::update_media_footer()
    {
        update_footer_metadata();
    }

    void MainWindow::update_media_playback_metadata(
        const Windows::Media::Playback::MediaPlaybackItem& item,
        std::uint64_t generation)
    {
        if (generation != content_generation_ ||
            current_kind_ != glance::app::PreviewKind::media ||
            !glance::app::footer_field_enabled(
                footer_preferences_, glance::app::FooterField::media_info) ||
            media_playback_item_ == nullptr ||
            media_playback_item_ != item)
        {
            return;
        }

        try
        {
            std::vector<std::wstring> fields;
            const auto append = [&fields](std::wstring value) {
                if (!value.empty())
                {
                    fields.push_back(std::move(value));
                }
            };

            if (media_is_audio_)
            {
                const auto tracks = item.AudioTracks();
                if (tracks.Size() > 0)
                {
                    const auto properties = tracks.GetAt(0).GetEncodingProperties();
                    if (properties.SampleRate() > 0)
                    {
                        std::wostringstream sample_rate;
                        sample_rate << std::fixed << std::setprecision(
                            properties.SampleRate() % 1000 == 0 ? 0 : 1)
                                    << properties.SampleRate() / 1000.0 << L"kHz";
                        append(sample_rate.str());
                    }
                    if (properties.BitsPerSample() > 0)
                    {
                        append(std::to_wstring(properties.BitsPerSample()) + L"-bit");
                    }
                    if (properties.ChannelCount() > 0)
                    {
                        append(glance::app::localize_format(
                            L"MediaFooterChannelsFormat",
                            { std::to_wstring(properties.ChannelCount()) }));
                    }
                    append(format_media_subtype(properties.Subtype()));
                    append(format_media_bitrate(properties.Bitrate()));
                }
            }
            else
            {
                const auto tracks = item.VideoTracks();
                if (tracks.Size() > 0)
                {
                    const auto properties = tracks.GetAt(0).GetEncodingProperties();
                    if (properties.Width() > 0 && properties.Height() > 0)
                    {
                        media_dimensions_ = std::to_wstring(properties.Width())
                            + L"x" + std::to_wstring(properties.Height());
                    }
                    append(format_media_frame_rate(properties.FrameRate()));
                    append(format_media_subtype(properties.Subtype()));
                    append(format_media_bitrate(properties.Bitrate()));
                }
            }

            media_playback_info_.clear();
            for (const auto& field : fields)
            {
                media_playback_info_ +=
                    media_playback_info_.empty() ? field : L" " + field;
            }
            update_media_footer();
        }
        catch (const hresult_error&)
        {
            media_playback_info_.clear();
            update_media_footer();
        }
    }

    void MainWindow::update_footer_metadata()
    {
        if (current_index_ >= files_.size())
        {
            FooterMetadataText().Text(L"");
            return;
        }

        const auto& file = files_[current_index_];
        std::vector<std::wstring> fields;
        const auto append = [&fields](std::wstring value) {
            if (!value.empty())
            {
                fields.push_back(std::move(value));
            }
        };
        for (const auto field : footer_preferences_.order)
        {
            if (!glance::app::footer_field_enabled(footer_preferences_, field))
            {
                continue;
            }
            switch (field)
            {
            case glance::app::FooterField::size:
                append(formatted_size(file.size));
                break;
            case glance::app::FooterField::modified_time:
                if (file.last_write_time != 0)
                {
                    append(glance::app::localize_format(
                        L"FooterModifiedTimeFormat",
                        { formatted_time(file.last_write_time) }));
                }
                break;
            case glance::app::FooterField::creation_time:
                if (file.creation_time != 0)
                {
                    append(glance::app::localize_format(
                        L"FooterCreationTimeFormat",
                        { formatted_time(file.creation_time) }));
                }
                break;
            case glance::app::FooterField::taken_time:
                if (current_kind_ == glance::app::PreviewKind::image &&
                    !image_taken_time_.empty())
                {
                    append(glance::app::localize_format(
                        L"FooterTakenTimeFormat",
                        { image_taken_time_ }));
                }
                break;
            case glance::app::FooterField::permissions:
                append(footer_access_loaded_ ? footer_access_mode_ : L"--");
                break;
            case glance::app::FooterField::media_info:
            {
                std::wstring media_info;
                const auto append_media_info = [&media_info](std::wstring_view value) {
                    if (!value.empty())
                    {
                        if (!media_info.empty())
                        {
                            media_info.push_back(L' ');
                        }
                        media_info.append(value);
                    }
                };
                if (current_kind_ == glance::app::PreviewKind::image &&
                    image_pixel_width_ > 0 && image_pixel_height_ > 0)
                {
                    append_media_info(
                        std::to_wstring(image_pixel_width_) + L"x" +
                        std::to_wstring(image_pixel_height_));
                    if (image_bits_per_pixel_ > 0)
                    {
                        append_media_info(std::to_wstring(image_bits_per_pixel_) + L"bpp");
                    }
                }
                else if (current_kind_ == glance::app::PreviewKind::media)
                {
                    append_media_info(media_dimensions_);
                    append_media_info(media_playback_info_);
                }
                append(std::move(media_info));
                break;
            }
            }
        }

        std::wstring metadata;
        for (const auto& field : fields)
        {
            metadata += metadata.empty() ? field : L"  |  " + field;
        }
        FooterMetadataText().Text(metadata);
    }

    void MainWindow::update_generic_file_metadata()
    {
        if (current_kind_ != glance::app::PreviewKind::generic || current_index_ >= files_.size())
        {
            return;
        }
        const auto& file = files_[current_index_];
        std::wstring metadata = formatted_size(file.size);
        if (file.last_write_time != 0)
        {
            metadata += L"  |  " + glance::app::localize_format(
                L"GenericModifiedAt",
                { formatted_time(file.last_write_time) });
        }
        if (file.creation_time != 0)
        {
            metadata += L"  |  " + glance::app::localize_format(
                L"GenericCreatedAt",
                { formatted_time(file.creation_time) });
        }
        FileMetadataText().Text(metadata);
    }

    void MainWindow::request_footer_access_if_needed()
    {
        if (!glance::app::footer_field_enabled(
                footer_preferences_, glance::app::FooterField::permissions) ||
            footer_access_loaded_ || footer_access_requested_ ||
            current_index_ >= files_.size())
        {
            return;
        }

        const auto& file = files_[current_index_];
        if (!file.is_filesystem || file.path.empty() || file.is_cloud_placeholder)
        {
            footer_access_mode_ = L"--";
            footer_access_loaded_ = true;
            update_footer_metadata();
            return;
        }
        footer_access_requested_ = true;
        load_footer_access_async(file.path, content_generation_);
    }

    fire_and_forget MainWindow::load_pdf_async(
        std::wstring path,
        std::uint64_t generation,
        std::wstring password)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        cancel_pdf_render();
        auto session = pdf_render_client_;
        if (session == nullptr)
        {
            session = glance::app::acquire_paged_document_render_client();
            pdf_render_client_ = session;
        }
        if (session == nullptr)
        {
            show_provider_error(
                glance::app::localize(L"PdfComponentMissingError"),
                generation);
            co_return;
        }
        pdf_source_path_ = path;
        pdf_password_ = password;
        PdfLoadingText().Visibility(Visibility::Visible);
        co_await resume_background();
        auto result = session->open(path, password);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            session,
            result = std::move(result),
            path = std::move(path),
            password = std::move(password),
            generation]() mutable {
            lifetime->apply_pdf_open_result(
                std::move(session),
                std::move(result),
                std::move(path),
                std::move(password),
                generation);
        }));
    }

    void MainWindow::apply_pdf_open_result(
        std::shared_ptr<glance::app::PagedDocumentRenderClient> session,
        glance::app::PagedDocumentOpenResult result,
        std::wstring path,
        std::wstring password,
        std::uint64_t generation)
    {
        using glance::contracts::document::Status;
        glance::contracts::log_event(
            L"PDF open completed: status=" +
            std::to_wstring(static_cast<std::uint32_t>(result.status)) +
            L", pages=" + std::to_wstring(result.page_count));
        if (generation != content_generation_ || session != pdf_render_client_)
        {
            session->close_document();
            return;
        }
        if (result.status == Status::password_required || result.status == Status::invalid_password)
        {
            PdfLoadingOverlay().Visibility(Visibility::Collapsed);
            pdf_source_path_ = std::move(path);
            pdf_password_ = std::move(password);
            reveal_deferred_preview();
            show_password_prompt(
                PasswordPromptTarget::pdf,
                result.status == Status::invalid_password);
            return;
        }
        if (result.status != Status::success)
        {
            show_provider_error(
                result.status == Status::dependency_missing
                    ? glance::app::localize(L"PdfComponentMissingError")
                    : glance::app::localize(L"PdfOpenError"),
                generation);
            return;
        }
        if (result.page_count == 0)
        {
            show_provider_error(glance::app::localize(L"PdfEmptyError"), generation);
            return;
        }
        hide_password_prompt();
        pdf_page_count_ = result.page_count;
        pdf_outline_ = std::move(result.outline);
        pdf_page_index_ = 0;
        render_pdf_page_async(pdf_page_index_, generation);
        build_pdf_navigation(generation);
    }

    fire_and_forget MainWindow::render_pdf_page_async(
        std::uint32_t page_index,
        std::uint64_t generation,
        bool dynamic_update)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto session = pdf_render_client_;
        const auto request = pdf_render_request_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (session == nullptr || page_index >= pdf_page_count_)
        {
            co_return;
        }
        if (PdfPageImage().Source() != nullptr)
        {
            PdfLoadingText().Visibility(Visibility::Collapsed);
        }
        PdfLoadingOverlay().Visibility(Visibility::Visible);
        const auto render_dimension =
            glance::app::normalize_rich_document_render_dimension(
                static_cast<std::uint32_t>(glance::app::component_setting_value(
                    L"pdf",
                    L"render-dimension",
                    glance::app::default_rich_document_render_dimension)));
        AtomicCounterGuard foreground_render(pdf_foreground_render_requests_);
        co_await resume_background();
        if (request != pdf_render_request_.load(std::memory_order_relaxed))
        {
            co_return;
        }
        auto rendered = session->render(
            page_index,
            render_dimension,
            render_dimension);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            session,
            rendered = std::move(rendered),
            page_index,
            request,
            generation,
            dynamic_update]() mutable {
            using glance::contracts::document::Status;
            if (generation != lifetime->content_generation_ ||
                session != lifetime->pdf_render_client_ ||
                page_index != lifetime->pdf_page_index_ ||
                request != lifetime->pdf_render_request_.load(std::memory_order_relaxed))
            {
                return;
            }
            if (rendered.status != Status::success)
            {
                lifetime->show_provider_error(
                    glance::app::localize(L"PdfRenderError"),
                    generation);
                return;
            }
            try
            {
                lifetime->PdfPageImage().Source(create_pdf_bitmap(rendered));
                lifetime->PdfLoadingOverlay().Visibility(Visibility::Collapsed);
                lifetime->PdfPageText().Text(
                    std::to_wstring(page_index + 1) + L" / " +
                    std::to_wstring(lifetime->pdf_page_count_));
                lifetime->sync_pdf_thumbnail_selection();
                lifetime->auto_fit_window_to_content(
                    rendered.page_width_points,
                    rendered.page_height_points,
                    dynamic_update);
                if (!dynamic_update)
                {
                    const auto weak = lifetime->get_weak();
                    static_cast<void>(lifetime->DispatcherQueue().TryEnqueue(
                        Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                        [weak, generation] {
                            const auto self = weak.get();
                            if (self == nullptr ||
                                generation != self->content_generation_ ||
                                self->current_kind_ != glance::app::PreviewKind::document)
                            {
                                return;
                            }
                            const auto scroller = self->PdfScroller();
                            scroller.CancelDirectManipulations();
                            scroller.ReleasePointerCaptures();
                            self->pdf_panning_ = false;
                            scroller.UpdateLayout();
                            const bool accepted =
                                scroller.ChangeView(0.0, 0.0, 1.0F, true);
                            glance::contracts::log_event(
                                L"PDF view reset after layout: accepted=" +
                                std::to_wstring(accepted) +
                                L", zoom=" +
                                std::to_wstring(scroller.ZoomFactor()));
                        }));
                }
            }
            catch (const hresult_error& error)
            {
                lifetime->show_provider_error(
                    glance::app::localize_format(L"PdfRenderErrorDetail", { error.message() }),
                    generation);
            }
        }));
    }

    fire_and_forget MainWindow::load_pdf_thumbnails_async(std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto session = pdf_render_client_;
        const auto page_count = pdf_page_count_;
        co_await resume_background();
        for (std::uint32_t page = 0; page < page_count; ++page)
        {
            while (pdf_foreground_render_requests_.load(std::memory_order_acquire) != 0)
            {
                co_await resume_after(std::chrono::milliseconds(4));
            }
            auto rendered = session->render(page, 176, 132);
            if (rendered.status != glance::contracts::document::Status::success)
            {
                co_return;
            }
            if (!dispatcher.TryEnqueue([
                    lifetime,
                    session,
                    rendered = std::move(rendered),
                    page,
                    generation]() mutable {
                    if (generation != lifetime->content_generation_ ||
                        session != lifetime->pdf_render_client_ ||
                        page >= lifetime->pdf_thumbnail_images_.size())
                    {
                        return;
                    }
                    try
                    {
                        if (auto image = lifetime->pdf_thumbnail_images_[page].get())
                        {
                            image.Source(create_pdf_bitmap(rendered));
                        }
                    }
                    catch (...)
                    {
                    }
                }))
            {
                co_return;
            }
            co_await resume_after(std::chrono::milliseconds(8));
        }
    }

    fire_and_forget MainWindow::load_directory_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_directory_preview(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), generation]() mutable {
                lifetime->apply_archive_preview(std::move(preview), generation);
            }));
    }

    void MainWindow::apply_archive_preview(
        glance::app::ArchivePreview preview,
        std::uint64_t generation)
    {
        if (generation != content_generation_)
        {
            return;
        }
        cancel_archive_icon_load();
        if (!preview.error.empty())
        {
            show_provider_error(std::move(preview.error), generation);
            return;
        }

        archive_entry_compressed_size_available_ =
            !archive_preview_is_directory_ && preview.entry_compressed_size_available;
        update_archive_header_state();
        auto root_nodes = ArchiveEntryTree().RootNodes();
        root_nodes.Clear();
        FolderEntryList().Items().Clear();
        if (archive_preview_is_directory_)
        {
            sort_folder_entries(preview.entries, folder_preview_preferences_);
        }
        auto state = std::make_shared<ArchiveRenderState>();
        state->preview = std::move(preview);
        state->generation = generation;
        state->icon_targets.reserve(state->preview.entry_count);
        state->icon_controls.reserve(state->preview.entry_count);
        for (const auto& entry : state->preview.entries)
        {
            state->pending.push_back(PendingArchiveNode{ &entry, nullptr });
        }
        if (!archive_preview_is_directory_ &&
            !active_file_directory_descriptor_.info_fields.empty())
        {
            const auto format_value = [this](
                                          const glance::app::FileDirectoryValue& value) {
                using glance::contracts::components::FileDirectoryValueKind;
                switch (value.kind)
                {
                case FileDirectoryValueKind::text:
                    return value.text;
                case FileDirectoryValueKind::unsigned_integer:
                    return std::to_wstring(value.unsigned_value);
                case FileDirectoryValueKind::bytes:
                    return formatted_size(value.unsigned_value);
                case FileDirectoryValueKind::timestamp:
                    return formatted_time(value.unsigned_value);
                case FileDirectoryValueKind::ratio:
                {
                    std::wostringstream text;
                    text << std::fixed << std::setprecision(1)
                         << value.ratio_value * 100.0 << L'%';
                    return text.str();
                }
                default:
                    return std::wstring(L"--");
                }
            };
            for (const auto& field : active_file_directory_descriptor_.info_fields)
            {
                if (!state->status.empty())
                {
                    state->status += L"  |  ";
                }
                state->status += field.label + L" " + format_value(field.value);
            }
        }
        else
        {
            state->status = glance::app::localize_format(
                L"FileCount",
                { std::to_wstring(state->preview.file_count) });
        }
        if (archive_preview_is_directory_)
        {
            state->status += L"  |  " + glance::app::localize_format(
                state->preview.truncated ? L"PartialTotalSize" : L"TotalSize",
                { state->preview.original_size_known
                    ? formatted_size(state->preview.original_size)
                    : std::wstring(L"--") });
        }
        else if (active_file_directory_descriptor_.info_fields.empty())
        {
            const auto compressed_size = state->preview.compressed_size_known
                ? formatted_size(state->preview.compressed_size)
                : std::wstring(L"--");
            const auto original_size = state->preview.original_size_known
                ? formatted_size(state->preview.original_size)
                : std::wstring(L"--");
            std::wstring compression_ratio{ L"--" };
            if (state->preview.compressed_size_known &&
                state->preview.original_size_known &&
                state->preview.original_size != 0)
            {
                std::wostringstream value;
                value << std::fixed << std::setprecision(1)
                      << static_cast<double>(state->preview.compressed_size) * 100.0 /
                             static_cast<double>(state->preview.original_size)
                      << L'%';
                compression_ratio = value.str();
            }
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveCompressedSize",
                { compressed_size });
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveOriginalSize",
                { original_size });
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveCompressionRatio",
                { compression_ratio });
        }
        if (state->preview.truncated)
        {
            state->status += L"  |  " + glance::app::localize(L"ListTruncated");
        }
        if (state->preview.depth_limited)
        {
            state->status += L"  |  " + glance::app::localize(L"ArchiveDepthLimited");
        }
        archive_render_state_ = state;
        render_archive_batch(state);
    }

    void MainWindow::render_archive_batch(const std::shared_ptr<ArchiveRenderState>& state)
    {
        if (state->generation != content_generation_ ||
            archive_render_state_ != state ||
            current_kind_ != glance::app::PreviewKind::archive)
        {
            return;
        }

        constexpr std::size_t batch_size = 64;
        auto root_nodes = ArchiveEntryTree().RootNodes();
        auto folder_items = FolderEntryList().Items();
        for (std::size_t index = 0; index < batch_size && !state->pending.empty(); ++index)
        {
            const auto pending = std::move(state->pending.front());
            state->pending.pop_front();
            const auto& entry = *pending.entry;

            Grid row;
            std::vector<ArchiveColumnSpec> component_columns;
            std::span<const ArchiveColumnSpec> columns;
            if (!archive_preview_is_directory_ &&
                !active_file_directory_columns_.empty())
            {
                component_columns.reserve(active_file_directory_columns_.size());
                for (std::size_t column = 0;
                     column < active_file_directory_columns_.size();
                     ++column)
                {
                    const auto descriptor_index =
                        active_file_directory_columns_[column];
                    const auto& descriptor =
                        active_file_directory_descriptor_.columns[descriptor_index];
                    component_columns.push_back(ArchiveColumnSpec{
                        .kind = ArchiveColumnKind::name,
                        .width = column == 0
                            ? 0.0
                            : static_cast<double>(
                                descriptor.width == 0 ? 110 : descriptor.width) });
                }
                columns = component_columns;
            }
            else
            {
                columns = archive_columns(
                    archive_preview_is_directory_,
                    archive_entry_compressed_size_available_);
            }
            configure_archive_columns(row, columns);

            Grid name_cell;
            name_cell.Margin(Thickness{ 8, 0, 8, 0 });
            ColumnDefinition icon_column;
            icon_column.Width(GridLength{ 28, GridUnitType::Pixel });
            name_cell.ColumnDefinitions().Append(icon_column);
            ColumnDefinition name_text_column;
            name_text_column.Width(GridLength{ 1, GridUnitType::Star });
            name_cell.ColumnDefinitions().Append(name_text_column);

            Grid icon_host;
            icon_host.Width(20);
            icon_host.Height(20);
            icon_host.HorizontalAlignment(HorizontalAlignment::Left);
            icon_host.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(icon_host, 0);

            const std::uint32_t icon_pixel_size =
                archive_preview_is_directory_ ? folder_icon_pixel_size : 16;
            Image icon_image;
            icon_image.Width(icon_pixel_size);
            icon_image.Height(icon_pixel_size);
            icon_image.Stretch(Media::Stretch::Uniform);
            icon_image.Visibility(Visibility::Collapsed);
            icon_host.Children().Append(icon_image);

            FontIcon fallback_icon;
            fallback_icon.FontSize(13);
            fallback_icon.Glyph(entry.is_folder ? L"\xE8B7" : L"\xE8A5");
            icon_host.Children().Append(fallback_icon);
            name_cell.Children().Append(icon_host);

            TextBlock name_text;
            name_text.Text(entry.name);
            name_text.FontSize(12);
            name_text.VerticalAlignment(VerticalAlignment::Center);
            name_text.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(name_text, 1);
            name_cell.Children().Append(name_text);
            Grid::SetColumn(name_cell, 0);
            row.Children().Append(name_cell);

            const auto icon_path = entry.path.empty() ? entry.name : entry.path;
            auto extension = entry.is_folder
                ? std::wstring(L":folder")
                : std::filesystem::path(icon_path).extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            if (extension.empty())
            {
                extension = L":file";
            }
            const auto control_index = state->icon_controls.size();
            state->icon_controls.push_back(ArchiveIconControl{
                icon_image,
                fallback_icon });
            state->icon_targets.push_back(ArchiveIconTarget{
                icon_path,
                std::move(extension),
                control_index,
                icon_pixel_size,
                entry.is_folder,
                archive_preview_is_directory_ &&
                    !entry.is_folder &&
                    (entry.attributes &
                        (FILE_ATTRIBUTE_OFFLINE |
                         FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                         FILE_ATTRIBUTE_RECALL_ON_OPEN)) == 0 });

            const auto append_text = [&row](std::wstring_view value, int column, TextAlignment alignment = TextAlignment::Left) {
                TextBlock text;
                text.Text(value);
                text.FontSize(12);
                text.VerticalAlignment(VerticalAlignment::Center);
                text.TextAlignment(alignment);
                text.TextTrimming(TextTrimming::CharacterEllipsis);
                text.Margin(Thickness{ 8, 0, 8, 0 });
                Grid::SetColumn(text, column);
                row.Children().Append(text);
            };
            for (std::size_t column = 1; column < columns.size(); ++column)
            {
                if (!active_file_directory_columns_.empty())
                {
                    const auto descriptor_index =
                        active_file_directory_columns_[column];
                    if (descriptor_index >= entry.values.size())
                    {
                        continue;
                    }
                    const auto& descriptor =
                        active_file_directory_descriptor_.columns[descriptor_index];
                    const auto& value = entry.values[descriptor_index];
                    std::wstring text;
                    const auto kind = static_cast<
                        glance::contracts::components::FileDirectoryValueKind>(value.kind);
                    switch (kind)
                    {
                    case glance::contracts::components::FileDirectoryValueKind::text:
                        text = value.text;
                        break;
                    case glance::contracts::components::FileDirectoryValueKind::bytes:
                        if (!entry.is_folder)
                        {
                            text = formatted_size(value.unsigned_value);
                        }
                        break;
                    case glance::contracts::components::FileDirectoryValueKind::timestamp:
                        if (value.unsigned_value != 0)
                        {
                            text = formatted_time(value.unsigned_value);
                        }
                        break;
                    case glance::contracts::components::FileDirectoryValueKind::unsigned_integer:
                        text = std::to_wstring(value.unsigned_value);
                        break;
                    case glance::contracts::components::FileDirectoryValueKind::ratio:
                    {
                        std::wostringstream output;
                        output << std::fixed << std::setprecision(1)
                               << value.ratio_value * 100.0 << L'%';
                        text = output.str();
                        break;
                    }
                    case glance::contracts::components::FileDirectoryValueKind::none:
                    default:
                        break;
                    }
                    append_text(
                        text,
                        static_cast<int>(column),
                        descriptor.alignment ==
                                glance::contracts::components::FileDirectoryAlignment::right
                            ? TextAlignment::Right
                            : TextAlignment::Left);
                    continue;
                }
                switch (columns[column].kind)
                {
                case ArchiveColumnKind::type:
                    append_text(entry.type_name, static_cast<int>(column));
                    break;
                case ArchiveColumnKind::modified_time:
                    append_text(
                        entry.modified_time == 0 ? L"" : formatted_time(entry.modified_time),
                        static_cast<int>(column));
                    break;
                case ArchiveColumnKind::compressed_size:
                    append_text(
                        entry.is_folder
                            ? L""
                            : entry.compressed_size_known
                                ? formatted_size(entry.compressed_size)
                                : L"--",
                        static_cast<int>(column),
                        TextAlignment::Right);
                    break;
                case ArchiveColumnKind::original_size:
                    append_text(
                        entry.is_folder
                            ? L""
                            : entry.original_size_known
                                ? formatted_size(entry.original_size)
                                : L"--",
                        static_cast<int>(column),
                        TextAlignment::Right);
                    break;
                case ArchiveColumnKind::name:
                default:
                    break;
                }
            }

            if (archive_preview_is_directory_)
            {
                ListViewItem item;
                item.Content(row);
                folder_items.Append(item);
                if (!pending_folder_selection_path_.empty() &&
                    CompareStringOrdinal(
                        entry.path.c_str(),
                        -1,
                        pending_folder_selection_path_.c_str(),
                        -1,
                        TRUE) == CSTR_EQUAL)
                {
                    FolderEntryList().SelectedIndex(
                        static_cast<int>(folder_items.Size() - 1));
                    if (!pending_folder_focus_restore_)
                    {
                        FolderEntryList().ScrollIntoView(item);
                    }
                    pending_folder_selection_path_.clear();
                }
            }
            else
            {
                TreeViewNode node;
                node.Content(row);
                if (pending.parent != nullptr)
                {
                    pending.parent.Children().Append(node);
                }
                else
                {
                    root_nodes.Append(node);
                }
                for (const auto& child : entry.children)
                {
                    state->pending.push_back(PendingArchiveNode{ &child, node });
                }
            }
        }

        if (!state->pending.empty())
        {
            const auto lifetime = get_strong();
            static_cast<void>(DispatcherQueue().TryEnqueue([lifetime, state] {
                lifetime->render_archive_batch(state);
            }));
            return;
        }

        ArchiveStatusText().Text(state->status);
        if (archive_preview_is_directory_ && pending_folder_focus_restore_)
        {
            FolderEntryList().UpdateLayout();
            if (pending_folder_scroll_offset_valid_)
            {
                if (const auto scroller = find_scroll_viewer(FolderEntryList()))
                {
                    static_cast<void>(scroller.ChangeView(
                        nullptr,
                        pending_folder_scroll_offset_,
                        nullptr,
                        true));
                }
            }
            pending_folder_scroll_offset_valid_ = false;
            pending_folder_focus_restore_ = false;

            const auto lifetime = get_strong();
            const auto focus_generation = state->generation;
            static_cast<void>(DispatcherQueue().TryEnqueue([lifetime, focus_generation] {
                if (focus_generation != lifetime->content_generation_ ||
                    !lifetime->archive_preview_is_directory_)
                {
                    return;
                }
                const int selected_index = lifetime->FolderEntryList().SelectedIndex();
                const auto target = selected_index >= 0
                    ? lifetime->FolderEntryList()
                        .ContainerFromIndex(selected_index)
                        .try_as<Control>()
                    : nullptr;
                if (target != nullptr)
                {
                    target.Focus(FocusState::Programmatic);
                }
                else
                {
                    lifetime->FolderEntryList().Focus(FocusState::Programmatic);
                }
            }));
        }
        if (!state->icon_targets.empty())
        {
            auto cancellation = std::make_shared<std::atomic_bool>(false);
            archive_icon_cancellation_ = cancellation;
            load_archive_icons_async(
                std::move(state->icon_targets),
                state->generation,
                std::move(cancellation));
        }
    }

    void MainWindow::cancel_archive_icon_load() noexcept
    {
        if (archive_icon_cancellation_ != nullptr)
        {
            archive_icon_cancellation_->store(true, std::memory_order_release);
            archive_icon_cancellation_.reset();
        }
    }

    fire_and_forget MainWindow::load_archive_icons_async(
        std::vector<ArchiveIconTarget> targets,
        std::uint64_t generation,
        std::shared_ptr<std::atomic_bool> cancellation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();

        std::vector<ArchiveIconTarget> thumbnail_targets;
        std::unordered_map<std::wstring, std::vector<ArchiveIconTarget>> groups;
        for (auto& target : targets)
        {
            if (target.thumbnail_candidate)
            {
                thumbnail_targets.push_back(target);
            }
            groups[target.cache_key].push_back(std::move(target));
        }
        for (auto& [cache_key, group] : groups)
        {
            if (cancellation->load(std::memory_order_acquire))
            {
                co_return;
            }
            static_cast<void>(cache_key);
            const auto& representative = group.front();
            auto bitmap = glance::app::load_shell_icon(
                representative.path,
                representative.is_folder,
                representative.pixel_size,
                true);
            if (bitmap == nullptr)
            {
                continue;
            }

            auto targets_for_ui = group;
            static_cast<void>(dispatcher.TryEnqueue(
                [lifetime,
                 cancellation,
                 bitmap = std::move(bitmap),
                 group = std::move(targets_for_ui),
                 generation]() mutable {
                    if (cancellation->load(std::memory_order_acquire) ||
                        generation != lifetime->content_generation_ ||
                        lifetime->current_kind_ != glance::app::PreviewKind::archive)
                    {
                        return;
                    }
                    try
                    {
                        const auto state = lifetime->archive_render_state_;
                        if (state == nullptr || state->generation != generation)
                        {
                            return;
                        }
                        const auto source = glance::app::create_shell_icon_source(*bitmap);
                        if (source == nullptr)
                        {
                            return;
                        }
                        for (auto& target : group)
                        {
                            if (target.control_index >= state->icon_controls.size())
                            {
                                continue;
                            }
                            const auto& controls =
                                state->icon_controls[target.control_index];
                            controls.image.Source(source);
                            controls.image.Visibility(Visibility::Visible);
                            controls.fallback.Visibility(Visibility::Collapsed);
                        }
                    }
                    catch (...)
                    {
                    }
                }));
        }

        struct ThumbnailUpdate
        {
            ArchiveIconTarget target;
            glance::app::ShellIconBitmapPtr bitmap;
        };
        std::vector<ThumbnailUpdate> updates;
        updates.reserve(thumbnail_update_batch_size);
        const auto dispatch_updates = [&]() {
            if (updates.empty())
            {
                return;
            }
            auto pending = std::move(updates);
            updates.clear();
            updates.reserve(thumbnail_update_batch_size);
            static_cast<void>(dispatcher.TryEnqueue(
                [lifetime,
                 cancellation,
                 pending = std::move(pending),
                 generation]() mutable {
                    if (generation != lifetime->content_generation_ ||
                        cancellation->load(std::memory_order_acquire) ||
                        lifetime->current_kind_ != glance::app::PreviewKind::archive ||
                        !lifetime->archive_preview_is_directory_)
                    {
                        return;
                    }
                    const auto state = lifetime->archive_render_state_;
                    if (state == nullptr || state->generation != generation)
                    {
                        return;
                    }
                    for (auto& update : pending)
                    {
                        try
                        {
                            if (update.target.control_index >=
                                state->icon_controls.size())
                            {
                                continue;
                            }
                            const auto source =
                                glance::app::create_shell_icon_source(*update.bitmap);
                            if (source == nullptr)
                            {
                                continue;
                            }
                            const auto& controls =
                                state->icon_controls[update.target.control_index];
                            controls.image.Source(source);
                            controls.image.Visibility(Visibility::Visible);
                            controls.fallback.Visibility(Visibility::Collapsed);
                        }
                        catch (...)
                        {
                        }
                    }
                }));
        };

        for (auto& target : thumbnail_targets)
        {
            if (cancellation->load(std::memory_order_acquire))
            {
                co_return;
            }
            auto bitmap = glance::app::load_shell_thumbnail(
                target.path,
                folder_thumbnail_pixel_size);
            if (cancellation->load(std::memory_order_acquire))
            {
                co_return;
            }
            if (bitmap == nullptr)
            {
                continue;
            }
            updates.push_back(ThumbnailUpdate{
                std::move(target),
                std::move(bitmap) });
            if (updates.size() >= thumbnail_update_batch_size)
            {
                dispatch_updates();
            }
        }
        dispatch_updates();
    }

    fire_and_forget MainWindow::load_component_async(
        std::wstring path,
        std::uint64_t generation)
    {
        const auto weak = get_weak();
        const auto dispatcher = DispatcherQueue();
        const auto language = glance::app::current_ui_language();
        const auto color_scheme =
            RootGrid().ActualTheme() == ElementTheme::Dark
            ? glance::contracts::components::PreviewColorScheme::dark
            : glance::contracts::components::PreviewColorScheme::light;
        glance::contracts::components::PreviewPreparationOptions options{
            .maximum_dimension = glance::app::normalize_rich_document_render_dimension(
                static_cast<std::uint32_t>(glance::app::component_setting_value(
                    L"pdf",
                    L"render-dimension",
                    glance::app::default_rich_document_render_dimension))) };
        co_await resume_background();

        auto result = glance::app::prepare_component_preview(
            path,
            language,
            options,
            color_scheme,
            [weak, dispatcher, generation, language](std::wstring text) mutable {
                static_cast<void>(dispatcher.TryEnqueue([
                    weak,
                    text = std::move(text),
                    generation,
                    language]() mutable {
                    const auto lifetime = weak.get();
                    if (lifetime == nullptr ||
                        generation != lifetime->content_generation_ ||
                        lifetime->current_kind_ != glance::app::PreviewKind::component ||
                        language != glance::app::current_ui_language())
                    {
                        return;
                    }
                    lifetime->component_loading_language_ = language;
                    lifetime->ComponentLoadingText().Text(
                        text.empty()
                            ? glance::app::localize(L"Loading")
                            : std::move(text));
                    lifetime->ComponentLoadingText().Visibility(Visibility::Visible);
                }));
            });

        static_cast<void>(dispatcher.TryEnqueue([
            weak,
            result = std::move(result),
            generation]() mutable {
            const auto lifetime = weak.get();
            if (lifetime == nullptr ||
                generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::component)
            {
                return;
            }
            if (result.status !=
                    glance::contracts::components::PrepareStatus::success ||
                (result.output_path.empty() && result.file_directory == nullptr))
            {
                if (result.status !=
                    glance::contracts::components::PrepareStatus::cancelled)
                {
                    lifetime->show_provider_error(
                        result.error_detail.empty()
                            ? glance::app::localize(L"ComponentStateError")
                            : std::move(result.error_detail),
                        generation);
                }
                else
                {
                    lifetime->ComponentLoadingText().Visibility(Visibility::Collapsed);
                }
                return;
            }

            lifetime->apply_component_preview(std::move(result), generation);
        }));
    }

    fire_and_forget MainWindow::load_component_file_directory_async(
        std::shared_ptr<void> session,
        std::wstring password,
        std::uint64_t generation)
    {
        const auto weak = get_weak();
        const auto dispatcher = DispatcherQueue();
        const auto language = glance::app::current_ui_language();
        co_await resume_background();

        glance::app::FileDirectoryDescriptor descriptor;
        const auto status = glance::app::open_component_file_directory(
            session,
            language,
            password,
            descriptor);
        glance::app::ArchivePreview preview;
        if (status == glance::contracts::components::FileDirectoryOpenStatus::ready)
        {
            std::unordered_set<std::uint64_t> visited;
            if (!append_component_directory_tree(
                    session,
                    descriptor,
                    0,
                    1,
                    preview.entries,
                    preview,
                    visited))
            {
                preview.error = glance::app::localize(L"ArchiveReadError");
            }
            apply_component_directory_summary(descriptor, preview);
        }

        static_cast<void>(dispatcher.TryEnqueue([
            weak,
            session = std::move(session),
            descriptor = std::move(descriptor),
            preview = std::move(preview),
            status,
            generation]() mutable {
            const auto lifetime = weak.get();
            if (lifetime == nullptr || generation != lifetime->content_generation_ ||
                session != lifetime->active_component_file_directory_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::archive)
            {
                return;
            }
            if (status ==
                    glance::contracts::components::FileDirectoryOpenStatus::password_required ||
                status ==
                    glance::contracts::components::FileDirectoryOpenStatus::invalid_password)
            {
                lifetime->show_password_prompt(
                    PasswordPromptTarget::archive,
                    status == glance::contracts::components::
                        FileDirectoryOpenStatus::invalid_password);
                return;
            }
            if (status != glance::contracts::components::FileDirectoryOpenStatus::ready)
            {
                if (status !=
                    glance::contracts::components::FileDirectoryOpenStatus::cancelled)
                {
                    lifetime->show_provider_error(
                        glance::app::localize(L"ArchiveReadError"),
                        generation);
                }
                return;
            }
            lifetime->active_file_directory_descriptor_ = std::move(descriptor);
            lifetime->archive_entry_compressed_size_available_ =
                preview.entry_compressed_size_available;
            lifetime->update_archive_header_state();
            lifetime->apply_archive_preview(std::move(preview), generation);
        }));
    }

    fire_and_forget MainWindow::refresh_component_loading_text_async(
        std::wstring path,
        std::uint64_t generation)
    {
        const auto weak = get_weak();
        const auto dispatcher = DispatcherQueue();
        const auto language = glance::app::current_ui_language();
        co_await resume_background();

        auto message = glance::app::component_loading_text(path, language);
        static_cast<void>(dispatcher.TryEnqueue([
            weak,
            message = std::move(message),
            generation,
            language]() mutable {
            const auto lifetime = weak.get();
            if (lifetime == nullptr ||
                generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::component ||
                language != glance::app::current_ui_language())
            {
                return;
            }

            lifetime->component_loading_language_ = language;
            if (!message.component_found)
            {
                lifetime->ComponentLoadingText().Visibility(Visibility::Collapsed);
                return;
            }
            lifetime->ComponentLoadingText().Text(
                message.text.empty()
                    ? glance::app::localize(L"Loading")
                    : std::move(message.text));
            lifetime->ComponentLoadingText().Visibility(Visibility::Visible);
        }));
    }

    void MainWindow::apply_component_preview(
        glance::app::ComponentPreviewResult result,
        std::uint64_t generation)
    {
        using glance::contracts::components::PreviewContentFormat;
        using glance::contracts::components::PreviewContentKind;

        glance::app::PreviewKind kind = glance::app::PreviewKind::generic;
        if (result.kind == PreviewContentKind::text &&
            result.format == PreviewContentFormat::plain_text)
        {
            kind = glance::app::PreviewKind::text;
        }
        else if (result.kind == PreviewContentKind::text &&
                 result.format == PreviewContentFormat::markdown)
        {
            kind = glance::app::PreviewKind::markdown;
        }
        else if (result.kind == PreviewContentKind::image &&
                 result.format == PreviewContentFormat::image_file)
        {
            kind = glance::app::PreviewKind::image;
        }
        else if (result.kind == PreviewContentKind::media &&
                 result.format == PreviewContentFormat::media_file)
        {
            kind = glance::app::PreviewKind::media;
        }
        else if (result.kind == PreviewContentKind::document &&
                 result.format == PreviewContentFormat::pdf)
        {
            kind = glance::app::PreviewKind::document;
        }
        else if (result.kind == PreviewContentKind::web &&
                 result.format == PreviewContentFormat::html)
        {
            kind = glance::app::PreviewKind::web;
        }
        else if (result.kind == PreviewContentKind::directory &&
                 result.format == PreviewContentFormat::file_directory &&
                 result.file_directory != nullptr)
        {
            kind = glance::app::PreviewKind::archive;
        }

        if (kind == glance::app::PreviewKind::generic ||
            generation != content_generation_ ||
            current_index_ >= files_.size())
        {
            show_provider_error(glance::app::localize(L"ComponentStateError"), generation);
            return;
        }
        if (kind == glance::app::PreviewKind::document &&
            !glance::app::paged_document_renderer().has_value())
        {
            present_generic(files_[current_index_]);
            return;
        }

        if (kind == glance::app::PreviewKind::archive)
        {
            active_component_preview_ = std::move(result.lease);
            active_component_file_directory_ = std::move(result.file_directory);
            active_file_directory_descriptor_ = {};
            active_file_directory_columns_.clear();
            archive_preview_is_directory_ = false;
            content_preview_kind_ = kind;
            current_kind_ = kind;
            update_preview_mode_button();
            update_archive_header_state();
            show_content_panel(kind);
            const auto loading_text = ComponentLoadingText().Text();
            ArchiveStatusText().Text(
                loading_text.empty()
                    ? winrt::hstring(glance::app::localize(L"LoadingArchive"))
                    : loading_text);
            ArchiveEntryTree().RootNodes().Clear();
            FolderEntryList().Items().Clear();
            ComponentLoadingText().Visibility(Visibility::Collapsed);
            load_component_file_directory_async(
                active_component_file_directory_,
                {},
                generation);
            return;
        }

        active_component_preview_ = std::move(result.lease);
        active_component_web_preview_ = std::move(result.web_preview);
        active_component_refinement_ =
            kind == glance::app::PreviewKind::image
            ? std::move(result.refinement)
            : nullptr;
        component_refinement_text_ = std::move(result.refinement_text);
        component_refinement_started_ = false;
        auto component_notice = std::move(result.notice);
        ComponentLoadingText().Visibility(Visibility::Collapsed);
        auto prepared_file = files_[current_index_];
        prepared_file.path = std::move(result.output_path);
        prepared_file.parsing_name = prepared_file.path;
        prepared_file.is_filesystem = true;
        prepared_file.is_cloud_placeholder = false;
        const bool restore_component_placement =
            generation == component_placement_generation_;
        component_placement_generation_ = 0;
        present_resolved_file(prepared_file, kind, generation);
        if (restore_component_placement && !topmost_ && !user_sized_ && !auto_fit_applies())
        {
            position_initial_window();
        }
        if (!component_notice.empty())
        {
            show_preview_message(
                std::move(component_notice),
                InfoBarSeverity::Informational,
                true);
        }
    }

    void MainWindow::begin_component_refinement(std::uint64_t generation)
    {
        if (generation != content_generation_ ||
            current_kind_ != glance::app::PreviewKind::image ||
            active_component_refinement_ == nullptr ||
            component_refinement_started_)
        {
            return;
        }

        component_refinement_started_ = true;
        auto notice = component_refinement_text_.empty()
            ? glance::app::localize(L"Loading")
            : component_refinement_text_;
        refine_component_preview_async(
            active_component_refinement_,
            std::move(notice),
            generation);
    }

    fire_and_forget MainWindow::refine_component_preview_async(
        std::shared_ptr<void> refinement,
        std::wstring notice,
        std::uint64_t generation)
    {
        const auto weak = get_weak();
        const auto dispatcher = DispatcherQueue();
        const auto language = glance::app::current_ui_language();
        show_preview_message(
            std::move(notice),
            InfoBarSeverity::Informational,
            false);
        co_await resume_background();

        auto result = glance::app::refine_component_preview(
            refinement,
            language);
        static_cast<void>(dispatcher.TryEnqueue([
            weak,
            refinement = std::move(refinement),
            result = std::move(result),
            generation]() mutable {
            const auto lifetime = weak.get();
            if (lifetime == nullptr ||
                generation != lifetime->content_generation_ ||
                refinement != lifetime->active_component_refinement_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::image)
            {
                return;
            }
            if (result.status !=
                    glance::contracts::components::PrepareStatus::success ||
                result.output_path.empty())
            {
                lifetime->active_component_refinement_.reset();
                lifetime->component_refinement_text_.clear();
                lifetime->component_refinement_started_ = false;
                if (result.status ==
                    glance::contracts::components::PrepareStatus::cancelled)
                {
                    lifetime->dismiss_preview_info_bar();
                    return;
                }
                lifetime->show_preview_message(
                    result.error_detail.empty()
                        ? glance::app::localize(L"ComponentStateError")
                        : std::move(result.error_detail),
                    InfoBarSeverity::Warning,
                    true);
                return;
            }
            lifetime->apply_component_refinement_async(
                std::move(result),
                std::move(refinement),
                generation);
        }));
    }

    fire_and_forget MainWindow::apply_component_refinement_async(
        glance::app::ComponentPreviewResult result,
        std::shared_ptr<void> refinement,
        std::uint64_t generation)
    {
        try
        {
            const auto path = result.output_path;
            const auto file =
                co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto properties = co_await file.Properties().GetImagePropertiesAsync();
            const auto stream = co_await file.OpenReadAsync();
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            if (generation != content_generation_ ||
                refinement != active_component_refinement_ ||
                current_kind_ != glance::app::PreviewKind::image)
            {
                co_return;
            }

            const auto width = properties.Width() != 0
                ? properties.Width()
                : static_cast<std::uint32_t>(bitmap.PixelWidth());
            const auto height = properties.Height() != 0
                ? properties.Height()
                : static_cast<std::uint32_t>(bitmap.PixelHeight());
            active_component_preview_ = std::move(result.lease);
            active_component_refinement_.reset();
            component_refinement_text_.clear();
            component_refinement_started_ = false;
            image_pixel_width_ = width;
            image_pixel_height_ = height;
            image_bits_per_pixel_ = 0;
            ImagePreview().Source(bitmap);
            update_image_fit_surface();
            update_image_zoom_map();
            update_footer_metadata();
            auto_fit_window_to_content(width, height);
            dismiss_preview_info_bar();
            if (glance::app::footer_field_enabled(
                    footer_preferences_,
                    glance::app::FooterField::media_info))
            {
                load_image_media_info_async(path, generation);
            }
            if (current_index_ < files_.size())
            {
                load_image_metadata_async(
                    files_[current_index_].path,
                    generation,
                    active_component_preview_);
            }
        }
        catch (...)
        {
            if (generation != content_generation_ ||
                refinement != active_component_refinement_)
            {
                co_return;
            }
            active_component_refinement_.reset();
            component_refinement_text_.clear();
            component_refinement_started_ = false;
            show_preview_message(
                glance::app::localize(L"ComponentStateError"),
                InfoBarSeverity::Warning,
                true);
        }
    }

    void MainWindow::apply_text_preview(
        glance::app::TextPreview preview,
        bool markdown,
        bool web,
        std::uint64_t generation,
        bool preview_as_text_attempt)
    {
        if (generation != content_generation_)
        {
            return;
        }
        text_chunk_loading_ = false;
        if (!preview.error.empty())
        {
            set_text_loading(false);
            if (preview_as_text_attempt)
            {
                PreviewAsTextButton().IsEnabled(true);
            }
            else if (web)
            {
                MarkdownCodeButton().IsEnabled(false);
                MarkdownPreviewButton().IsEnabled(web_preview_available_);
                set_markdown_preview_mode(web_preview_available_);
            }
            else if (current_index_ < files_.size())
            {
                present_generic(files_[current_index_], true, true);
            }
            show_text_preview_error(std::move(preview.error));
            return;
        }

        if (preview_as_text_attempt)
        {
            if (current_index_ >= files_.size())
            {
                return;
            }
            if (!prepare_text_preview(files_[current_index_], false))
            {
                return;
            }
        }

        auto initial_content = std::move(preview.content);
        current_text_ = markdown ? initial_content : std::wstring{};
        current_text_reader_ = std::move(preview.reader);
        current_text_has_more_ = preview.has_more;
        text_chunk_loading_ = false;
        if (current_text_encoding_ == glance::app::TextEncoding::automatic)
        {
            EncodingSelector().Content(box_value(preview.encoding));
        }
        TextEncodingText().Text(L"");

        if (text_editor_ != nullptr)
        {
            text_editor_->clear();
            text_editor_->append_text(initial_content);
        }
        update_line_number_visibility();
        set_text_loading(false);
        ensure_text_viewport_filled();

        if (markdown)
        {
            if (web_preview_available_ && web_view_ready_ && !current_text_has_more_)
            {
                render_markdown();
            }
            MarkdownPreviewButton().IsEnabled(
                web_preview_available_ && !current_text_has_more_);
            set_markdown_preview_mode(
                web_preview_available_ && !current_text_has_more_ && markdown_preview_);
        }
        else if (web)
        {
            MarkdownPreviewButton().IsEnabled(web_preview_available_);
            MarkdownCodeButton().IsEnabled(true);
            set_markdown_preview_mode(web_preview_available_ && markdown_preview_);
        }
    }

    fire_and_forget MainWindow::load_next_text_chunk_async(std::uint64_t generation)
    {
        if (text_chunk_loading_ || !current_text_has_more_ || current_text_reader_ == nullptr)
        {
            co_return;
        }

        text_chunk_loading_ = true;
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto reader = current_text_reader_;
        co_await resume_background();
        auto preview = glance::app::load_next_text_preview_chunk(reader, text_chunk_bytes);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), generation]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->text_chunk_loading_ = false;
                if (!preview.error.empty())
                {
                    lifetime->current_text_has_more_ = false;
                    lifetime->current_text_reader_.reset();
                    lifetime->set_text_loading(false);
                    lifetime->show_text_preview_error(std::move(preview.error));
                    return;
                }

                auto appended = std::move(preview.content);
                lifetime->current_text_reader_ = std::move(preview.reader);
                lifetime->current_text_has_more_ = preview.has_more;
                if (lifetime->current_text_markdown_)
                {
                    lifetime->current_text_.append(appended);
                }
                if (lifetime->text_editor_ != nullptr)
                {
                    lifetime->text_editor_->append_text(appended);
                }
                lifetime->set_text_loading(false);
                lifetime->ensure_text_viewport_filled();
                glance::contracts::log_event(
                    L"Incremental text chunk applied: characters=" +
                    std::to_wstring(appended.size()) +
                    L", has_more=" +
                    std::to_wstring(lifetime->current_text_has_more_));
                if (lifetime->current_text_markdown_)
                {
                    if (lifetime->web_preview_available_ &&
                        lifetime->web_view_ready_ &&
                        !lifetime->current_text_has_more_)
                    {
                        lifetime->render_markdown();
                    }
                    lifetime->MarkdownPreviewButton().IsEnabled(
                        lifetime->web_preview_available_ &&
                        !lifetime->current_text_has_more_);
                }
            }));
    }

    fire_and_forget MainWindow::render_markdown()
    {
        if (!web_preview_available_ || !web_view_ready_)
        {
            co_return;
        }
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto markdown = current_text_;
        const bool dark_theme = RootGrid().ActualTheme() == ElementTheme::Dark;
        const auto generation = content_generation_;
        co_await resume_background();
        auto html = glance::app::render_markdown_html(markdown, dark_theme);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, html = std::move(html), generation]() mutable {
                if (generation == lifetime->content_generation_ &&
                    lifetime->web_view_ready_)
                {
                    lifetime->render_markdown_async(std::move(html), generation);
                }
            }));
    }

    fire_and_forget MainWindow::initialize_markdown_web_view_async(std::uint64_t generation)
    {
        if (web_view_ready_ || web_view_initializing_)
        {
            co_return;
        }

        const auto lifetime = get_strong();
        web_view_initializing_ = true;
        try
        {
            const auto web_view = ensure_web_view_control();
            const auto environment =
                co_await glance::app::shared_webview_environment_async();
            co_await web_view.EnsureCoreWebView2Async(environment);
            co_await configure_web_view_core(web_view.CoreWebView2());
            web_view_ready_ = true;
            web_view_initializing_ = false;
            if (current_text_markdown_ && web_preview_available_ &&
                !text_chunk_loading_ && !current_text_has_more_)
            {
                render_markdown();
            }
        }
        catch (const hresult_error& error)
        {
            web_view_initializing_ = false;
            glance::contracts::log_event(
                L"Markdown WebView initialization failed: " + std::wstring(error.message()));
            if (generation == content_generation_ || current_text_markdown_)
            {
                web_preview_available_ = false;
                MarkdownModeButtons().Visibility(Visibility::Collapsed);
                MarkdownPreviewButton().IsEnabled(false);
                set_markdown_preview_mode(false);
            }
        }
    }

    fire_and_forget MainWindow::render_markdown_async(std::wstring html, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
            if (generation != content_generation_ || !web_view_ready_ ||
                web_preview_ == nullptr)
            {
                co_return;
            }

            const auto web_view = web_preview_;
            const auto core = web_view.CoreWebView2();
            clear_web_resource_mappings(core);
            const auto parent = std::filesystem::path(current_text_path_).parent_path();
            if (!parent.empty())
            {
                constexpr wchar_t markdown_host[] =
                    L"glance-markdown-assets.invalid";
                core.SetVirtualHostNameToFolderMapping(
                    markdown_host,
                    parent.c_str(),
                    Microsoft::Web::WebView2::Core::
                        CoreWebView2HostResourceAccessKind::DenyCors);
                web_resource_mapping_hosts_.emplace_back(markdown_host);
            }
            web_content_ready_ = false;
            web_navigation_generation_ = generation;
            web_navigation_id_ = 0;
            web_view.Opacity(0.0);
            WebPreviewHost().Visibility(Visibility::Collapsed);
            web_view.NavigateToString(html);
            web_preview_available_ = true;
            update_web_view_idle_state();
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Markdown WebView initialization failed: " + std::wstring(error.message()));
            if (generation == content_generation_)
            {
                web_preview_available_ = false;
                MarkdownModeButtons().Visibility(Visibility::Collapsed);
                MarkdownPreviewButton().IsEnabled(false);
                set_markdown_preview_mode(false);
            }
        }
    }

    fire_and_forget MainWindow::render_web_document_async(
        std::wstring path,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto component_web_preview = active_component_web_preview_;
        try
        {
            const auto file_url = component_web_preview == nullptr
                ? file_url_from_path(path)
                : std::optional<std::wstring>{};
            if (component_web_preview == nullptr && !file_url.has_value())
            {
                throw hresult_error(E_INVALIDARG, L"Unable to create a file URL.");
            }

            const auto web_view = ensure_web_view_control();
            const auto environment =
                co_await glance::app::shared_webview_environment_async();
            co_await web_view.EnsureCoreWebView2Async(environment);
            co_await configure_web_view_core(web_view.CoreWebView2());
            web_view_ready_ = true;
            if (generation != content_generation_ ||
                current_kind_ != glance::app::PreviewKind::web)
            {
                co_return;
            }

            const auto core = web_view.CoreWebView2();
            clear_web_resource_mappings(core);
            if (component_web_preview != nullptr)
            {
                for (const auto& mapping : component_web_preview->mappings)
                {
                    core.SetVirtualHostNameToFolderMapping(
                        mapping.host_name,
                        mapping.folder_path,
                        mapping.access_kind ==
                                glance::contracts::components::
                                    WebResourceAccessKind::allow
                            ? Microsoft::Web::WebView2::Core::
                                CoreWebView2HostResourceAccessKind::Allow
                            : Microsoft::Web::WebView2::Core::
                                CoreWebView2HostResourceAccessKind::DenyCors);
                    web_resource_mapping_hosts_.push_back(mapping.host_name);
                }
            }
            web_content_ready_ = false;
            web_navigation_generation_ = generation;
            web_navigation_id_ = 0;
            web_view.Opacity(0.0);
            WebPreviewHost().Visibility(Visibility::Collapsed);
            core.Navigate(
                component_web_preview != nullptr
                    ? component_web_preview->navigation_uri
                    : *file_url);
            web_preview_available_ = true;
            MarkdownPreviewButton().IsEnabled(true);
            update_web_view_idle_state();
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Web document initialization failed: " + std::wstring(error.message()));
            if (generation == content_generation_ &&
                current_kind_ == glance::app::PreviewKind::web)
            {
                web_preview_available_ = false;
                MarkdownModeButtons().Visibility(Visibility::Collapsed);
                MarkdownPreviewButton().IsEnabled(false);
                set_markdown_preview_mode(false);
            }
        }
    }

    WebView2 MainWindow::ensure_web_view_control()
    {
        if (web_preview_ == nullptr)
        {
            web_preview_ = WebView2();
            web_preview_.HorizontalAlignment(HorizontalAlignment::Stretch);
            web_preview_.VerticalAlignment(VerticalAlignment::Stretch);
            web_preview_.Opacity(0.0);
            WebPreviewHost().Children().Append(web_preview_);
        }
        const bool dark_theme = RootGrid().ActualTheme() == ElementTheme::Dark;
        web_preview_.DefaultBackgroundColor(
            current_text_web_
                ? dark_theme
                    ? Windows::UI::Color{ 255, 32, 32, 32 }
                    : Windows::UI::Color{ 255, 255, 255, 255 }
                : dark_theme
                    ? Windows::UI::Color{ 255, 32, 32, 32 }
                    : Windows::UI::Color{ 255, 255, 255, 255 });
        return web_preview_;
    }

    Windows::Foundation::IAsyncAction MainWindow::configure_web_view_core(
        Microsoft::Web::WebView2::Core::CoreWebView2 const& core)
    {
        const auto lifetime = get_strong();
        static_cast<void>(lifetime);
        const auto settings = core.Settings();
        settings.AreDefaultContextMenusEnabled(false);
        settings.AreDevToolsEnabled(false);
        settings.IsStatusBarEnabled(false);
        if (web_view_handlers_registered_)
        {
            co_return;
        }

        co_await core.AddScriptToExecuteOnDocumentCreatedAsync(
            web_double_click_fullscreen_script);

        const auto weak = get_weak();
        core.NavigationStarting(
            [weak](auto const&, auto const& args) {
                if (const auto self = weak.get();
                    self != nullptr &&
                    self->web_navigation_generation_ == self->content_generation_)
                {
                    self->web_navigation_id_ = args.NavigationId();
                }
            });
        core.ContentLoading(
            [weak](auto const& sender, auto const& args) {
                const auto self = weak.get();
                if (self == nullptr ||
                    self->web_navigation_generation_ != self->content_generation_ ||
                    self->web_navigation_id_ == 0 ||
                    args.NavigationId() != self->web_navigation_id_)
                {
                    return;
                }

                self->web_content_ready_ = true;
                sender.PostWebMessageAsString(
                    self->double_click_fullscreen_enabled_
                        ? web_double_click_fullscreen_enabled
                        : web_double_click_fullscreen_disabled);
                self->web_preview_.Opacity(1.0);
                self->WebPreviewHost().Visibility(
                    self->markdown_preview_ ? Visibility::Visible : Visibility::Collapsed);
                self->update_web_view_idle_state();
            });
        core.NavigationCompleted(
            [weak](auto const&, auto const& args) {
                const auto self = weak.get();
                if (self == nullptr ||
                    self->web_navigation_generation_ != self->content_generation_ ||
                    args.NavigationId() != self->web_navigation_id_ ||
                    args.IsSuccess())
                {
                    return;
                }

                glance::contracts::log_event(
                    L"Web preview navigation failed. Status=" +
                    std::to_wstring(static_cast<int>(args.WebErrorStatus())));
            });
        core.WebMessageReceived(
            [weak](auto const&, auto const& args) {
                if (const auto self = weak.get();
                    self != nullptr &&
                    self->web_navigation_generation_ == self->content_generation_)
                {
                    try
                    {
                        const std::wstring message =
                            args.TryGetWebMessageAsString().c_str();
                        if (message == web_toggle_fullscreen)
                        {
                            static_cast<void>(self->handle_preview_content_double_click());
                            return;
                        }
                        glance::contracts::log_event(
                            L"Web preview: " + message);
                    }
                    catch (...)
                    {
                    }
                }
            });
        web_view_handlers_registered_ = true;
    }

    void MainWindow::clear_web_resource_mappings(
        Microsoft::Web::WebView2::Core::CoreWebView2 const& core) noexcept
    {
        for (const auto& host : web_resource_mapping_hosts_)
        {
            try
            {
                core.ClearVirtualHostNameToFolderMapping(host);
            }
            catch (...)
            {
            }
        }
        web_resource_mapping_hosts_.clear();
    }

    void MainWindow::clear_web_view_content() noexcept
    {
        web_preview_available_ = false;
        web_content_ready_ = false;
        web_navigation_generation_ = 0;
        web_navigation_id_ = 0;
        try
        {
            if (web_preview_ != nullptr)
            {
                if (const auto core = web_preview_.CoreWebView2())
                {
                    core.Stop();
                    clear_web_resource_mappings(core);
                }
                web_preview_.Opacity(0.0);
            }
        }
        catch (...)
        {
        }
        WebPreviewHost().Visibility(Visibility::Collapsed);
        update_web_view_idle_state();
    }

    void MainWindow::update_web_view_idle_state()
    {
        if (web_view_idle_timer_ == nullptr)
        {
            return;
        }
        web_view_idle_timer_.Stop();
        if (web_preview_ == nullptr)
        {
            return;
        }
        const bool active = visible_ && !basic_info_mode_ && markdown_preview_ &&
            (current_kind_ == glance::app::PreviewKind::markdown ||
             current_kind_ == glance::app::PreviewKind::web) &&
            WebPreviewHost().Visibility() == Visibility::Visible;
        if (!active)
        {
            web_view_idle_timer_.Start();
        }
    }

    void MainWindow::release_web_view_control() noexcept
    {
        if (web_view_idle_timer_ != nullptr)
        {
            web_view_idle_timer_.Stop();
        }
        if (web_preview_ == nullptr)
        {
            return;
        }
        const auto web_view = web_preview_;
        web_preview_ = nullptr;
        web_view_initializing_ = false;
        web_view_ready_ = false;
        web_view_handlers_registered_ = false;
        web_content_ready_ = false;
        web_navigation_generation_ = 0;
        web_navigation_id_ = 0;
        try
        {
            if (const auto core = web_view.CoreWebView2())
            {
                clear_web_resource_mappings(core);
            }
            WebPreviewHost().Visibility(Visibility::Collapsed);
            WebPreviewHost().Children().Clear();
        }
        catch (...)
        {
        }
        try
        {
            web_view.Close();
        }
        catch (...)
        {
        }
    }

    void MainWindow::set_text_loading(bool loading)
    {
        text_loading_ = loading;
        TextLoadingText().Text(loading ? glance::app::localize(L"Loading") : L"");
        TextLoadingOverlay().Visibility(loading ? Visibility::Visible : Visibility::Collapsed);
        update_text_editor_visibility();
    }

    bool MainWindow::ensure_text_editor()
    {
        if (text_editor_ != nullptr)
        {
            return text_editor_->available();
        }

        const auto weak = get_weak();
        text_editor_ = std::make_unique<glance::app::ScintillaTextView>(
            window_,
            [weak] {
                if (const auto self = weak.get())
                {
                    self->ensure_text_viewport_filled();
                }
            },
            [weak](int steps) {
                if (const auto self = weak.get())
                {
                    self->adjust_text_font_size(steps);
                }
            },
            [weak] {
                if (const auto self = weak.get())
                {
                    return self->handle_preview_content_double_click();
                }
                return false;
            });
        if (!text_editor_->available())
        {
            return false;
        }
        update_text_editor_bounds();
        return true;
    }

    void MainWindow::update_text_editor_bounds() noexcept
    {
        if (text_editor_ == nullptr || !text_editor_->available())
        {
            return;
        }
        try
        {
            const auto xaml_root = TextEditorHost().XamlRoot();
            if (xaml_root == nullptr)
            {
                return;
            }
            const auto origin = TextEditorHost()
                .TransformToVisual(nullptr)
                .TransformPoint({ 0.0F, 0.0F });
            const double scale = xaml_root.RasterizationScale();
            text_editor_->set_bounds(
                static_cast<int>(std::lround(origin.X * scale)),
                static_cast<int>(std::lround(origin.Y * scale)),
                static_cast<int>(std::lround(TextEditorHost().ActualWidth() * scale)),
                static_cast<int>(std::lround(TextEditorHost().ActualHeight() * scale)));
            update_text_editor_occlusions();
        }
        catch (...)
        {
        }
    }

    void MainWindow::update_text_editor_occlusions() noexcept
    {
        if (text_editor_ == nullptr || !text_editor_->available())
        {
            return;
        }
        try
        {
            const auto xaml_root = TextEditorHost().XamlRoot();
            if (xaml_root == nullptr)
            {
                return;
            }
            const double scale = xaml_root.RasterizationScale();
            std::vector<RECT> rectangles;
            const auto append = [&](FrameworkElement const& element) {
                if (element.Visibility() != Visibility::Visible ||
                    element.ActualWidth() <= 0.0 ||
                    element.ActualHeight() <= 0.0)
                {
                    return;
                }
                const auto origin = element
                    .TransformToVisual(TextEditorHost())
                    .TransformPoint({ 0.0F, 0.0F });
                constexpr int padding = 3;
                rectangles.push_back({
                    static_cast<LONG>(std::floor(origin.X * scale)) - padding,
                    static_cast<LONG>(std::floor(origin.Y * scale)) - padding,
                    static_cast<LONG>(std::ceil(
                        (origin.X + static_cast<float>(element.ActualWidth())) * scale)) + padding,
                    static_cast<LONG>(std::ceil(
                        (origin.Y + static_cast<float>(element.ActualHeight())) * scale)) + padding,
                });
            };
            if (PreviewErrorInfoBar().IsOpen())
            {
                append(PreviewErrorInfoBar());
            }
            if (fullscreen_ && fullscreen_title_visible_)
            {
                append(PreviewTitleBar());
            }
            if (fullscreen_ && fullscreen_footer_visible_)
            {
                append(PreviewFooterBar());
            }
            append(TextFontSizeOverlay());
            text_editor_->set_occlusions(rectangles);
        }
        catch (...)
        {
        }
    }

    void MainWindow::queue_text_editor_occlusion_update()
    {
        const auto weak = get_weak();
        static_cast<void>(DispatcherQueue().TryEnqueue(
            Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [weak] {
                if (const auto self = weak.get())
                {
                    self->update_text_editor_occlusions();
                }
            }));
    }

    void MainWindow::update_text_editor_visibility() noexcept
    {
        if (text_editor_ == nullptr)
        {
            return;
        }
        const bool code_visible =
            visible_ &&
            !text_loading_ &&
            !markdown_preview_ &&
            TextPanel().Visibility() == Visibility::Visible;
        text_editor_->set_visible(code_visible);
    }

    void MainWindow::apply_text_preferences()
    {
        text_preferences_ = glance::app::load_text_preferences();
        line_numbers_visible_ = text_preferences_.line_numbers;
        syntax_highlighting_ = text_preferences_.syntax_highlighting;
        word_wrap_ = text_preferences_.word_wrap;
        SyntaxHighlightButton().IsChecked(syntax_highlighting_);
        WordWrapButton().IsChecked(word_wrap_);
        update_text_layout();
        update_line_number_visibility();
    }

    void MainWindow::apply_text_font_metrics()
    {
        if (text_editor_ != nullptr)
        {
            text_editor_->set_preferences(
                text_preferences_,
                syntax_highlighting_,
                RootGrid().ActualTheme() == ElementTheme::Dark);
        }
    }

    void MainWindow::update_text_layout()
    {
        if (text_editor_ != nullptr)
        {
            text_editor_->set_preferences(
                text_preferences_,
                syntax_highlighting_,
                RootGrid().ActualTheme() == ElementTheme::Dark);
        }
        ensure_text_viewport_filled();
    }

    void MainWindow::adjust_text_font_size(int steps)
    {
        if (steps == 0)
        {
            return;
        }
        const double font_size = std::clamp(
            text_preferences_.font_size + steps,
            7.0,
            32.0);
        if (font_size != text_preferences_.font_size)
        {
            text_preferences_.font_size = font_size;
            glance::app::save_text_preferences(text_preferences_);
            apply_text_font_metrics();
        }
        show_text_font_size_overlay();
    }

    void MainWindow::ensure_text_viewport_filled()
    {
        if (text_chunk_loading_ ||
            !current_text_has_more_ ||
            current_text_reader_ == nullptr ||
            text_editor_ == nullptr ||
            !text_editor_->should_load_more())
        {
            return;
        }
        load_next_text_chunk_async(content_generation_);
    }

    void MainWindow::show_content_panel(glance::app::PreviewKind kind)
    {
        const bool text = kind == glance::app::PreviewKind::text ||
            kind == glance::app::PreviewKind::markdown ||
            kind == glance::app::PreviewKind::web;
        const bool component_web =
            kind == glance::app::PreviewKind::web &&
            active_component_web_preview_ != nullptr;
        GenericPanel().Visibility(kind == glance::app::PreviewKind::generic ? Visibility::Visible : Visibility::Collapsed);
        TextPanel().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        ImagePanel().Visibility(kind == glance::app::PreviewKind::image ? Visibility::Visible : Visibility::Collapsed);
        MediaPanel().Visibility(kind == glance::app::PreviewKind::media ? Visibility::Visible : Visibility::Collapsed);
        PdfPanel().Visibility(kind == glance::app::PreviewKind::document ? Visibility::Visible : Visibility::Collapsed);
        ArchivePanel().Visibility(kind == glance::app::PreviewKind::archive ? Visibility::Visible : Visibility::Collapsed);
        ImageStatusControls().Visibility(
            kind == glance::app::PreviewKind::image ? Visibility::Visible : Visibility::Collapsed);
        GalleryModeButton().Visibility(
            gallery_source_available() &&
                    gallery_media_kind_ != glance::contracts::components::GalleryMediaKind::none
                ? Visibility::Visible
                : Visibility::Collapsed);
        GalleryModeButton().IsChecked(gallery_mode_ != GalleryMode::inactive);
        TextStatusControls().Visibility(
            text && !component_web ? Visibility::Visible : Visibility::Collapsed);
        LineNumbersButton().Visibility(
            text && !component_web ? Visibility::Visible : Visibility::Collapsed);
        SyntaxHighlightButton().Visibility(
            text && !component_web ? Visibility::Visible : Visibility::Collapsed);
        WordWrapButton().Visibility(
            text && !component_web ? Visibility::Visible : Visibility::Collapsed);
        if (kind != glance::app::PreviewKind::generic)
        {
            GenericAdvancedInfoButton().Visibility(Visibility::Collapsed);
        }
        if (kind != glance::app::PreviewKind::image)
        {
            ImagePreview().Source(nullptr);
            image_metadata_.clear();
            image_metadata_json_.clear();
            image_metadata_visible_ = false;
            ImageExifButton().IsChecked(false);
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
        }
        if (kind != glance::app::PreviewKind::media)
        {
            stop_media_playback();
        }
        if (kind != glance::app::PreviewKind::document)
        {
            cancel_pdf_render();
            PdfPageImage().Source(nullptr);
            hide_password_prompt();
        }
        if (!text)
        {
            clear_web_view_content();
        }
        update_text_editor_visibility();
        rebuild_component_contributions();
    }

    void MainWindow::RefreshComponentContributions()
    {
        rebuild_component_contributions();
    }

    void MainWindow::reset_component_hover_info() noexcept
    {
        if (component_hover_cancellation_ != nullptr)
        {
            component_hover_cancellation_->store(true, std::memory_order_release);
            component_hover_cancellation_.reset();
        }
        if (component_data_copy_cancellation_ != nullptr)
        {
            component_data_copy_cancellation_->store(true, std::memory_order_release);
            component_data_copy_cancellation_.reset();
        }
        active_component_hover_ = {};
        ComponentHoverInfoProgressRing().IsActive(false);
        ComponentHoverInfoProgressRing().Visibility(Visibility::Collapsed);
        ComponentHoverInfoText().Blocks().Clear();
        ComponentHoverInfoOverlay().Visibility(Visibility::Collapsed);
    }

    void MainWindow::rebuild_component_contributions()
    {
        reset_component_hover_info();
        component_hover_info_text_.clear();
        component_hover_cache_component_id_.clear();
        component_hover_cache_info_id_.clear();
        ComponentStatusControls().Children().Clear();
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }

        using glance::contracts::components::PreviewContentFormat;
        using glance::contracts::components::PreviewContentKind;
        PreviewContentKind kind{ PreviewContentKind::none };
        PreviewContentFormat format{ PreviewContentFormat::none };
        switch (current_kind_)
        {
        case glance::app::PreviewKind::text:
            kind = PreviewContentKind::text;
            format = PreviewContentFormat::plain_text;
            break;
        case glance::app::PreviewKind::markdown:
            kind = PreviewContentKind::text;
            format = PreviewContentFormat::markdown;
            break;
        case glance::app::PreviewKind::image:
            kind = PreviewContentKind::image;
            format = PreviewContentFormat::image_file;
            break;
        case glance::app::PreviewKind::media:
            kind = PreviewContentKind::media;
            format = PreviewContentFormat::media_file;
            break;
        case glance::app::PreviewKind::document:
            kind = PreviewContentKind::document;
            format = PreviewContentFormat::pdf;
            break;
        case glance::app::PreviewKind::web:
            kind = PreviewContentKind::web;
            format = PreviewContentFormat::html;
            break;
        case glance::app::PreviewKind::archive:
            kind = PreviewContentKind::directory;
            format = PreviewContentFormat::file_directory;
            break;
        default:
            return;
        }

        const auto shortcuts = glance::app::component_status_bar_shortcuts(
            files_[current_index_].path,
            kind,
            format,
            glance::app::current_ui_language());
        const auto style = Application::Current().Resources()
            .Lookup(box_value(L"IconToggleButtonStyle")).as<Style>();
        const auto weak = get_weak();
        for (const auto& shortcut : shortcuts)
        {
            Controls::Primitives::ToggleButton button;
            button.Style(style);
            Controls::FontIcon icon;
            icon.FontSize(14);
            icon.Glyph(std::wstring(
                1,
                static_cast<wchar_t>(shortcut.fluent_icon_glyph)));
            button.Content(icon);
            ToolTipService::SetToolTip(button, box_value(shortcut.tooltip));
            button.Click([weak, shortcut](IInspectable const& sender, RoutedEventArgs const&) {
                if (const auto self = weak.get())
                {
                    self->activate_component_shortcut(
                        shortcut,
                        sender.as<Controls::Primitives::ToggleButton>());
                }
            });
            if (shortcut.supports_data_copy)
            {
                button.RightTapped(
                    [weak, shortcut, icon](
                        IInspectable const&,
                        RightTappedRoutedEventArgs const& args) {
                        args.Handled(true);
                        if (const auto self = weak.get())
                        {
                            self->copy_component_shortcut_data_async(shortcut, icon);
                        }
                    });
            }
            ComponentStatusControls().Children().Append(button);
        }
    }

    void MainWindow::activate_component_shortcut(
        const glance::app::ComponentStatusBarShortcut& shortcut,
        const Controls::Primitives::ToggleButton& button)
    {
        if (current_index_ >= files_.size())
        {
            button.IsChecked(false);
            return;
        }
        const bool requested_checked = button.IsChecked().Value();
        auto activation = glance::app::activate_component_status_bar_shortcut(
            shortcut,
            files_[current_index_].path,
            glance::app::current_ui_language(),
            requested_checked);
        if (activation.kind == glance::app::ComponentStatusBarActivationKind::request_component_action)
        {
            button.IsChecked(false);
            if (const auto action = glance::app::component_management_action(
                    activation.component_id,
                    activation.component_action_id,
                    glance::app::current_ui_language()))
            {
                confirm_component_action(*action);
            }
            return;
        }
        if (activation.kind != glance::app::ComponentStatusBarActivationKind::toggle_hover_info ||
            !activation.checked)
        {
            button.IsChecked(false);
            reset_component_hover_info();
            return;
        }

        for (const auto& child : ComponentStatusControls().Children())
        {
            const auto toggle = child.try_as<Controls::Primitives::ToggleButton>();
            if (toggle != nullptr && toggle != button)
            {
                toggle.IsChecked(false);
            }
        }
        reset_component_hover_info();
        active_component_hover_ = activation;
        ComponentHoverInfoOverlay().Visibility(Visibility::Visible);
        if (component_hover_cache_component_id_ == activation.component_id &&
            component_hover_cache_info_id_ == activation.hover_info_id &&
            !component_hover_info_text_.empty())
        {
            set_information_panel_text(
                ComponentHoverInfoText(),
                component_hover_info_text_,
                true);
            return;
        }

        set_information_panel_text(
            ComponentHoverInfoText(),
            activation.loading_text,
            false);
        ComponentHoverInfoProgressRing().Visibility(Visibility::Visible);
        ComponentHoverInfoProgressRing().IsActive(true);
        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        component_hover_cancellation_ = cancellation;
        load_component_hover_info_async(
            std::move(activation),
            files_[current_index_].path,
            content_generation_,
            std::move(cancellation));
    }

    fire_and_forget MainWindow::load_component_hover_info_async(
        glance::app::ComponentStatusBarActivation activation,
        std::wstring path,
        std::uint64_t generation,
        std::shared_ptr<std::atomic_bool> cancellation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const std::wstring language = glance::app::current_ui_language();
        co_await resume_background();
        auto text = glance::app::query_component_hover_info(
            activation,
            path,
            language,
            *cancellation);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime,
             activation = std::move(activation),
             cancellation = std::move(cancellation),
             generation,
             text = std::move(text)]() mutable {
                if (generation != lifetime->content_generation_ ||
                    cancellation->load(std::memory_order_acquire) ||
                    lifetime->component_hover_cancellation_ != cancellation ||
                    lifetime->active_component_hover_.component_id != activation.component_id ||
                    lifetime->active_component_hover_.hover_info_id != activation.hover_info_id)
                {
                    return;
                }
                lifetime->component_hover_cancellation_.reset();
                lifetime->ComponentHoverInfoProgressRing().IsActive(false);
                lifetime->ComponentHoverInfoProgressRing().Visibility(Visibility::Collapsed);
                if (text.empty())
                {
                    lifetime->ComponentHoverInfoOverlay().Visibility(Visibility::Collapsed);
                    return;
                }
                lifetime->component_hover_info_text_ = text;
                lifetime->component_hover_cache_component_id_ = activation.component_id;
                lifetime->component_hover_cache_info_id_ = activation.hover_info_id;
                set_information_panel_text(
                    lifetime->ComponentHoverInfoText(),
                    lifetime->component_hover_info_text_,
                    true);
            }));
    }

    fire_and_forget MainWindow::copy_component_shortcut_data_async(
        glance::app::ComponentStatusBarShortcut shortcut,
        FontIcon feedback_icon)
    {
        if (!shortcut.supports_data_copy || current_index_ >= files_.size())
        {
            co_return;
        }
        if (component_data_copy_cancellation_ != nullptr)
        {
            component_data_copy_cancellation_->store(true, std::memory_order_release);
        }
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto path = files_[current_index_].path;
        const auto generation = content_generation_;
        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        component_data_copy_cancellation_ = cancellation;
        co_await resume_background();
        auto json = glance::app::query_component_status_bar_shortcut_data(
            shortcut,
            path,
            *cancellation);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime,
             cancellation = std::move(cancellation),
             generation,
             feedback_icon = std::move(feedback_icon),
             json = std::move(json)]() mutable {
                if (generation != lifetime->content_generation_ ||
                    cancellation->load(std::memory_order_acquire) ||
                    lifetime->component_data_copy_cancellation_ != cancellation)
                {
                    return;
                }
                lifetime->component_data_copy_cancellation_.reset();
                lifetime->copy_text_to_clipboard(
                    json,
                    L"Copy component data",
                    feedback_icon);
            }));
    }

    fire_and_forget MainWindow::confirm_component_action(
        glance::app::ComponentManagementAction action)
    {
        const auto lifetime = get_strong();
        try
        {
            Controls::ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(action.confirmation_title));
            dialog.Content(box_value(action.confirmation_message));
            dialog.PrimaryButtonText(action.confirmation_button);
            dialog.CloseButtonText(glance::app::localize(L"Cancel"));
            dialog.DefaultButton(Controls::ContentDialogButton::Primary);
            if (co_await dialog.ShowAsync() != Controls::ContentDialogResult::Primary)
            {
                co_return;
            }
            if (lifetime->detached_)
            {
                lifetime->stop_detached_focus_monitor();
                lifetime->clear_preview_content();
                lifetime->Close();
            }
            else
            {
                lifetime->HidePreview();
            }
            if (lifetime->component_action_callback_)
            {
                lifetime->component_action_callback_(
                    std::move(action.component_id),
                    std::move(action.action_id));
            }
        }
        catch (...)
        {
        }
    }

    void MainWindow::dismiss_preview_info_bar()
    {
        preview_notice_active_ = false;
        preview_notice_hiding_ = false;
        preview_notice_resource_key_.clear();
        if (preview_notice_timer_ != nullptr)
        {
            preview_notice_timer_.Stop();
        }
        if (preview_notice_hide_timer_ != nullptr)
        {
            preview_notice_hide_timer_.Stop();
        }
        const auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(
            PreviewErrorInfoBar());
        visual.StopAnimation(L"Opacity");
        visual.Opacity(1.0F);
        PreviewErrorInfoBar().IsOpen(false);
        queue_text_editor_occlusion_update();
    }

    void MainWindow::show_preview_notice(std::wstring resource_key)
    {
        const auto message = glance::app::localize(resource_key);
        show_preview_message(
            message,
            InfoBarSeverity::Informational,
            true);
        preview_notice_resource_key_ = std::move(resource_key);
    }

    void MainWindow::show_preview_message(
        std::wstring message,
        InfoBarSeverity severity,
        bool auto_hide)
    {
        dismiss_preview_info_bar();
        preview_notice_active_ = true;
        PreviewErrorInfoBar().Title(L"");
        PreviewErrorInfoBar().Message(std::move(message));
        PreviewErrorInfoBar().Severity(severity);
        PreviewErrorInfoBar().IsClosable(false);
        PreviewErrorInfoBar().IsOpen(true);
        queue_text_editor_occlusion_update();
        animate_preview_info_bar(true);
        if (!auto_hide)
        {
            return;
        }
        if (preview_notice_timer_ == nullptr)
        {
            preview_notice_timer_ = DispatcherTimer();
            preview_notice_timer_.Interval(std::chrono::milliseconds(2880));
            const auto weak = get_weak();
            preview_notice_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->preview_notice_timer_.Stop();
                    if (!self->preview_notice_active_)
                    {
                        return;
                    }
                    self->preview_notice_hiding_ = true;
                    self->animate_preview_info_bar(false);
                    self->preview_notice_hide_timer_.Start();
                }
            });
        }
        if (preview_notice_hide_timer_ == nullptr)
        {
            preview_notice_hide_timer_ = DispatcherTimer();
            preview_notice_hide_timer_.Interval(std::chrono::milliseconds(120));
            const auto weak = get_weak();
            preview_notice_hide_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->preview_notice_hide_timer_.Stop();
                    if (!self->preview_notice_hiding_)
                    {
                        return;
                    }
                    self->preview_notice_active_ = false;
                    self->preview_notice_hiding_ = false;
                    self->preview_notice_resource_key_.clear();
                    self->PreviewErrorInfoBar().IsOpen(false);
                    self->queue_text_editor_occlusion_update();
                    const auto visual =
                        Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(
                            self->PreviewErrorInfoBar());
                    visual.StopAnimation(L"Opacity");
                    visual.Opacity(1.0F);
                }
            });
        }
        preview_notice_timer_.Start();
    }

    void MainWindow::animate_preview_info_bar(bool opening)
    {
        const auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(
            PreviewErrorInfoBar());
        const auto compositor = visual.Compositor();
        visual.StopAnimation(L"Opacity");
        visual.Opacity(opening ? 1.0F : 0.0F);

        const auto animation = compositor.CreateScalarKeyFrameAnimation();
        animation.Duration(opening
            ? std::chrono::milliseconds(140)
            : std::chrono::milliseconds(120));
        const auto easing = compositor.CreateCubicBezierEasingFunction(
            opening
                ? Windows::Foundation::Numerics::float2{ 0.16F, 1.0F }
                : Windows::Foundation::Numerics::float2{ 0.70F, 0.0F },
            opening
                ? Windows::Foundation::Numerics::float2{ 0.30F, 1.0F }
                : Windows::Foundation::Numerics::float2{ 0.84F, 0.0F });
        animation.InsertKeyFrame(0.0F, opening ? 0.0F : 1.0F);
        animation.InsertKeyFrame(1.0F, opening ? 1.0F : 0.0F, easing);
        visual.StartAnimation(L"Opacity", animation);
    }

    void MainWindow::show_text_preview_error(std::wstring message)
    {
        dismiss_preview_info_bar();
        PreviewErrorInfoBar().Title(glance::app::localize(L"PreviewErrorInfoBar.Title"));
        PreviewErrorInfoBar().Message(std::move(message));
        PreviewErrorInfoBar().Severity(InfoBarSeverity::Error);
        PreviewErrorInfoBar().IsClosable(true);
        PreviewErrorInfoBar().IsOpen(true);
        queue_text_editor_occlusion_update();
        animate_preview_info_bar(true);
    }

    void MainWindow::update_preview_mode_button()
    {
        const bool available =
            content_preview_kind_ != glance::app::PreviewKind::generic &&
            current_index_ < files_.size() &&
            !files_[current_index_].path.empty() &&
            !files_[current_index_].is_cloud_placeholder &&
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        PreviewModeButton().Visibility(available ? Visibility::Visible : Visibility::Collapsed);
        PreviewModeButton().IsChecked(available && basic_info_mode_);
        ToolTipService::SetToolTip(
            PreviewModeButton(),
            box_value(glance::app::localize(
                available && basic_info_mode_
                    ? L"PreviewModeShowContentTooltip"
                    : L"PreviewModeShowInfoTooltip")));
    }

    void MainWindow::update_preview_as_text_button()
    {
        const bool available =
            generic_text_preview_allowed_ &&
            !basic_info_mode_ &&
            current_kind_ == glance::app::PreviewKind::generic &&
            current_index_ < files_.size() &&
            !files_[current_index_].path.empty() &&
            !files_[current_index_].is_cloud_placeholder &&
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            files_[current_index_].size <= maximum_preview_as_text_bytes &&
            glance::app::can_try_preview_as_text(files_[current_index_].path);
        PreviewAsTextButton().Visibility(available ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::show_provider_error(std::wstring message, std::uint64_t generation)
    {
        if (generation != content_generation_ || current_index_ >= files_.size())
        {
            return;
        }
        present_generic(files_[current_index_]);
        ErrorText().Text(std::move(message));
        ErrorText().Visibility(Visibility::Visible);
        reveal_deferred_preview();
    }

    void MainWindow::update_image_fit_surface()
    {
        const double viewport_width = std::max(1.0, ImagePanel().ActualWidth());
        const double viewport_height = std::max(1.0, ImagePanel().ActualHeight());
        ImageFitSurface().Width(viewport_width);
        ImageFitSurface().Height(viewport_height);

        if (image_pixel_width_ == 0 || image_pixel_height_ == 0)
        {
            return;
        }

        const bool swaps_dimensions =
            static_cast<int>(std::lround(image_rotation_)) % 180 != 0;
        const double rotated_width = swaps_dimensions ? image_pixel_height_ : image_pixel_width_;
        const double rotated_height = swaps_dimensions ? image_pixel_width_ : image_pixel_height_;
        const double fit_scale = std::min(
            viewport_width / rotated_width,
            viewport_height / rotated_height);
        ImagePreview().Width(std::max(1.0, image_pixel_width_ * fit_scale));
        ImagePreview().Height(std::max(1.0, image_pixel_height_ * fit_scale));
    }

    void MainWindow::fit_image_to_viewport()
    {
        ImageScroller().CancelDirectManipulations();
        ImageScroller().ReleasePointerCaptures();
        ImageZoomMapOverlay().ReleasePointerCaptures();
        image_panning_ = false;
        image_zoom_map_panning_ = false;
        update_image_fit_surface();
        static_cast<void>(ImageScroller().ChangeView(0.0, 0.0, 1.0F, true));
        update_image_zoom_map();
    }

    void MainWindow::update_image_zoom_map()
    {
        const double zoom = ImageScroller().ZoomFactor();
        if (!image_zoom_map_enabled_ ||
            current_kind_ != glance::app::PreviewKind::image ||
            ImagePreview().Source() == nullptr ||
            image_pixel_width_ == 0 ||
            image_pixel_height_ == 0 ||
            zoom <= 1.001)
        {
            ImageZoomMapOverlay().Visibility(Visibility::Collapsed);
            return;
        }

        constexpr double maximum_map_width = 180.0;
        constexpr double maximum_map_height = 120.0;
        const bool swaps_axes =
            static_cast<int>(std::lround(image_rotation_)) % 180 != 0;
        const double rotated_pixel_width = swaps_axes
            ? image_pixel_height_
            : image_pixel_width_;
        const double rotated_pixel_height = swaps_axes
            ? image_pixel_width_
            : image_pixel_height_;
        const double map_scale = std::min(
            maximum_map_width / rotated_pixel_width,
            maximum_map_height / rotated_pixel_height);
        const double mapped_width = rotated_pixel_width * map_scale;
        const double mapped_height = rotated_pixel_height * map_scale;
        const double image_width = swaps_axes ? mapped_height : mapped_width;
        const double image_height = swaps_axes ? mapped_width : mapped_height;
        const double image_left = (mapped_width - image_width) / 2.0;
        const double image_top = (mapped_height - image_height) / 2.0;

        ImageZoomMapOverlay().Width(mapped_width);
        ImageZoomMapOverlay().Height(mapped_height);
        ImageZoomMapCanvas().Width(mapped_width);
        ImageZoomMapCanvas().Height(mapped_height);
        ImageZoomMapViewportLayer().Width(mapped_width);
        ImageZoomMapViewportLayer().Height(mapped_height);

        const auto configure_map_image = [&](const Border& frame, const auto& transform) {
            frame.Width(image_width);
            frame.Height(image_height);
            Canvas::SetLeft(frame, image_left);
            Canvas::SetTop(frame, image_top);
            transform.Rotation(image_rotation_);
            transform.ScaleX(image_scale_x_);
            transform.ScaleY(image_scale_y_);
        };
        configure_map_image(ImageZoomMapBaseFrame(), ImageZoomMapBaseTransform());
        configure_map_image(ImageZoomMapViewportFrame(), ImageZoomMapViewportTransform());

        const double surface_width = ImageFitSurface().Width();
        const double surface_height = ImageFitSurface().Height();
        const double displayed_width = swaps_axes
            ? ImagePreview().Height()
            : ImagePreview().Width();
        const double displayed_height = swaps_axes
            ? ImagePreview().Width()
            : ImagePreview().Height();
        const double zoomed_image_left = (surface_width - displayed_width) * zoom / 2.0;
        const double zoomed_image_top = (surface_height - displayed_height) * zoom / 2.0;
        const double zoomed_image_width = displayed_width * zoom;
        const double zoomed_image_height = displayed_height * zoom;
        const double viewport_width = std::max(
            1.0,
            ImageScroller().ViewportWidth() > 0.0
                ? ImageScroller().ViewportWidth()
                : ImageScroller().ActualWidth());
        const double viewport_height = std::max(
            1.0,
            ImageScroller().ViewportHeight() > 0.0
                ? ImageScroller().ViewportHeight()
                : ImageScroller().ActualHeight());
        const double visible_left = std::clamp(
            (ImageScroller().HorizontalOffset() - zoomed_image_left) / zoomed_image_width,
            0.0,
            1.0);
        const double visible_top = std::clamp(
            (ImageScroller().VerticalOffset() - zoomed_image_top) / zoomed_image_height,
            0.0,
            1.0);
        const double visible_right = std::clamp(
            (ImageScroller().HorizontalOffset() + viewport_width - zoomed_image_left) /
                zoomed_image_width,
            0.0,
            1.0);
        const double visible_bottom = std::clamp(
            (ImageScroller().VerticalOffset() + viewport_height - zoomed_image_top) /
                zoomed_image_height,
            0.0,
            1.0);
        if (visible_right <= visible_left || visible_bottom <= visible_top)
        {
            ImageZoomMapOverlay().Visibility(Visibility::Collapsed);
            return;
        }

        const double viewport_left = visible_left * mapped_width;
        const double viewport_top = visible_top * mapped_height;
        const double viewport_map_width = (visible_right - visible_left) * mapped_width;
        const double viewport_map_height = (visible_bottom - visible_top) * mapped_height;
        ImageZoomMapViewportClip().Rect(Windows::Foundation::Rect{
            static_cast<float>(viewport_left),
            static_cast<float>(viewport_top),
            static_cast<float>(viewport_map_width),
            static_cast<float>(viewport_map_height) });
        ImageZoomMapViewportBorder().Width(viewport_map_width);
        ImageZoomMapViewportBorder().Height(viewport_map_height);
        Canvas::SetLeft(ImageZoomMapViewportBorder(), viewport_left);
        Canvas::SetTop(ImageZoomMapViewportBorder(), viewport_top);
        ImageZoomMapOverlay().Visibility(Visibility::Visible);
    }

    void MainWindow::move_image_viewport_from_zoom_map(Windows::Foundation::Point position)
    {
        const double map_width = ImageZoomMapCanvas().ActualWidth();
        const double map_height = ImageZoomMapCanvas().ActualHeight();
        if (map_width <= 0.0 || map_height <= 0.0)
        {
            return;
        }

        const double zoom = ImageScroller().ZoomFactor();
        const bool swaps_axes =
            static_cast<int>(std::lround(image_rotation_)) % 180 != 0;
        const double displayed_width = swaps_axes
            ? ImagePreview().Height()
            : ImagePreview().Width();
        const double displayed_height = swaps_axes
            ? ImagePreview().Width()
            : ImagePreview().Height();
        const double zoomed_image_left =
            (ImageFitSurface().Width() - displayed_width) * zoom / 2.0;
        const double zoomed_image_top =
            (ImageFitSurface().Height() - displayed_height) * zoom / 2.0;
        const double viewport_width = std::max(
            1.0,
            ImageScroller().ViewportWidth() > 0.0
                ? ImageScroller().ViewportWidth()
                : ImageScroller().ActualWidth());
        const double viewport_height = std::max(
            1.0,
            ImageScroller().ViewportHeight() > 0.0
                ? ImageScroller().ViewportHeight()
                : ImageScroller().ActualHeight());
        const double horizontal = zoomed_image_left +
            std::clamp(static_cast<double>(position.X) / map_width, 0.0, 1.0) *
                displayed_width * zoom -
            viewport_width / 2.0;
        const double vertical = zoomed_image_top +
            std::clamp(static_cast<double>(position.Y) / map_height, 0.0, 1.0) *
                displayed_height * zoom -
            viewport_height / 2.0;
        static_cast<void>(ImageScroller().ChangeView(
            std::max(0.0, horizontal),
            std::max(0.0, vertical),
            nullptr,
            true));
    }

    void MainWindow::update_pdf_fit_surface()
    {
        PdfFitSurface().Width(std::max(1.0, PdfScroller().ActualWidth()));
        PdfFitSurface().Height(std::max(1.0, PdfScroller().ActualHeight()));
    }

    void MainWindow::TopmostButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const bool was_topmost = topmost_;
        topmost_ = TopmostButton().IsChecked().Value();
        set_topmost(topmost_);
        update_state();
        if (!was_topmost && topmost_ && glance::app::load_window_preferences().auto_fit_media)
        {
            show_preview_notice(L"AutoFitPausedNotice");
        }
    }

    void MainWindow::PreviewModeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (content_preview_kind_ == glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            basic_info_mode_ = false;
            update_preview_mode_button();
            return;
        }

        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder ||
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            basic_info_mode_ = false;
            update_preview_mode_button();
            return;
        }

        basic_info_mode_ = PreviewModeButton().IsChecked().Value();
        update_preview_mode_button();
        if (!basic_info_mode_)
        {
            present_file(current_index_);
            return;
        }

        ++content_generation_;
        dismiss_preview_info_bar();
        present_generic(file, false, true);
    }

    void MainWindow::FullscreenButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        set_fullscreen(!fullscreen_);
    }

    void MainWindow::ClosePreviewButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (detached_)
        {
            stop_detached_focus_monitor();
            clear_preview_content();
            Close();
            return;
        }
        pinned_ = false;
        topmost_ = false;
        PinButton().IsChecked(false);
        TopmostButton().IsChecked(false);
        set_topmost(false);
        HidePreview();
    }

    void MainWindow::BackButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        static_cast<void>(NavigateBack());
    }

    void MainWindow::set_markdown_preview_mode(bool preview)
    {
        markdown_preview_ = preview;
        MarkdownPreviewButton().IsChecked(preview);
        MarkdownCodeButton().IsChecked(!preview);
        MarkdownPreviewButton().FontWeight(preview ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        MarkdownCodeButton().FontWeight(preview ? Windows::UI::Text::FontWeights::Normal() : Windows::UI::Text::FontWeights::SemiBold());
        WebPreviewHost().Visibility(
            preview && web_content_ready_ ? Visibility::Visible : Visibility::Collapsed);
        update_text_editor_visibility();
        update_web_view_idle_state();
    }

    void MainWindow::MarkdownPreviewButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!web_preview_available_)
        {
            set_markdown_preview_mode(false);
            return;
        }
        set_markdown_preview_mode(true);
        if (current_text_markdown_ && !current_text_has_more_)
        {
            if (web_view_ready_)
            {
                render_markdown();
            }
            else
            {
                initialize_markdown_web_view_async(content_generation_);
            }
        }
        else if (web_preview_ == nullptr && current_text_web_ && !current_text_path_.empty())
        {
            render_web_document_async(current_text_path_, content_generation_);
        }
    }

    void MainWindow::MarkdownCodeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        set_markdown_preview_mode(false);
    }

    void MainWindow::update_line_number_visibility()
    {
        if (text_editor_ != nullptr)
        {
            text_editor_->set_line_numbers(line_numbers_visible_);
        }
        LineNumbersButton().IsEnabled(true);
        LineNumbersButton().IsChecked(line_numbers_visible_);
        ToolTipService::SetToolTip(
            LineNumbersButton(),
            box_value(glance::app::localize(L"LineNumbersTooltip")));
    }

    void MainWindow::LineNumbersButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        line_numbers_visible_ = LineNumbersButton().IsChecked().Value();
        text_preferences_.line_numbers = line_numbers_visible_;
        glance::app::save_text_preferences(text_preferences_);
        update_line_number_visibility();
    }

    void MainWindow::SyntaxHighlightButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (preview_notice_active_)
        {
            dismiss_preview_info_bar();
        }
        syntax_highlighting_ = SyntaxHighlightButton().IsChecked().Value();
        text_preferences_.syntax_highlighting = syntax_highlighting_;
        glance::app::save_text_preferences(text_preferences_);
        if (text_editor_ != nullptr)
        {
            text_editor_->set_syntax_highlighting(syntax_highlighting_);
        }
    }

    void MainWindow::WordWrapButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        word_wrap_ = WordWrapButton().IsChecked().Value();
        text_preferences_.word_wrap = word_wrap_;
        glance::app::save_text_preferences(text_preferences_);
        update_text_layout();
    }

    void MainWindow::ArchiveHeaderButton_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!archive_preview_is_directory_)
        {
            return;
        }

        const auto button = sender.try_as<Button>();
        const auto tag = button != nullptr
            ? unbox_value_or<hstring>(button.Tag(), L"")
            : hstring{};
        auto field = glance::app::FolderSortField::name;
        if (tag == L"type")
        {
            field = glance::app::FolderSortField::type;
        }
        else if (tag == L"modified")
        {
            field = glance::app::FolderSortField::modified_time;
        }
        else if (tag == L"size")
        {
            field = glance::app::FolderSortField::size;
        }

        if (folder_preview_preferences_.sort_field == field)
        {
            folder_preview_preferences_.ascending = !folder_preview_preferences_.ascending;
        }
        else
        {
            folder_preview_preferences_.sort_field = field;
            folder_preview_preferences_.ascending = true;
        }
        glance::app::save_folder_preview_preferences(folder_preview_preferences_);
        update_archive_header_state();

        if (archive_render_state_ == nullptr)
        {
            return;
        }

        const auto* selected_entry = selected_folder_entry();
        pending_folder_selection_path_ =
            selected_entry == nullptr ? std::wstring{} : selected_entry->path;
        pending_folder_scroll_offset_valid_ = false;
        pending_folder_focus_restore_ = false;
        auto preview = std::move(archive_render_state_->preview);
        archive_render_state_.reset();
        ArchiveEntryTree().RootNodes().Clear();
        FolderEntryList().Items().Clear();
        ArchiveStatusText().Text(glance::app::localize(L"LoadingFolder"));
        apply_archive_preview(std::move(preview), content_generation_);
    }

    void MainWindow::FolderEntryList_DoubleTapped(
        IInspectable const&,
        DoubleTappedRoutedEventArgs const& args)
    {
        if (ActivateSelectedFolderEntry())
        {
            args.Handled(true);
        }
    }

    void MainWindow::PreviewContentHost_DoubleTapped(
        IInspectable const&,
        DoubleTappedRoutedEventArgs const& args)
    {
        if (!glance::app::should_handle_xaml_fullscreen_double_tap(
                args.Handled(),
                WebPreviewHost().Visibility() == Visibility::Visible,
                is_interactive_preview_source(args.OriginalSource())))
        {
            return;
        }
        if (handle_preview_content_double_click())
        {
            args.Handled(true);
        }
    }

    void MainWindow::EncodingOption_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto option = sender.try_as<MenuFlyoutItem>();
        if (!option || current_text_path_.empty())
        {
            return;
        }

        const auto tag = unbox_value_or<hstring>(option.Tag(), L"");
        if (tag == L"utf8")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf8;
        }
        else if (tag == L"utf16_le")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf16_le;
        }
        else if (tag == L"utf16_be")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf16_be;
        }
        else if (tag == L"gb2312")
        {
            current_text_encoding_ = glance::app::TextEncoding::gb2312;
        }
        else if (tag == L"gbk")
        {
            current_text_encoding_ = glance::app::TextEncoding::gbk;
        }
        else if (tag == L"gb18030")
        {
            current_text_encoding_ = glance::app::TextEncoding::gb18030;
        }
        else if (tag == L"big5")
        {
            current_text_encoding_ = glance::app::TextEncoding::big5;
        }
        else if (tag == L"system")
        {
            current_text_encoding_ = glance::app::TextEncoding::system;
        }
        else
        {
            return;
        }

        EncodingSelector().Content(box_value(option.Text()));
        glance::app::cancel_text_preview_read(current_text_reader_);
        current_text_.clear();
        current_text_reader_.reset();
        current_text_has_more_ = false;
        text_chunk_loading_ = true;
        if (text_editor_ != nullptr)
        {
            text_editor_->clear();
        }
        set_text_loading(true);
        const auto generation = ++content_generation_;
        load_text_async(
            current_text_path_,
            current_text_markdown_,
            current_text_web_,
            generation,
            current_text_encoding_);
    }

    void MainWindow::TextEditorHost_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        update_text_editor_bounds();
        update_text_editor_visibility();
    }

    void MainWindow::TextEditorHost_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_text_editor_bounds();
        ensure_text_viewport_filled();
    }

    void MainWindow::show_text_font_size_overlay()
    {
        TextFontSizeOverlayText().Text(glance::app::localize_format(
            L"FontSizeOverlayFormat",
            { std::to_wstring(static_cast<int>(text_preferences_.font_size)) }));
        TextFontSizeOverlay().Visibility(Visibility::Visible);
        queue_text_editor_occlusion_update();

        if (font_size_overlay_timer_ == nullptr)
        {
            font_size_overlay_timer_ = DispatcherTimer();
            font_size_overlay_timer_.Interval(std::chrono::milliseconds(900));
            const auto weak = get_weak();
            font_size_overlay_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->font_size_overlay_timer_.Stop();
                    self->TextFontSizeOverlay().Visibility(Visibility::Collapsed);
                    self->queue_text_editor_occlusion_update();
                }
            });
        }
        font_size_overlay_timer_.Stop();
        font_size_overlay_timer_.Start();
    }

    void MainWindow::ImagePanel_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_image_fit_surface();
        update_image_zoom_map();
    }

    void MainWindow::PdfPanel_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_pdf_fit_surface();
    }

    void MainWindow::update_image_metadata_visibility()
    {
        const bool show = image_metadata_visible_ &&
            ImageMetadataText().Blocks().Size() != 0;
        ImageMetadataOverlay().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        ImageExifButton().IsChecked(image_metadata_visible_);
    }

    void MainWindow::ImageExifButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        image_metadata_visible_ = ImageExifButton().IsChecked().Value();
        update_image_metadata_visibility();
    }

    void MainWindow::ImageExifButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        args.Handled(true);
        copy_text_to_clipboard(
            image_metadata_json_,
            L"Copy EXIF data",
            ImageExifIcon());
    }

    void MainWindow::set_image_zoom(float zoom, Windows::Foundation::Point anchor)
    {
        const float old_zoom = ImageScroller().ZoomFactor();
        const float new_zoom = std::clamp(zoom, 1.0F, 16.0F);
        if (std::abs(new_zoom - old_zoom) < 0.001F)
        {
            return;
        }

        const double horizontal =
            (ImageScroller().HorizontalOffset() + anchor.X) * new_zoom / old_zoom - anchor.X;
        const double vertical =
            (ImageScroller().VerticalOffset() + anchor.Y) * new_zoom / old_zoom - anchor.Y;
        static_cast<void>(ImageScroller().ChangeView(
            std::max(0.0, horizontal),
            std::max(0.0, vertical),
            new_zoom,
            true));
    }

    void MainWindow::update_image_zoom_controls()
    {
        const double zoom = std::clamp(
            static_cast<double>(ImageScroller().ZoomFactor()),
            1.0,
            16.0);
        if (std::abs(ImageZoomSlider().Value() - zoom) >= 0.001)
        {
            ImageZoomSlider().Value(zoom);
        }
        ImageZoomValueText().Text(formatted_zoom_factor(zoom));
        if (std::abs(zoom - 1.0) >= 0.001)
        {
            ImageZoomIcon().Foreground(Application::Current().Resources().Lookup(
                box_value(L"AccentTextFillColorPrimaryBrush")).as<Media::Brush>());
        }
        else
        {
            ImageZoomIcon().ClearValue(IconElement::ForegroundProperty());
        }
    }

    void MainWindow::update_image_transform_controls()
    {
        const auto update_icon = [](const FontIcon& icon, bool active) {
            if (active)
            {
                icon.Foreground(Application::Current().Resources().Lookup(
                    box_value(L"AccentTextFillColorPrimaryBrush")).as<Media::Brush>());
            }
            else
            {
                icon.ClearValue(IconElement::ForegroundProperty());
            }
        };
        update_icon(RotateIcon(), std::abs(image_rotation_) >= 0.001);
        update_icon(FlipIcon(), image_scale_x_ < 0.0 || image_scale_y_ < 0.0);
    }

    void MainWindow::ImageZoomButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        set_image_zoom(
            1.0F,
            Windows::Foundation::Point{
                static_cast<float>(ImageScroller().ActualWidth() / 2.0),
                static_cast<float>(ImageScroller().ActualHeight() / 2.0) });
        args.Handled(true);
    }

    void MainWindow::ImageZoomSlider_ValueChanged(
        IInspectable const&,
        Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        const float zoom = static_cast<float>(args.NewValue());
        ImageZoomValueText().Text(formatted_zoom_factor(zoom));
        if (current_kind_ != glance::app::PreviewKind::image)
        {
            return;
        }
        set_image_zoom(
            zoom,
            Windows::Foundation::Point{
                static_cast<float>(ImageScroller().ActualWidth() / 2.0),
                static_cast<float>(ImageScroller().ActualHeight() / 2.0) });
    }

    void MainWindow::ImageScroller_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(ImageScroller());
        const int delta = point.Properties().MouseWheelDelta();
        if (delta == 0)
        {
            return;
        }
        if (handle_gallery_wheel(delta))
        {
            args.Handled(true);
            return;
        }
        const float factor = std::pow(1.2F, static_cast<float>(delta) / 120.0F);
        set_image_zoom(ImageScroller().ZoomFactor() * factor, point.Position());
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(ImageScroller());
        if (point.Properties().IsMiddleButtonPressed() && middle_click_gallery_enabled_)
        {
            toggle_gallery_mode();
            args.Handled(true);
            return;
        }
        if (!point.Properties().IsLeftButtonPressed() || ImageScroller().ZoomFactor() <= 1.001F)
        {
            return;
        }
        if (!ImageScroller().CapturePointer(args.Pointer()))
        {
            return;
        }
        image_panning_ = true;
        image_pan_start_ = point.Position();
        image_pan_horizontal_offset_ = ImageScroller().HorizontalOffset();
        image_pan_vertical_offset_ = ImageScroller().VerticalOffset();
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerMoved(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (!image_panning_)
        {
            return;
        }
        const auto point = args.GetCurrentPoint(ImageScroller());
        if (!point.Properties().IsLeftButtonPressed())
        {
            end_image_pan(args);
            return;
        }
        const auto position = point.Position();
        static_cast<void>(ImageScroller().ChangeView(
            std::max(0.0, image_pan_horizontal_offset_ + image_pan_start_.X - position.X),
            std::max(0.0, image_pan_vertical_offset_ + image_pan_start_.Y - position.Y),
            nullptr,
            true));
        args.Handled(true);
    }

    void MainWindow::end_image_pan(PointerRoutedEventArgs const& args)
    {
        if (!image_panning_)
        {
            return;
        }
        image_panning_ = false;
        ImageScroller().ReleasePointerCapture(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerReleased(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_pan(args);
    }

    void MainWindow::ImageScroller_PointerCanceled(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_pan(args);
    }

    void MainWindow::ImageScroller_PointerCaptureLost(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        image_panning_ = false;
        args.Handled(true);
    }

    void MainWindow::RotateButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        rotate_image(90.0);
    }

    void MainWindow::RotateButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        rotate_image(-90.0);
        args.Handled(true);
    }

    void MainWindow::ImageScroller_ViewChanged(
        IInspectable const&,
        ScrollViewerViewChangedEventArgs const&)
    {
        update_image_zoom_controls();
        update_image_zoom_map();
    }

    void MainWindow::ImageZoomMapOverlay_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(ImageZoomMapCanvas());
        if (!point.Properties().IsLeftButtonPressed() ||
            !ImageZoomMapOverlay().CapturePointer(args.Pointer()))
        {
            return;
        }

        image_zoom_map_panning_ = true;
        move_image_viewport_from_zoom_map(point.Position());
        args.Handled(true);
    }

    void MainWindow::ImageZoomMapOverlay_PointerMoved(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (!image_zoom_map_panning_)
        {
            return;
        }

        const auto point = args.GetCurrentPoint(ImageZoomMapCanvas());
        if (!point.Properties().IsLeftButtonPressed())
        {
            end_image_zoom_map_pan(args);
            return;
        }

        move_image_viewport_from_zoom_map(point.Position());
        args.Handled(true);
    }

    void MainWindow::end_image_zoom_map_pan(PointerRoutedEventArgs const& args)
    {
        if (!image_zoom_map_panning_)
        {
            return;
        }

        image_zoom_map_panning_ = false;
        ImageZoomMapOverlay().ReleasePointerCapture(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::ImageZoomMapOverlay_PointerReleased(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_zoom_map_pan(args);
    }

    void MainWindow::ImageZoomMapOverlay_PointerCanceled(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_zoom_map_pan(args);
    }

    void MainWindow::ImageZoomMapOverlay_PointerCaptureLost(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        image_zoom_map_panning_ = false;
        args.Handled(true);
    }

    void MainWindow::rotate_image(double degrees)
    {
        image_rotation_ = std::fmod(image_rotation_ + degrees + 360.0, 360.0);
        ImageTransform().Rotation(image_rotation_);
        update_image_transform_controls();
        fit_image_to_viewport();
    }

    void MainWindow::FlipButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        flip_image(true);
    }

    void MainWindow::FlipButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        flip_image(false);
        args.Handled(true);
    }

    void MainWindow::flip_image(bool horizontal)
    {
        const bool swaps_axes =
            static_cast<int>(std::lround(image_rotation_)) % 180 != 0;
        if (horizontal != swaps_axes)
        {
            image_scale_x_ = -image_scale_x_;
            ImageTransform().ScaleX(image_scale_x_);
        }
        else if (active_file_directory_descriptor_.info_fields.empty())
        {
            image_scale_y_ = -image_scale_y_;
            ImageTransform().ScaleY(image_scale_y_);
        }
        update_image_transform_controls();
        update_image_zoom_map();
    }

    void MainWindow::show_media_controls()
    {
        media_controls_idle_ticks_ = 0;
        MediaControlsOverlay().Opacity(1.0);
        MediaControlsOverlay().IsHitTestVisible(true);
    }

    void MainWindow::stop_media_playback()
    {
        media_timer_.Stop();
        if (MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().Pause();
        }
        MediaPreview().Source(nullptr);
        media_playback_item_ = nullptr;
        media_playback_generation_ = 0;
        media_playback_info_.clear();
    }

    void MainWindow::update_media_controls()
    {
        if (MediaPanel().Visibility() != Visibility::Visible || MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        const auto session = player.PlaybackSession();
        const double duration = std::max(0.0, session.NaturalDuration().count() / 10000000.0);
        const double position = std::max(0.0, session.Position().count() / 10000000.0);
        updating_media_position_ = true;
        MediaSeekSlider().Maximum(std::max(1.0, duration));
        MediaSeekSlider().Value(std::min(position, std::max(1.0, duration)));
        updating_media_position_ = false;

        const auto format_duration = [](double seconds) {
            const auto total = static_cast<std::uint64_t>(seconds);
            const auto hours = total / 3600;
            const auto minutes = total / 60 % 60;
            const auto remaining = total % 60;
            std::wostringstream output;
            if (hours > 0)
            {
                output << hours << L':' << std::setfill(L'0') << std::setw(2) << minutes;
            }
            else
            {
                output << minutes;
            }
            output << L':' << std::setfill(L'0') << std::setw(2) << remaining;
            return output.str();
        };
        MediaTimeText().Text(format_duration(position) + L" / " + format_duration(duration));
        const bool playing = session.PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing;
        MediaPlayPauseIcon().Glyph(playing ? L"\xE769" : L"\xE768");
        MediaMuteIcon().Glyph(player.IsMuted() || player.Volume() == 0.0 ? L"\xE74F" : L"\xE767");

        if (!media_is_audio_ && playing && MediaControlsOverlay().Opacity() > 0.0)
        {
            ++media_controls_idle_ticks_;
            if (media_controls_idle_ticks_ >= 10)
            {
                MediaControlsOverlay().Opacity(0.0);
                MediaControlsOverlay().IsHitTestVisible(false);
            }
        }
    }

    void MainWindow::MediaPanel_PointerMoved(IInspectable const&, PointerRoutedEventArgs const&)
    {
        show_media_controls();
    }

    void MainWindow::MediaPanel_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(MediaPanel());
        if (point.Properties().IsMiddleButtonPressed() && middle_click_gallery_enabled_)
        {
            toggle_gallery_mode();
            args.Handled(true);
        }
    }

    void MainWindow::MediaPanel_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (current_kind_ != glance::app::PreviewKind::media)
        {
            return;
        }

        const int delta = args.GetCurrentPoint(MediaPanel()).Properties().MouseWheelDelta();
        if (delta == 0)
        {
            return;
        }
        if (handle_gallery_wheel(delta))
        {
            args.Handled(true);
            return;
        }
        if (MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }

        args.Handled(true);
        const auto player = MediaPreview().MediaPlayer();
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            media_volume_wheel_delta_ += delta;
            const int steps = media_volume_wheel_delta_ / WHEEL_DELTA;
            media_volume_wheel_delta_ %= WHEEL_DELTA;
            if (steps != 0)
            {
                const double volume = std::clamp(
                    MediaVolumeSlider().Value() + steps * 5.0,
                    0.0,
                    100.0);
                MediaVolumeSlider().Value(volume);
                if (volume > 0.0)
                {
                    player.IsMuted(false);
                }
            }
        }
        else
        {
            media_seek_wheel_delta_ += reverse_media_seek_wheel_ ? -delta : delta;
            const int steps = media_seek_wheel_delta_ / WHEEL_DELTA;
            media_seek_wheel_delta_ %= WHEEL_DELTA;
            if (steps != 0)
            {
                const auto session = player.PlaybackSession();
                const double duration = std::max(
                    0.0,
                    session.NaturalDuration().count() / 10000000.0);
                if (duration > 0.0)
                {
                    const double position = std::clamp(
                        session.Position().count() / 10000000.0 + steps * 5.0,
                        0.0,
                        duration);
                    session.Position(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
                        std::chrono::duration<double>(position)));
                    updating_media_position_ = true;
                    MediaSeekSlider().Value(position);
                    updating_media_position_ = false;
                }
            }
        }
        show_media_controls();
        update_media_controls();
    }

    void MainWindow::MediaPlayPauseButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        if (player.PlaybackSession().PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing)
        {
            player.Pause();
        }
        else
        {
            player.Play();
        }
        show_media_controls();
    }

    void MainWindow::MediaMuteButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        player.IsMuted(!player.IsMuted());
        MediaMuteIcon().Glyph(player.IsMuted() ? L"\xE74F" : L"\xE767");
        show_media_controls();
    }

    void MainWindow::GalleryModeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        toggle_gallery_mode();
        GalleryModeButton().IsChecked(gallery_mode_ != GalleryMode::inactive);
    }

    void MainWindow::GalleryModeButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        if (gallery_mode_ == GalleryMode::inactive)
        {
            gallery_same_extension_override_ = true;
            open_gallery();
        }
        else
        {
            leave_gallery(true);
        }
        GalleryModeButton().IsChecked(gallery_mode_ != GalleryMode::inactive);
        args.Handled(true);
    }

    void MainWindow::MediaSeekSlider_ValueChanged(
        IInspectable const&,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        if (!updating_media_position_ && MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().PlaybackSession().Position(
                std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
                    std::chrono::duration<double>(args.NewValue())));
            show_media_controls();
        }
    }

    void MainWindow::MediaVolumeSlider_ValueChanged(
        IInspectable const&,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        if (MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().Volume(args.NewValue() / 100.0);
            show_media_controls();
        }
    }

    void MainWindow::PreviousPdfPageButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (pdf_render_client_ == nullptr || pdf_page_index_ == 0)
        {
            return;
        }
        navigate_to_pdf_page(pdf_page_index_ - 1);
    }

    void MainWindow::NextPdfPageButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (pdf_render_client_ == nullptr || pdf_page_index_ + 1 >= pdf_page_count_)
        {
            return;
        }
        navigate_to_pdf_page(pdf_page_index_ + 1);
    }

    void MainWindow::PdfThumbnailsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        show_pdf_navigation(true);
    }

    void MainWindow::PdfOutlineButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        show_pdf_navigation(false);
    }

    void MainWindow::PdfThumbnailList_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        if (pdf_thumbnail_selection_updating_ || PdfThumbnailList().SelectedIndex() < 0)
        {
            return;
        }
        navigate_to_pdf_page(static_cast<std::uint32_t>(PdfThumbnailList().SelectedIndex()));
    }

    void MainWindow::PdfOutlineEntry_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto button = sender.try_as<Button>();
        if (button == nullptr || button.Tag() == nullptr)
        {
            return;
        }
        navigate_to_pdf_page(unbox_value<std::uint32_t>(button.Tag()));
    }

    void MainWindow::PasswordPromptSubmitButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        submit_password();
    }

    void MainWindow::PasswordPromptInput_KeyDown(IInspectable const&, KeyRoutedEventArgs const& args)
    {
        if (args.Key() == Windows::System::VirtualKey::Enter)
        {
            args.Handled(true);
            submit_password();
        }
    }

    void MainWindow::PreviewErrorInfoBar_Closed(
        InfoBar const&,
        InfoBarClosedEventArgs const&)
    {
        queue_text_editor_occlusion_update();
    }

    void MainWindow::show_pdf_navigation(bool thumbnails)
    {
        const bool show_outline = !thumbnails && !pdf_outline_.empty();
        PdfThumbnailsButton().IsChecked(!show_outline);
        PdfOutlineButton().IsChecked(show_outline);
        PdfThumbnailList().Visibility(show_outline ? Visibility::Collapsed : Visibility::Visible);
        PdfOutlineTree().Visibility(show_outline ? Visibility::Visible : Visibility::Collapsed);
        if (!show_outline)
        {
            sync_pdf_thumbnail_selection();
        }
    }

    void MainWindow::sync_pdf_thumbnail_selection()
    {
        const auto items = PdfThumbnailList().Items();
        if (pdf_page_index_ >= items.Size())
        {
            return;
        }
        pdf_thumbnail_selection_updating_ = true;
        PdfThumbnailList().SelectedIndex(static_cast<std::int32_t>(pdf_page_index_));
        pdf_thumbnail_selection_updating_ = false;
        PdfThumbnailList().ScrollIntoView(items.GetAt(pdf_page_index_));
    }

    void MainWindow::build_pdf_navigation(std::uint64_t generation)
    {
        pdf_thumbnail_selection_updating_ = true;
        PdfThumbnailList().Items().Clear();
        PdfOutlineTree().RootNodes().Clear();
        pdf_thumbnail_images_.clear();
        pdf_thumbnail_images_.reserve(pdf_page_count_);
        pdf_thumbnail_items_built_ = 0;
        pdf_thumbnail_selection_updating_ = false;

        std::vector<TreeViewNode> parents;
        for (const auto& outline : pdf_outline_)
        {
            Button button;
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(HorizontalAlignment::Left);
            button.Background(nullptr);
            button.BorderThickness(Thickness{ 0 });
            button.Padding(Thickness{ 4, 2, 4, 2 });
            button.IsEnabled(outline.page_index >= 0 &&
                static_cast<std::uint32_t>(outline.page_index) < pdf_page_count_);
            if (button.IsEnabled())
            {
                button.Tag(box_value(static_cast<std::uint32_t>(outline.page_index)));
                button.Click({ this, &MainWindow::PdfOutlineEntry_Click });
            }
            TextBlock text;
            text.Text(outline.title);
            text.TextTrimming(TextTrimming::CharacterEllipsis);
            button.Content(text);
            TreeViewNode node;
            node.Content(button);
            const std::size_t depth = std::min<std::size_t>(outline.depth, parents.size());
            if (depth == 0)
            {
                PdfOutlineTree().RootNodes().Append(node);
            }
            else
            {
                parents[depth - 1].Children().Append(node);
            }
            if (parents.size() > depth)
            {
                parents.resize(depth);
            }
            parents.push_back(node);
        }
        PdfOutlineButton().IsEnabled(!pdf_outline_.empty());
        show_pdf_navigation(true);
        append_pdf_thumbnail_batch(generation);
    }

    void MainWindow::append_pdf_thumbnail_batch(std::uint64_t generation)
    {
        if (generation != content_generation_ || pdf_render_client_ == nullptr)
        {
            return;
        }
        constexpr std::uint32_t batch_size = 64;
        const auto end = pdf_thumbnail_items_built_ +
            std::min(batch_size, pdf_page_count_ - pdf_thumbnail_items_built_);
        for (std::uint32_t page = pdf_thumbnail_items_built_; page < end; ++page)
        {
            StackPanel content;
            content.Spacing(4);
            Grid preview;
            FontIcon placeholder;
            placeholder.Glyph(L"\xE8A5");
            placeholder.FontSize(32);
            placeholder.Opacity(0.35);
            placeholder.HorizontalAlignment(HorizontalAlignment::Center);
            placeholder.VerticalAlignment(VerticalAlignment::Center);
            Image image;
            image.Width(176);
            image.Height(132);
            image.Stretch(Microsoft::UI::Xaml::Media::Stretch::Uniform);
            preview.Children().Append(placeholder);
            preview.Children().Append(image);
            Border frame;
            frame.Height(136);
            frame.Padding(Thickness{ 2 });
            frame.Child(preview);
            TextBlock label;
            label.Text(std::to_wstring(page + 1));
            label.FontSize(11);
            label.TextAlignment(TextAlignment::Center);
            content.Children().Append(frame);
            content.Children().Append(label);
            ListViewItem item;
            item.Tag(box_value(page));
            item.Content(content);
            PdfThumbnailList().Items().Append(item);
            pdf_thumbnail_images_.push_back(make_weak(image));
        }
        pdf_thumbnail_items_built_ = end;
        if (end < pdf_page_count_)
        {
            const auto weak = get_weak();
            static_cast<void>(DispatcherQueue().TryEnqueue([weak, generation] {
                if (const auto self = weak.get())
                {
                    self->append_pdf_thumbnail_batch(generation);
                }
            }));
            return;
        }
        sync_pdf_thumbnail_selection();
        load_pdf_thumbnails_async(generation);
    }

    void MainWindow::navigate_to_pdf_page(std::uint32_t page_index)
    {
        if (pdf_render_client_ == nullptr || page_index >= pdf_page_count_ ||
            page_index == pdf_page_index_)
        {
            return;
        }
        pdf_panning_ = false;
        pdf_page_index_ = page_index;
        static_cast<void>(PdfScroller().ChangeView(nullptr, nullptr, 1.0F, true));
        render_pdf_page_async(page_index, content_generation_, true);
    }

    void MainWindow::show_password_prompt(
        PasswordPromptTarget target,
        bool invalid_password)
    {
        password_prompt_target_ = target;
        PasswordPromptTitle().Text(glance::app::localize(L"PasswordPromptTitle"));
        PasswordPromptError().Text(
            invalid_password ? glance::app::localize(L"PasswordIncorrect") : L"");
        PasswordPromptError().Visibility(
            invalid_password ? Visibility::Visible : Visibility::Collapsed);
        PasswordPromptInput().Password(L"");
        set_password_prompt_activation(true);
        PasswordPromptOverlay().Visibility(Visibility::Visible);
        if (GetForegroundWindow() == window_)
        {
            password_prompt_focused_ = true;
            update_state();
            PasswordPromptInput().Focus(FocusState::Programmatic);
        }
    }

    void MainWindow::hide_password_prompt()
    {
        const bool was_focused = password_prompt_focused_;
        password_prompt_target_ = PasswordPromptTarget::none;
        password_prompt_focused_ = false;
        PasswordPromptInput().Password(L"");
        PasswordPromptError().Text(L"");
        PasswordPromptError().Visibility(Visibility::Collapsed);
        PasswordPromptOverlay().Visibility(Visibility::Collapsed);
        set_password_prompt_activation(false);
        if (was_focused)
        {
            update_state();
        }
    }

    void MainWindow::set_password_prompt_activation(bool enabled) noexcept
    {
        if (window_ == nullptr || password_prompt_activation_enabled_ == enabled)
        {
            return;
        }
        auto extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        if (enabled)
        {
            extended_style &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
        }
        else
        {
            extended_style |= WS_EX_NOACTIVATE;
        }
        SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        password_prompt_activation_enabled_ = enabled;
    }

    void MainWindow::submit_password()
    {
        if (PasswordPromptOverlay().Visibility() != Visibility::Visible)
        {
            return;
        }
        const auto target = password_prompt_target_;
        const std::wstring password = PasswordPromptInput().Password().c_str();
        PasswordPromptSubmitButton().IsEnabled(false);
        PasswordPromptError().Visibility(Visibility::Collapsed);
        hide_password_prompt();
        PasswordPromptSubmitButton().IsEnabled(true);
        if (target == PasswordPromptTarget::pdf && !pdf_source_path_.empty())
        {
            PdfLoadingText().Text(glance::app::localize(L"LoadingPdf"));
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_pdf_async(pdf_source_path_, content_generation_, password);
        }
        else if (target == PasswordPromptTarget::archive &&
                 active_component_file_directory_ != nullptr)
        {
            ArchiveStatusText().Text(glance::app::localize(L"LoadingArchive"));
            load_component_file_directory_async(
                active_component_file_directory_,
                password,
                content_generation_);
        }
    }

    void MainWindow::PdfScroller_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            pdf_wheel_delta_ = 0;
            return;
        }
        if (PdfScroller().ZoomFactor() > 1.001F || pdf_render_client_ == nullptr)
        {
            pdf_wheel_delta_ = 0;
            return;
        }

        pdf_wheel_delta_ += args.GetCurrentPoint(PdfFitSurface()).Properties().MouseWheelDelta();
        if (std::abs(pdf_wheel_delta_) >= WHEEL_DELTA)
        {
            if (pdf_wheel_delta_ > 0 && pdf_page_index_ > 0)
            {
                navigate_to_pdf_page(pdf_page_index_ - 1);
            }
            else if (pdf_wheel_delta_ < 0 && pdf_page_index_ + 1 < pdf_page_count_)
            {
                navigate_to_pdf_page(pdf_page_index_ + 1);
            }
            pdf_wheel_delta_ = 0;
        }
        args.Handled(true);
    }

    void MainWindow::PdfScroller_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(PdfScroller());
        if (!point.Properties().IsLeftButtonPressed() ||
            !glance::app::zoom_allows_pan(PdfScroller().ZoomFactor()))
        {
            return;
        }
        if (!PdfScroller().CapturePointer(args.Pointer()))
        {
            return;
        }
        pdf_panning_ = true;
        pdf_pan_start_ = point.Position();
        pdf_pan_horizontal_offset_ = PdfScroller().HorizontalOffset();
        pdf_pan_vertical_offset_ = PdfScroller().VerticalOffset();
        args.Handled(true);
    }

    void MainWindow::PdfScroller_PointerMoved(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (!pdf_panning_)
        {
            return;
        }
        const auto point = args.GetCurrentPoint(PdfScroller());
        if (!point.Properties().IsLeftButtonPressed())
        {
            end_pdf_pan(args);
            return;
        }
        const auto position = point.Position();
        const auto offsets = glance::app::calculate_pan_offsets(
            { pdf_pan_horizontal_offset_, pdf_pan_vertical_offset_ },
            { pdf_pan_start_.X, pdf_pan_start_.Y },
            { position.X, position.Y });
        static_cast<void>(PdfScroller().ChangeView(
            offsets.horizontal,
            offsets.vertical,
            nullptr,
            true));
        args.Handled(true);
    }

    void MainWindow::end_pdf_pan(PointerRoutedEventArgs const& args)
    {
        if (!pdf_panning_)
        {
            return;
        }
        pdf_panning_ = false;
        PdfScroller().ReleasePointerCapture(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::PdfScroller_PointerReleased(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_pdf_pan(args);
    }

    void MainWindow::PdfScroller_PointerCanceled(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_pdf_pan(args);
    }

    void MainWindow::PdfScroller_PointerCaptureLost(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        pdf_panning_ = false;
        args.Handled(true);
    }

    void MainWindow::PinButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const bool pin_requested = PinButton().IsChecked().Value();
        if (detached_ && !pin_requested)
        {
            stop_detached_focus_monitor();
            clear_preview_content();
            Close();
            return;
        }

        pinned_ = pin_requested;
        if (pinned_)
        {
            topmost_ = true;
            TopmostButton().IsChecked(true);
            set_topmost(true);
        }
        update_window_action_visibility();
        update_state();
    }

    void MainWindow::update_window_action_visibility()
    {
        const auto visibility = pinned_ ? Visibility::Collapsed : Visibility::Visible;
        TopmostButton().Visibility(visibility);
        ClosePreviewButton().Visibility(visibility);
    }

    void MainWindow::update_state()
    {
        if (!visible_)
        {
            state_ = glance::contracts::PreviewWindowState::hidden;
        }
        else if (password_prompt_target_ != PasswordPromptTarget::none &&
                 password_prompt_focused_ &&
                 !detached_)
        {
            state_ = glance::contracts::PreviewWindowState::active_interactive;
        }
        else if (detached_)
        {
            state_ = pinned_
                ? glance::contracts::PreviewWindowState::detached_pinned_topmost
                : glance::contracts::PreviewWindowState::detached_unpinned;
        }
        else if (pinned_ && topmost_)
        {
            detached_ = true;
            state_ = glance::contracts::PreviewWindowState::detached_pinned_topmost;
        }
        else if (pinned_)
        {
            state_ = glance::contracts::PreviewWindowState::active_pinned;
        }
        else if (topmost_)
        {
            state_ = glance::contracts::PreviewWindowState::active_topmost;
        }
        else
        {
            state_ = glance::contracts::PreviewWindowState::active_following;
        }

        if (state_callback_)
        {
            state_callback_(instance_id_, state_);
        }
    }

    void MainWindow::set_topmost(bool enabled)
    {
        SetWindowPos(
            window_,
            enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void MainWindow::start_detached_focus_monitor()
    {
        if (focus_timer_ == nullptr)
        {
            focus_timer_ = DispatcherTimer();
            focus_timer_.Interval(std::chrono::milliseconds(100));
            const auto weak = get_weak();
            focus_timer_.Tick([weak](IInspectable const&, IInspectable const&)
            {
                if (const auto self = weak.get();
                    self != nullptr && GetForegroundWindow() != self->foreground_when_unpinned_)
                {
                    self->stop_detached_focus_monitor();
                    self->Close();
                }
            });
        }
        focus_timer_.Start();
    }

    void MainWindow::stop_detached_focus_monitor()
    {
        if (focus_timer_ != nullptr)
        {
            focus_timer_.Stop();
        }
    }

    void MainWindow::FileList_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        const int index = FileList().SelectedIndex();
        if (index >= 0)
        {
            present_file(static_cast<std::uint32_t>(index));
        }
    }

    void MainWindow::CopyPathButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size())
        {
            return;
        }
        try
        {
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            const auto& file = files_[current_index_];
            std::wstring path = file.is_filesystem
                ? file.path
                : file.parsing_name;
            const auto preferences = glance::app::load_path_copy_preferences();
            if (preferences.use_unix_separators)
            {
                std::replace(path.begin(), path.end(), L'\\', L'/');
            }
            if (preferences.quote_path)
            {
                path = L"\"" + path + L"\"";
            }
            package.SetText(path);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            show_copy_feedback(CopyPathIcon());
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Copy path failed: " + std::wstring(error.message()));
        }
    }

    fire_and_forget MainWindow::CopyPathButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        args.Handled(true);
        if (current_index_ >= files_.size())
        {
            co_return;
        }

        const auto lifetime = get_strong();
        const auto path = !files_[current_index_].path.empty()
            ? files_[current_index_].path
            : files_[current_index_].parsing_name;
        const bool is_directory =
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        try
        {
            Windows::Storage::IStorageItem item{ nullptr };
            if (is_directory)
            {
                item = co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(path);
            }
            else
            {
                item = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            }
            auto items = single_threaded_vector<Windows::Storage::IStorageItem>();
            items.Append(std::move(item));

            Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.RequestedOperation(
                Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            package.SetStorageItems(items);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            show_copy_feedback(CopyPathIcon());
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Copy file failed: " + std::wstring(error.message()));
        }
    }

    void MainWindow::show_copy_feedback(const FontIcon& icon)
    {
        if (copy_feedback_icon_ != nullptr)
        {
            copy_feedback_icon_.Glyph(copy_feedback_original_glyph_);
            copy_feedback_icon_.ClearValue(IconElement::ForegroundProperty());
        }
        copy_feedback_icon_ = icon;
        copy_feedback_original_glyph_ = icon.Glyph();
        icon.Glyph(L"\xE73E");
        icon.Foreground(Application::Current().Resources().Lookup(
            box_value(L"AccentTextFillColorPrimaryBrush")).as<Media::Brush>());
        if (copy_feedback_timer_ == nullptr)
        {
            copy_feedback_timer_ = DispatcherTimer();
            copy_feedback_timer_.Interval(std::chrono::milliseconds(1200));
            const auto weak = get_weak();
            copy_feedback_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->copy_feedback_timer_.Stop();
                    if (self->copy_feedback_icon_ != nullptr)
                    {
                        self->copy_feedback_icon_.Glyph(
                            self->copy_feedback_original_glyph_);
                        self->copy_feedback_icon_.ClearValue(
                            IconElement::ForegroundProperty());
                        self->copy_feedback_icon_ = nullptr;
                        self->copy_feedback_original_glyph_.clear();
                    }
                }
            });
        }
        copy_feedback_timer_.Stop();
        copy_feedback_timer_.Start();
    }

    void MainWindow::copy_text_to_clipboard(
        std::wstring_view text,
        std::wstring_view operation,
        const FontIcon& feedback_icon)
    {
        if (text.empty())
        {
            return;
        }
        try
        {
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(hstring(text));
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            show_copy_feedback(feedback_icon);
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                std::wstring(operation) + L" failed: " + std::wstring(error.message()));
        }
    }

    void MainWindow::OpenFolderButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }
        std::wstring parameters = L"/select,\"" + files_[current_index_].path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OpenDefaultButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size())
        {
            return;
        }
        const auto& file = files_[current_index_];
        const auto& target = file.is_filesystem ? file.path : file.parsing_name;
        if (target.empty())
        {
            return;
        }
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OpenDefaultButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        args.Handled(true);
        if (current_index_ >= files_.size() || files_[current_index_].path.empty() ||
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return;
        }
        const OPENASINFO open_as{
            files_[current_index_].path.c_str(),
            nullptr,
            OAIF_ALLOW_REGISTRATION | OAIF_EXEC };
        static_cast<void>(SHOpenWithDialog(window_, &open_as));
    }

    void MainWindow::LoadCloudFileButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }

        const HANDLE file = CreateFileW(
            files_[current_index_].path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            ErrorText().Text(glance::app::localize_format(
                L"CloudLoadError", { std::to_wstring(GetLastError()) }));
            ErrorText().Visibility(Visibility::Visible);
            return;
        }
        CloseHandle(file);
        files_[current_index_].is_cloud_placeholder = false;
        LoadCloudFileButton().Visibility(Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
        present_file(current_index_);
    }

    void MainWindow::PreviewAsTextButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (basic_info_mode_ ||
            !generic_text_preview_allowed_ ||
            current_kind_ != glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            return;
        }

        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder ||
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            file.size > maximum_preview_as_text_bytes ||
            !glance::app::can_try_preview_as_text(file.path))
        {
            return;
        }

        ++content_generation_;
        dismiss_preview_info_bar();
        PreviewAsTextButton().IsEnabled(false);
        ErrorText().Visibility(Visibility::Collapsed);
        load_text_async(
            file.path,
            false,
            false,
            content_generation_,
            glance::app::TextEncoding::automatic,
            true);
    }

    void MainWindow::GenericAdvancedInfoButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_kind_ != glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            return;
        }

        generic_preview_preferences_.show_advanced_info =
            GenericAdvancedInfoButton().IsChecked().Value();
        glance::app::save_generic_preview_preferences(generic_preview_preferences_);
        if (!generic_preview_preferences_.show_advanced_info)
        {
            GenericAdvancedInfoText().Text(L"");
            GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
            return;
        }

        const auto& file = files_[current_index_];
        if (!file.path.empty() && !file.is_cloud_placeholder &&
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            load_generic_file_info_async(file.path, content_generation_);
        }
    }

    std::wstring MainWindow::formatted_size(std::uint64_t size) const
    {
        constexpr std::array<const wchar_t*, 5> units{ L"B", L"KB", L"MB", L"GB", L"TB" };
        double value = static_cast<double>(size);
        std::size_t unit{};
        while (value >= 1024.0 && unit + 1 < units.size())
        {
            value /= 1024.0;
            ++unit;
        }
        std::wostringstream output;
        output.precision(unit == 0 ? 0 : 1);
        output << std::fixed << value << L' ' << units[unit];
        return output.str();
    }

    std::wstring MainWindow::formatted_time(std::uint64_t file_time) const
    {
        FILETIME utc{ static_cast<DWORD>(file_time), static_cast<DWORD>(file_time >> 32U) };
        FILETIME local{};
        SYSTEMTIME system_time{};
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &system_time))
        {
            return glance::app::localize(L"UnknownTime");
        }
        wchar_t date[64]{};
        wchar_t time[64]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &system_time, nullptr, date, 64, nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &system_time, nullptr, time, 64);
        return std::wstring(date) + L" " + time;
    }
}
