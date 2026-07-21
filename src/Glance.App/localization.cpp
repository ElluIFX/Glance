#include "pch.h"
#include "localization.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <optional>

namespace
{
    std::filesystem::path resource_file_path()
    {
        std::wstring executable_path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            executable_path.data(),
            static_cast<DWORD>(executable_path.size()));
        executable_path.resize(length);
        return std::filesystem::path(executable_path).parent_path() / L"Glance.pri";
    }

    std::optional<std::wstring> mapped_language(std::wstring_view language)
    {
        std::wstring normalized(language);
        std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value == L'_' ? L'-' : value));
        });
        if (normalized == L"zh" || normalized == L"zh-cn" || normalized == L"zh-sg" ||
            normalized == L"zh-hans" || normalized.starts_with(L"zh-hans-"))
        {
            return L"zh-CN";
        }
        if (normalized == L"en" || normalized.starts_with(L"en-"))
        {
            return L"en-US";
        }
        return std::nullopt;
    }

    class ResourceStore
    {
    public:
        ResourceStore()
            : manager_(resource_file_path().wstring()),
              resources_(manager_.MainResourceMap().GetSubtree(L"Resources")),
              context_(manager_.CreateResourceContext())
        {
        }

        void set_language(std::wstring_view language)
        {
            std::scoped_lock lock(mutex_);
            context_.QualifierValues().Insert(
                winrt::Microsoft::Windows::ApplicationModel::Resources::
                    KnownResourceQualifierName::Language(),
                winrt::hstring(language));
        }

        std::wstring get(std::wstring_view key)
        {
            std::wstring resource_id(key);
            std::ranges::replace(resource_id, L'.', L'/');
            std::scoped_lock lock(mutex_);
            const auto candidate = resources_.TryGetValue(resource_id, context_);
            if (candidate == nullptr)
            {
                return std::wstring(key);
            }
            const auto value = candidate.ValueAsString();
            return value.empty() ? std::wstring(key) : std::wstring(value);
        }

    private:
        std::mutex mutex_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager manager_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceMap resources_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceContext context_;
    };

    ResourceStore& resource_store()
    {
        static ResourceStore store;
        return store;
    }
}

namespace glance::app
{
    std::wstring resolve_ui_language(std::wstring_view saved_language)
    {
        if (!saved_language.empty())
        {
            if (const auto mapped = mapped_language(saved_language))
            {
                return *mapped;
            }
            return L"en-US";
        }

        try
        {
            for (const auto& language : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages())
            {
                if (const auto mapped = mapped_language(language.c_str()))
                {
                    return *mapped;
                }
            }
        }
        catch (const winrt::hresult_error&)
        {
        }
        return L"en-US";
    }

    void apply_ui_language(std::wstring_view language)
    {
        const auto resolved = resolve_ui_language(language);
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
            resolved);
        resource_store().set_language(resolved);
    }

    std::wstring localize(std::wstring_view key)
    {
        try
        {
            return resource_store().get(key);
        }
        catch (const winrt::hresult_error&)
        {
            return std::wstring(key);
        }
    }

    std::wstring localize_format(
        std::wstring_view key,
        std::initializer_list<std::wstring_view> arguments)
    {
        std::wstring result = localize(key);
        std::size_t index{};
        for (const auto argument : arguments)
        {
            const std::wstring token = L"{" + std::to_wstring(index++) + L"}";
            std::size_t position{};
            while ((position = result.find(token, position)) != std::wstring::npos)
            {
                result.replace(position, token.size(), argument);
                position += argument.size();
            }
        }
        return result;
    }
}
