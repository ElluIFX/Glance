#include "pch.h"
#include "localization.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

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

    class ResourceSource
    {
    public:
        explicit ResourceSource(const std::filesystem::path& path)
            : manager_(path.wstring()),
              resources_(manager_.MainResourceMap().GetSubtree(L"Resources")),
              context_(manager_.CreateResourceContext()),
              fallback_context_(manager_.CreateResourceContext())
        {
            fallback_context_.QualifierValues().Insert(
                winrt::Microsoft::Windows::ApplicationModel::Resources::
                    KnownResourceQualifierName::Language(),
                L"en-US");
        }

        void set_language(std::wstring_view language)
        {
            context_.QualifierValues().Insert(
                winrt::Microsoft::Windows::ApplicationModel::Resources::
                    KnownResourceQualifierName::Language(),
                winrt::hstring(language));
            cache_.clear();
        }

        std::wstring get(std::wstring_view key)
        {
            if (const auto cached = cache_.find(std::wstring(key)); cached != cache_.end())
            {
                return cached->second;
            }
            std::wstring resource_id(key);
            std::ranges::replace(resource_id, L'.', L'/');
            auto candidate = resources_.TryGetValue(resource_id, context_);
            if (candidate == nullptr)
            {
                candidate = resources_.TryGetValue(resource_id, fallback_context_);
            }
            const auto resolved = candidate == nullptr || candidate.ValueAsString().empty()
                ? std::wstring(key)
                : std::wstring(candidate.ValueAsString());
            cache_.emplace(std::wstring(key), resolved);
            return resolved;
        }

    private:
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager manager_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceMap resources_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceContext context_;
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceContext
            fallback_context_;
        std::unordered_map<std::wstring, std::wstring> cache_;
    };

    class ResourceStore
    {
    public:
        ResourceStore()
            : application_(resource_file_path()), language_(L"en-US")
        {
            application_.set_language(language_);
        }

        void set_language(std::wstring_view language)
        {
            std::scoped_lock lock(mutex_);
            language_ = language;
            application_.set_language(language_);
            for (const auto& [id, resources] : components_)
            {
                static_cast<void>(id);
                resources->set_language(language_);
            }
        }

        std::wstring language()
        {
            std::scoped_lock lock(mutex_);
            return language_;
        }

        std::wstring get(std::wstring_view key)
        {
            std::scoped_lock lock(mutex_);
            return application_.get(key);
        }

        bool add_component(
            std::wstring_view component_id,
            const std::filesystem::path& path)
        {
            auto resources = std::make_unique<ResourceSource>(path);
            std::scoped_lock lock(mutex_);
            if (components_.contains(std::wstring(component_id)))
            {
                return false;
            }
            resources->set_language(language_);
            components_.emplace(std::wstring(component_id), std::move(resources));
            return true;
        }

        void remove_component(std::wstring_view component_id)
        {
            std::scoped_lock lock(mutex_);
            components_.erase(std::wstring(component_id));
        }

        std::wstring get_component(
            std::wstring_view component_id,
            std::wstring_view key)
        {
            std::scoped_lock lock(mutex_);
            const auto match = components_.find(std::wstring(component_id));
            return match == components_.end()
                ? std::wstring(key)
                : match->second->get(key);
        }

    private:
        std::mutex mutex_;
        ResourceSource application_;
        std::unordered_map<std::wstring, std::unique_ptr<ResourceSource>> components_;
        std::wstring language_;
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

    std::wstring current_ui_language()
    {
        return resource_store().language();
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

    bool register_component_resources(
        std::wstring_view component_id,
        const std::filesystem::path& resource_path) noexcept
    {
        try
        {
            return !component_id.empty() && resource_path.is_absolute() &&
                resource_store().add_component(component_id, resource_path);
        }
        catch (...)
        {
            return false;
        }
    }

    void unregister_component_resources(std::wstring_view component_id) noexcept
    {
        try
        {
            resource_store().remove_component(component_id);
        }
        catch (...)
        {
        }
    }

    std::wstring localize_component(
        std::wstring_view component_id,
        std::wstring_view key)
    {
        try
        {
            return resource_store().get_component(component_id, key);
        }
        catch (...)
        {
            return std::wstring(key);
        }
    }

}
