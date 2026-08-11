#include "external_host_provider.h"

#include "glance/contracts/diagnostics.h"
#include "glance/contracts/source_api.h"
#include "../../version.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <utility>

namespace
{
    using namespace glance::contracts::sources;

    struct ModuleDeleter
    {
        void operator()(std::remove_pointer_t<HMODULE>* module) const noexcept
        {
            if (module != nullptr)
            {
                FreeLibrary(module);
            }
        }
    };

    using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

    struct SourceManifest
    {
        std::wstring id;
        std::filesystem::path directory;
        std::wstring entry_point;
        std::vector<std::wstring> process_names;
        std::vector<std::wstring> window_class_prefixes;
    };

    struct LoadedSource
    {
        ~LoadedSource()
        {
            if (api.shutdown != nullptr)
            {
                api.shutdown();
            }
        }

        UniqueModule module;
        SourceApi api;
        SourceRegistration registration;
        std::optional<ItemListApi> item_list;
        std::optional<FocusChangeApi> focus_change;
    };

    struct SourceDescriptor
    {
        SourceManifest manifest;
        std::unique_ptr<LoadedSource> loaded;
        bool load_attempted{};
        std::wstring load_error;
        glance::core::ExternalHostContext last_context;
        std::wstring process_name;
        std::wstring window_class;
    };

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::optional<winrt::Windows::Data::Json::JsonObject> read_json(
        const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }
        const std::string content{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>() };
        if (content.empty())
        {
            return std::nullopt;
        }
        return winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(content));
    }

    std::vector<std::wstring> read_string_array(
        const winrt::Windows::Data::Json::JsonObject& object,
        std::wstring_view name)
    {
        std::vector<std::wstring> values;
        const auto array = object.GetNamedArray(name);
        values.reserve(array.Size());
        for (const auto& value : array)
        {
            std::wstring text(value.GetString());
            if (!text.empty())
            {
                std::ranges::transform(text, text.begin(), [](wchar_t value) {
                    return static_cast<wchar_t>(std::towlower(value));
                });
                values.emplace_back(text);
            }
        }
        return values;
    }

    std::optional<SourceManifest> read_manifest(const std::filesystem::path& path)
    {
        try
        {
            const auto root = read_json(path);
            if (!root || static_cast<std::uint32_t>(root->GetNamedNumber(L"schema_version")) != 1)
            {
                return std::nullopt;
            }
            SourceManifest manifest;
            manifest.id = root->GetNamedString(L"id");
            manifest.entry_point = root->GetNamedString(L"entry_point");
            if (manifest.id.empty() || manifest.entry_point.empty() ||
                std::filesystem::path(manifest.entry_point).filename() != manifest.entry_point ||
                root->GetNamedString(L"architecture") != L"x64")
            {
                return std::nullopt;
            }
            const auto match = root->GetNamedObject(L"match");
            manifest.process_names = read_string_array(match, L"process_names");
            manifest.window_class_prefixes = read_string_array(match, L"window_class_prefixes");
            if (manifest.process_names.empty() || manifest.window_class_prefixes.empty())
            {
                return std::nullopt;
            }
            manifest.directory = path.parent_path();
            return manifest;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<SourceDescriptor> discover_sources()
    {
        std::vector<SourceDescriptor> sources;
        const auto root = executable_directory() / L"sources";
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error))
        {
            if (!iterator->is_directory(error))
            {
                continue;
            }
            const auto manifest = read_manifest(iterator->path() / L"source.json");
            if (manifest)
            {
                SourceDescriptor descriptor;
                descriptor.manifest = *manifest;
                sources.push_back(std::move(descriptor));
            }
            else
            {
                glance::contracts::log_event(
                    L"Ignoring an invalid source manifest in " + iterator->path().wstring() + L".");
            }
        }
        std::ranges::sort(sources, {}, [](const SourceDescriptor& source) {
            return source.manifest.id;
        });
        glance::contracts::log_event(
            L"Discovered " + std::to_wstring(sources.size()) + L" optional source manifest(s).");
        return sources;
    }

    bool equal_insensitive(std::wstring_view left, std::wstring_view right)
    {
        return left.size() == right.size() &&
            _wcsnicmp(left.data(), right.data(), left.size()) == 0;
    }

    bool starts_with_insensitive(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() &&
            _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
    }

    bool matches(const SourceManifest& manifest, const glance::core::ExternalHostContext& context)
    {
        return std::ranges::any_of(manifest.process_names, [&](const auto& name) {
            return equal_insensitive(context.process_name, name);
        }) && std::ranges::any_of(manifest.window_class_prefixes, [&](const auto& prefix) {
            return starts_with_insensitive(context.window_class, prefix);
        });
    }

    SourceHostContext make_api_context(const glance::core::ExternalHostContext& context)
    {
        SourceHostContext result;
        result.root_window = reinterpret_cast<std::uintptr_t>(context.root_window);
        result.focused_window = reinterpret_cast<std::uintptr_t>(context.thread_info.hwndFocus);
        result.caret_window = reinterpret_cast<std::uintptr_t>(context.thread_info.hwndCaret);
        result.process_id = context.process_id;
        result.thread_id = context.thread_id;
        result.gui_thread_flags = context.thread_info.flags;
        result.process_name = context.process_name.data();
        result.window_class = context.window_class.data();
        return result;
    }

    bool valid_optional_api(const ItemListApi& api)
    {
        return api.size >= sizeof(ItemListApi) && api.version == item_list_api_version &&
            api.query_count != nullptr && api.enumerate != nullptr;
    }

    bool valid_optional_api(const FocusChangeApi& api)
    {
        return api.size >= sizeof(FocusChangeApi) && api.version == focus_change_api_version &&
            api.focus != nullptr;
    }

    std::uint64_t effective_capabilities(const LoadedSource& source)
    {
        std::uint64_t capabilities = static_cast<std::uint64_t>(Capability::selection);
        if (source.item_list)
        {
            capabilities |= static_cast<std::uint64_t>(Capability::item_list);
        }
        if (source.focus_change)
        {
            capabilities |= static_cast<std::uint64_t>(Capability::focus_change);
        }
        return capabilities & source.registration.capability_mask;
    }

    LoadedSource* ensure_loaded(SourceDescriptor& descriptor)
    {
        if (descriptor.loaded)
        {
            return descriptor.loaded.get();
        }
        if (descriptor.load_attempted)
        {
            return nullptr;
        }
        descriptor.load_attempted = true;
        const auto library_path = descriptor.manifest.directory / descriptor.manifest.entry_point;
        UniqueModule module(LoadLibraryExW(
            library_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (!module)
        {
            descriptor.load_error = L"Source DLL could not be loaded (Win32 " +
                std::to_wstring(GetLastError()) + L")";
            return nullptr;
        }
        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(module.get(), get_api_export));
        auto source = std::make_unique<LoadedSource>();
        source->module = std::move(module);
        if (get_api == nullptr || !get_api(abi_version, &source->api) ||
            source->api.size < sizeof(SourceApi) || source->api.abi != abi_version ||
            source->api.initialize == nullptr || source->api.query_selection == nullptr ||
            source->api.query_status == nullptr || source->api.query_interface == nullptr ||
            source->api.shutdown == nullptr || !source->api.initialize(&source->registration) ||
            source->registration.size < sizeof(SourceRegistration) ||
            descriptor.manifest.id != source->registration.source_id ||
            std::wstring_view(source->registration.target_app_version) != GLANCE_VERSION_WSTRING)
        {
            descriptor.load_error = L"Source ABI or target version is incompatible";
            return nullptr;
        }

        void* optional{};
        if (source->api.query_interface(
                &item_list_api_id, item_list_api_version, &optional) && optional != nullptr)
        {
            const auto candidate = *static_cast<ItemListApi*>(optional);
            if (valid_optional_api(candidate))
            {
                source->item_list = candidate;
            }
        }
        optional = nullptr;
        if (source->api.query_interface(
                &focus_change_api_id, focus_change_api_version, &optional) && optional != nullptr)
        {
            const auto candidate = *static_cast<FocusChangeApi*>(optional);
            if (valid_optional_api(candidate))
            {
                source->focus_change = candidate;
            }
        }
        source->registration.capability_mask = effective_capabilities(*source);
        descriptor.loaded = std::move(source);
        glance::contracts::log_event(L"Loaded optional source: " + descriptor.manifest.id + L".");
        return descriptor.loaded.get();
    }

    SourceDescriptor* find_source(
        std::vector<SourceDescriptor>& sources,
        std::wstring_view source_id,
        std::uintptr_t source_window)
    {
        const auto found = std::ranges::find(sources, source_id, [](const SourceDescriptor& source) {
            return std::wstring_view(source.manifest.id);
        });
        if (found == sources.end() || !found->loaded ||
            reinterpret_cast<std::uintptr_t>(found->last_context.root_window) != source_window ||
            !IsWindow(found->last_context.root_window))
        {
            return nullptr;
        }
        return &*found;
    }
}

