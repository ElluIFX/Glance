#include "pch.h"
#include "image_metadata_provider.h"

#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    bool is_image_metadata(std::wstring_view name)
    {
        return name.starts_with(L"System.Photo.") ||
               name.starts_with(L"System.GPS.") ||
               name.starts_with(L"System.Image.") ||
               name == L"System.Author" ||
               name == L"System.Comment" ||
               name == L"System.Copyright" ||
               name == L"System.Keywords" ||
               name == L"System.Rating" ||
               name == L"System.Subject" ||
               name == L"System.Title";
    }

    bool is_dimension_property(std::wstring_view name)
    {
        return name == L"System.Image.Dimensions" ||
               name == L"System.Image.HorizontalSize" ||
               name == L"System.Image.VerticalSize";
    }

    std::wstring display_name(std::wstring_view canonical_name)
    {
        constexpr std::wstring_view prefix = L"System.";
        return canonical_name.starts_with(prefix)
            ? std::wstring(canonical_name.substr(prefix.size()))
            : std::wstring(canonical_name);
    }
}

namespace glance::app
{
    std::wstring load_image_metadata(const std::wstring& path)
    {
        ComPtr<IShellItem2> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            return {};
        }

        ComPtr<IPropertyStore> store;
        if (FAILED(item->GetPropertyStore(GPS_BESTEFFORT, IID_PPV_ARGS(&store))))
        {
            return {};
        }

        DWORD count{};
        if (FAILED(store->GetCount(&count)))
        {
            return {};
        }

        std::vector<std::pair<std::wstring, std::wstring>> entries;
        entries.reserve(count);
        for (DWORD index = 0; index < count; ++index)
        {
            PROPERTYKEY key{};
            if (FAILED(store->GetAt(index, &key)))
            {
                continue;
            }

            PWSTR raw_name{};
            if (FAILED(PSGetNameFromPropertyKey(key, &raw_name)) || raw_name == nullptr)
            {
                continue;
            }
            const std::wstring canonical_name(raw_name);
            CoTaskMemFree(raw_name);
            if (!is_image_metadata(canonical_name) || is_dimension_property(canonical_name))
            {
                continue;
            }

            PROPVARIANT value{};
            PropVariantInit(&value);
            if (FAILED(store->GetValue(key, &value)) || value.vt == VT_EMPTY || value.vt == VT_NULL)
            {
                PropVariantClear(&value);
                continue;
            }

            PWSTR raw_value{};
            const HRESULT format_result = PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &raw_value);
            PropVariantClear(&value);
            if (FAILED(format_result) || raw_value == nullptr || raw_value[0] == L'\0')
            {
                CoTaskMemFree(raw_value);
                continue;
            }

            entries.emplace_back(display_name(canonical_name), std::wstring(raw_value));
            CoTaskMemFree(raw_value);
        }

        std::ranges::sort(entries, {}, &std::pair<std::wstring, std::wstring>::first);
        std::wstring result;
        for (const auto& [name, value] : entries)
        {
            if (!result.empty())
            {
                result.push_back(L'\n');
            }
            result.append(name).append(L": ").append(value);
        }
        return result;
    }
}
