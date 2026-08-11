#include "extension_localization.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace
{
    std::filesystem::path resource_path(const void* module_address)
    {
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(module_address),
                &module))
        {
            return {};
        }
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).replace_filename(L"resources.pri");
    }

    std::wstring resolve_language(const wchar_t* language_tag)
    {
        if (language_tag == nullptr || language_tag[0] == L'\0')
        {
            return L"en-US";
        }
        wchar_t resolved[LOCALE_NAME_MAX_LENGTH]{};
        return ResolveLocaleName(language_tag, resolved, std::size(resolved)) == 0
            ? L"en-US"
            : resolved;
    }
}

namespace glance::extensions
{
    ResourceStore::~ResourceStore()
    {
        shutdown();
    }

    bool ResourceStore::initialize() noexcept
    {
        try
        {
            std::scoped_lock lock(mutex_);
            if (manager_ != nullptr)
            {
                return true;
            }
            const auto path = resource_path(this);
            MrmManagerHandle manager{};
            MrmContextHandle context{};
            MrmMapHandle resources{};
            if (path.empty() || FAILED(MrmCreateResourceManager(path.c_str(), &manager)) ||
                FAILED(MrmCreateResourceContext(manager, &context)) ||
                FAILED(MrmGetChildResourceMap(manager, nullptr, L"Resources", &resources)))
            {
                if (context != nullptr)
                {
                    MrmDestroyResourceContext(context);
                }
                if (manager != nullptr)
                {
                    MrmDestroyResourceManager(manager);
                }
                return false;
            }
            manager_ = manager;
            context_ = context;
            resources_ = resources;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void ResourceStore::shutdown() noexcept
    {
        std::scoped_lock lock(mutex_);
        resources_ = nullptr;
        if (context_ != nullptr)
        {
            MrmDestroyResourceContext(context_);
            context_ = nullptr;
        }
        if (manager_ != nullptr)
        {
            MrmDestroyResourceManager(manager_);
            manager_ = nullptr;
        }
    }

    bool ResourceStore::copy(
        std::wstring_view key,
        const wchar_t* language_tag,
        wchar_t* destination,
        std::size_t capacity) noexcept
    {
        if (destination == nullptr || capacity == 0)
        {
            return false;
        }
        destination[0] = L'\0';
        try
        {
            std::wstring resource_id(key);
            std::ranges::replace(resource_id, L'.', L'/');
            const auto language = resolve_language(language_tag);
            std::scoped_lock lock(mutex_);
            if (manager_ == nullptr ||
                FAILED(MrmSetQualifier(context_, L"Language", language.c_str())))
            {
                return false;
            }
            PWSTR value{};
            HRESULT result = MrmLoadStringResource(
                manager_, context_, resources_, resource_id.c_str(), &value);
            if (FAILED(result) && language != L"en-US" &&
                SUCCEEDED(MrmSetQualifier(context_, L"Language", L"en-US")))
            {
                result = MrmLoadStringResource(
                    manager_, context_, resources_, resource_id.c_str(), &value);
            }
            if (FAILED(result) || value == nullptr)
            {
                return false;
            }
            const std::size_t length = std::wcslen(value);
            const bool fits = length < capacity;
            if (fits)
            {
                std::copy_n(value, length + 1, destination);
            }
            MrmFreeResource(value);
            return fits;
        }
        catch (...)
        {
            return false;
        }
    }
}