namespace glance::core
{
    struct ExternalHostProviderRegistry::Impl
    {
        std::vector<SourceDescriptor> sources{ discover_sources() };
    };

    ExternalHostProviderRegistry::ExternalHostProviderRegistry() : impl_(std::make_unique<Impl>()) {}
    ExternalHostProviderRegistry::~ExternalHostProviderRegistry() = default;

    std::optional<ExternalHostSelection> ExternalHostProviderRegistry::query(
        const ExternalHostContext& context)
    {
        for (auto& descriptor : impl_->sources)
        {
            if (!matches(descriptor.manifest, context))
            {
                continue;
            }
            auto* source = ensure_loaded(descriptor);
            if (source == nullptr)
            {
                return ExternalHostSelection{};
            }
            descriptor.process_name.assign(context.process_name);
            descriptor.window_class.assign(context.window_class);
            descriptor.last_context = context;
            descriptor.last_context.process_name = descriptor.process_name;
            descriptor.last_context.window_class = descriptor.window_class;

            const auto api_context = make_api_context(descriptor.last_context);
            SourceSelectionResult result;
            if (!source->api.query_selection(&api_context, &result))
            {
                return ExternalHostSelection{};
            }
            ExternalHostSelection selection;
            selection.accepts_hotkey = result.accepts_hotkey != FALSE;
            selection.text_input_active = result.text_input_active != FALSE;
            selection.source_id = descriptor.manifest.id;
            selection.capabilities = result.capability_mask & effective_capabilities(*source);
            selection.filesystem_path = result.filesystem_path;
            return selection;
        }
        return std::nullopt;
    }

