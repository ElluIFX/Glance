#include "glance/contracts/component_api.h"
#include "glance/contracts/source_api.h"
#include <array>
#include <algorithm>
#include <filesystem>
#include <cwchar>
#include <iostream>
#include <string>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>

int run_component_metadata_tests()
{
    using namespace glance::contracts::components;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    const auto root = std::filesystem::path(executable).parent_path() / L"components";
    int failures{}, checked{};
    for (const auto& directory : std::filesystem::directory_iterator(root))
    {
        if (!directory.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(directory))
        {
            if (!file.path().filename().wstring().ends_with(L"Component.dll")) continue;
            ++checked;
            const HMODULE module = LoadLibraryExW(file.path().c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            const auto get_api = module ? reinterpret_cast<GetApiFunction>(GetProcAddress(module, get_api_export)) : nullptr;
            ComponentApi api;
            bool valid = get_api && get_api(abi_version, &api) && api.query_interface;
            void* pointer{};
            if (valid)
            {
                valid = api.query_interface(&component_metadata_api_id, component_metadata_api_version, &pointer) && pointer;
                if (valid)
                {
                    const auto& metadata = *static_cast<const ComponentMetadataApi*>(pointer);
                    valid = metadata.size >= sizeof(metadata) && metadata.version == component_metadata_api_version;
                    for (const auto* key : {metadata.summary_key, metadata.capabilities_key, metadata.dependencies_key})
                        valid = valid && key[0] != L'\0' && wmemchr(key, L'\0', resource_key_capacity);
                    if (valid)
                    {
                        try
                        {
                            using namespace winrt::Microsoft::Windows::ApplicationModel::Resources;
                            ResourceManager manager((directory.path() / L"resources.pri").wstring());
                            const auto resources = manager.MainResourceMap().GetSubtree(L"Resources");
                            for (const auto* language : {L"en-US", L"zh-CN"})
                            {
                                auto context = manager.CreateResourceContext();
                                context.QualifierValues().Insert(KnownResourceQualifierName::Language(), language);
                                for (const auto* key : {metadata.summary_key, metadata.capabilities_key, metadata.dependencies_key})
                                {
                                    std::wstring resource_id(key);
                                    std::replace(resource_id.begin(), resource_id.end(), L'.', L'/');
                                    const auto candidate = resources.TryGetValue(resource_id, context);
                                    bool translated = false;
                                    if (candidate && !candidate.ValueAsString().empty())
                                        for (const auto& qualifier : candidate.QualifierValues())
                                        {
                                            if (CompareStringOrdinal(qualifier.Key().c_str(), -1, L"Language", -1, TRUE) == CSTR_EQUAL &&
                                                CompareStringOrdinal(qualifier.Value().c_str(), -1, language, -1, TRUE) == CSTR_EQUAL)
                                                translated = true;
                                        }
                                    valid = valid && translated;
                                }
                            }
                        }
                        catch (const winrt::hresult_error& error)
                        {
                            std::wcout << L"Resource lookup failed: " << error.message().c_str() << std::endl;
                            valid = false;
                        }
                    }
                }
                pointer = reinterpret_cast<void*>(1);
                valid = valid && !api.query_interface(&component_metadata_api_id, component_metadata_api_version + 1, &pointer) && !pointer;
                ComponentApi obsolete;
                valid = valid && !get_api(abi_version - 1, &obsolete);
            }
            std::wcout << (valid ? L"PASS " : L"FAIL ") << directory.path().filename().wstring() << L" metadata ABI and bilingual resources" << std::endl;
            if (!valid) ++failures;
            if (module) FreeLibrary(module);
        }
    }
    const auto source_root = std::filesystem::path(executable).parent_path() / L"sources";
    for (const auto& directory : std::filesystem::directory_iterator(source_root))
    {
        if (!directory.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(directory))
        {
            if (!file.path().filename().wstring().ends_with(L"Source.dll")) continue;
            ++checked;
            namespace sources = glance::contracts::sources;
            const HMODULE module = LoadLibraryExW(file.path().c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            const auto get_api = module ? reinterpret_cast<sources::GetApiFunction>(GetProcAddress(module, sources::get_api_export)) : nullptr;
            sources::SourceApi api;
            sources::SourceRegistration registration;
            bool valid = get_api && get_api(sources::abi_version, &api) && api.query_interface && api.initialize &&
                api.initialize(&registration);
            void* pointer{};
            if (valid)
            {
                valid = api.query_interface(&sources::source_metadata_api_id, sources::source_metadata_api_version, &pointer) && pointer;
                if (valid)
                {
                    const auto& metadata = *static_cast<const sources::SourceMetadataApi*>(pointer);
                    valid = metadata.size >= sizeof(metadata) && metadata.version == sources::source_metadata_api_version && metadata.query;
                    if (valid)
                    {
                        try
                        {
                            using namespace winrt::Microsoft::Windows::ApplicationModel::Resources;
                            ResourceManager manager((directory.path() / L"resources.pri").wstring());
                            const auto resources = manager.MainResourceMap().GetSubtree(L"Resources");
                            for (const auto* language : {L"en-US", L"zh-CN"})
                            {
                                sources::SourceMetadataResult result;
                                valid = valid && metadata.query(language, &result);
                                auto context = manager.CreateResourceContext();
                                context.QualifierValues().Insert(KnownResourceQualifierName::Language(), language);
                                const std::array values{result.summary, result.capabilities, result.dependencies};
                                const std::array keys{L"SourceMeta/Summary", L"SourceMeta/Capabilities", L"SourceMeta/Dependencies"};
                                for (std::size_t index = 0; index < keys.size(); ++index)
                                {
                                    const auto candidate = resources.TryGetValue(keys[index], context);
                                    bool translated = false;
                                    if (candidate)
                                        for (const auto& qualifier : candidate.QualifierValues())
                                            if (CompareStringOrdinal(qualifier.Key().c_str(), -1, L"Language", -1, TRUE) == CSTR_EQUAL &&
                                                CompareStringOrdinal(qualifier.Value().c_str(), -1, language, -1, TRUE) == CSTR_EQUAL)
                                                translated = true;
                                    const bool terminated = wmemchr(values[index], L'\0', sources::metadata_text_capacity) != nullptr;
                                    valid = valid && terminated && values[index][0] != L'\0' && translated && candidate.ValueAsString() == values[index];
                                }
                            }
                            sources::SourceMetadataResult invalid;
                            invalid.size = 0;
                            valid = valid && !metadata.query(L"en-US", &invalid);
                        }
                        catch (const winrt::hresult_error& error)
                        {
                            std::wcout << L"Source resource lookup failed: " << error.message().c_str() << std::endl;
                            valid = false;
                        }
                    }
                }
                pointer = reinterpret_cast<void*>(1);
                valid = valid && !api.query_interface(&sources::source_metadata_api_id, sources::source_metadata_api_version + 1, &pointer) && !pointer;
            }
            std::wcout << (valid ? L"PASS " : L"FAIL ") << directory.path().filename().wstring() << L" source metadata ABI and bilingual resources" << std::endl;
            if (!valid) ++failures;
            if (api.shutdown) api.shutdown();
            if (module) FreeLibrary(module);
        }
    }
    if (checked == 0) { std::cout << "FAIL no add-ons tested" << std::endl; ++failures; }
    return failures == 0 ? 0 : 1;
}