    bool ExternalHostProviderRegistry::query_item_count(
        std::wstring_view source_id,
        std::uintptr_t source_window,
        std::uint32_t& item_count,
        std::uint32_t& focused_offset,
        std::uint64_t& focused_item_id)
    {
        auto* descriptor = find_source(impl_->sources, source_id, source_window);
        if (descriptor == nullptr || !descriptor->loaded->item_list)
        {
            return false;
        }
        const auto context = make_api_context(descriptor->last_context);
        return descriptor->loaded->item_list->query_count(
            &context, &item_count, &focused_offset, &focused_item_id) != FALSE;
    }

    std::vector<ExternalHostItem> ExternalHostProviderRegistry::enumerate_items(
        std::wstring_view source_id,
        std::uintptr_t source_window,
        std::uint32_t offset,
        std::uint32_t limit)
    {
        auto* descriptor = find_source(impl_->sources, source_id, source_window);
        if (descriptor == nullptr || !descriptor->loaded->item_list || limit == 0)
        {
            return {};
        }
        std::vector<ExternalHostItem> items;
        items.reserve(limit);
        SourceItemSink sink;
        sink.context = &items;
        sink.append = [](void* context, const SourceItem* item) noexcept -> BOOL {
            if (context == nullptr || item == nullptr || item->filesystem_path == nullptr)
            {
                return FALSE;
            }
            try
            {
                static_cast<std::vector<ExternalHostItem>*>(context)->push_back(
                    ExternalHostItem{ item->item_id, item->filesystem_path });
                return TRUE;
            }
            catch (...)
            {
                return FALSE;
            }
        };
        const auto api_context = make_api_context(descriptor->last_context);
        if (!descriptor->loaded->item_list->enumerate(
                &api_context, offset, limit, &sink))
        {
            return {};
        }
        return items;
    }

    bool ExternalHostProviderRegistry::focus_item(
        std::wstring_view source_id,
        std::uintptr_t source_window,
        std::uint64_t item_id,
        std::wstring_view expected_path)
    {
        auto* descriptor = find_source(impl_->sources, source_id, source_window);
        if (descriptor == nullptr || !descriptor->loaded->focus_change)
        {
            return false;
        }
        const auto api_context = make_api_context(descriptor->last_context);
        const std::wstring path(expected_path);
        return descriptor->loaded->focus_change->focus(
            &api_context, item_id, path.c_str()) != FALSE;
    }

    std::vector<ExternalHostStatus> ExternalHostProviderRegistry::statuses(
        std::wstring_view language_tag)
    {
        std::vector<ExternalHostStatus> statuses;
        statuses.reserve(impl_->sources.size());
        for (auto& descriptor : impl_->sources)
        {
            auto* source = ensure_loaded(descriptor);
            if (source == nullptr)
            {
                glance::contracts::log_event(
                    L"Optional source " + descriptor.manifest.id +
                    L" is unavailable: " + descriptor.load_error + L".");
                statuses.push_back(ExternalHostStatus{
                    descriptor.manifest.id,
                    descriptor.manifest.id,
                    {},
                    static_cast<std::uint32_t>(HealthSeverity::error),
                    1,
                    0 });
                continue;
            }
            SourceStatusResult result;
            const std::wstring language(language_tag);
            if (!source->api.query_status(language.c_str(), &result) ||
                result.display_name[0] == L'\0')
            {
                continue;
            }
            statuses.push_back(ExternalHostStatus{
                descriptor.manifest.id,
                result.display_name,
                result.detail,
                static_cast<std::uint32_t>(result.severity),
                result.code,
                result.capability_mask });
        }
        std::ranges::sort(statuses, {}, &ExternalHostStatus::display_name);
        return statuses;
    }
}
