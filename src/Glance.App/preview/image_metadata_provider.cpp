#include "pch.h"
#include "image_metadata_provider.h"

#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
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

    bool is_redundant_dimension_property(std::wstring_view name)
    {
        return name == L"System.Image.HorizontalSize" ||
               name == L"System.Image.VerticalSize";
    }

    bool is_low_level_metadata_property(std::wstring_view name)
    {
        return (name.starts_with(L"System.Photo.") && name.ends_with(L"Text")) ||
               name == L"System.Photo.ProgramMode" ||
               name.ends_with(L"Numerator") ||
               name.ends_with(L"Denominator") ||
               name == L"System.Image.Compression" ||
               name == L"System.Photo.MakerNote" ||
               name == L"System.Photo.TranscodedForSync" ||
               name == L"System.GPS.VersionID" ||
               name == L"System.GPS.LatitudeRef" ||
               name == L"System.GPS.LongitudeRef" ||
               name == L"System.GPS.DestLatitudeRef" ||
               name == L"System.GPS.DestLongitudeRef";
    }

    std::wstring fallback_display_name(std::wstring_view canonical_name)
    {
        const auto separator = canonical_name.rfind(L'.');
        return std::wstring(separator == std::wstring_view::npos
            ? canonical_name
            : canonical_name.substr(separator + 1));
    }

    std::wstring property_display_name(
        const PROPERTYKEY& key,
        std::wstring_view canonical_name)
    {
        ComPtr<IPropertyDescription> description;
        if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&description))) &&
            description != nullptr)
        {
            PWSTR raw_name{};
            const HRESULT display_result = description->GetDisplayName(&raw_name);
            if (raw_name != nullptr)
            {
                std::wstring name(raw_name);
                CoTaskMemFree(raw_name);
                if (SUCCEEDED(display_result) && !name.empty())
                {
                    return name;
                }
            }
        }
        return fallback_display_name(canonical_name);
    }

    constexpr std::array camera_properties{
        std::wstring_view{ L"System.Photo.CameraManufacturer" },
        std::wstring_view{ L"System.Photo.CameraModel" },
        std::wstring_view{ L"System.Photo.LensManufacturer" },
        std::wstring_view{ L"System.Photo.LensModel" },
        std::wstring_view{ L"System.Photo.ExposureTime" },
        std::wstring_view{ L"System.Photo.FNumber" },
        std::wstring_view{ L"System.Photo.ISOSpeed" },
        std::wstring_view{ L"System.Photo.FocalLength" },
        std::wstring_view{ L"System.Photo.FocalLengthInFilm" },
        std::wstring_view{ L"System.Photo.ExposureProgram" },
        std::wstring_view{ L"System.Photo.ExposureBias" },
        std::wstring_view{ L"System.Photo.MeteringMode" },
        std::wstring_view{ L"System.Photo.WhiteBalance" },
        std::wstring_view{ L"System.Photo.LightSource" },
        std::wstring_view{ L"System.Photo.Flash" },
        std::wstring_view{ L"System.Photo.DigitalZoom" },
        std::wstring_view{ L"System.Photo.Contrast" },
        std::wstring_view{ L"System.Photo.Saturation" },
        std::wstring_view{ L"System.Photo.Sharpness" },
        std::wstring_view{ L"System.Photo.GainControl" },
    };

    constexpr std::array ordered_properties{
        std::wstring_view{ L"System.Photo.DateTaken" },
        std::wstring_view{ L"System.Photo.CameraManufacturer" },
        std::wstring_view{ L"System.Photo.CameraModel" },
        std::wstring_view{ L"System.Photo.LensManufacturer" },
        std::wstring_view{ L"System.Photo.LensModel" },
        std::wstring_view{ L"System.Photo.ExposureTime" },
        std::wstring_view{ L"System.Photo.FNumber" },
        std::wstring_view{ L"System.Photo.ISOSpeed" },
        std::wstring_view{ L"System.Photo.FocalLength" },
        std::wstring_view{ L"System.Photo.FocalLengthInFilm" },
        std::wstring_view{ L"System.Photo.ExposureProgram" },
        std::wstring_view{ L"System.Photo.ExposureBias" },
        std::wstring_view{ L"System.Photo.MeteringMode" },
        std::wstring_view{ L"System.Photo.WhiteBalance" },
        std::wstring_view{ L"System.Photo.LightSource" },
        std::wstring_view{ L"System.Photo.Flash" },
        std::wstring_view{ L"System.Photo.DigitalZoom" },
        std::wstring_view{ L"System.Photo.Orientation" },
        std::wstring_view{ L"System.Image.Dimensions" },
        std::wstring_view{ L"System.Image.BitDepth" },
        std::wstring_view{ L"System.Image.ColorSpace" },
        std::wstring_view{ L"System.Image.CompressionText" },
        std::wstring_view{ L"System.Image.HorizontalResolution" },
        std::wstring_view{ L"System.Image.VerticalResolution" },
        std::wstring_view{ L"System.Title" },
        std::wstring_view{ L"System.Subject" },
        std::wstring_view{ L"System.Author" },
        std::wstring_view{ L"System.Keywords" },
        std::wstring_view{ L"System.Comment" },
        std::wstring_view{ L"System.Copyright" },
        std::wstring_view{ L"System.Rating" },
    };

    glance::app::ImageMetadataSection metadata_section(std::wstring_view name) noexcept
    {
        using glance::app::ImageMetadataSection;
        if (name == L"System.Photo.EXIFVersion")
        {
            return ImageMetadataSection::details;
        }
        if (name.starts_with(L"System.GPS."))
        {
            return ImageMetadataSection::location;
        }
        if (name.starts_with(L"System.Image.") || name == L"System.Photo.Orientation")
        {
            return ImageMetadataSection::image;
        }
        if (std::ranges::find(camera_properties, name) != camera_properties.end())
        {
            return ImageMetadataSection::camera;
        }
        if (name.starts_with(L"System.Photo."))
        {
            return ImageMetadataSection::capture;
        }
        return ImageMetadataSection::details;
    }

    std::size_t metadata_priority(std::wstring_view name) noexcept
    {
        const auto match = std::ranges::find(ordered_properties, name);
        return match == ordered_properties.end()
            ? ordered_properties.size()
            : static_cast<std::size_t>(match - ordered_properties.begin());
    }

    struct PendingMetadataEntry
    {
        glance::app::ImageMetadataSection section{};
        std::size_t priority{};
        std::wstring canonical_name;
        std::wstring name;
        std::wstring value;
        std::wstring raw_value;
    };

    bool metadata_entry_less(
        const PendingMetadataEntry& left,
        const PendingMetadataEntry& right) noexcept
    {
        if (left.section != right.section)
        {
            return left.section < right.section;
        }
        if (left.priority != right.priority)
        {
            return left.priority < right.priority;
        }
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    }

    bool metadata_entry_less(
        const glance::app::ImageMetadataEntry& left,
        const glance::app::ImageMetadataEntry& right) noexcept
    {
        if (left.section != right.section)
        {
            return left.section < right.section;
        }
        const auto left_priority = metadata_priority(left.canonical_name);
        const auto right_priority = metadata_priority(right.canonical_name);
        if (left_priority != right_priority)
        {
            return left_priority < right_priority;
        }
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    }

    std::wstring format_component_value(
        const PROPERTYKEY& key,
        const glance::contracts::components::ImageMetadataEntry& entry)
    {
        using glance::contracts::components::ImageMetadataValueKind;
        PROPVARIANT value{};
        PropVariantInit(&value);
        HRESULT initialized = E_INVALIDARG;
        switch (entry.value_kind)
        {
        case ImageMetadataValueKind::text:
            initialized = InitPropVariantFromString(entry.text, &value);
            break;
        case ImageMetadataValueKind::unsigned_integer:
            initialized = entry.unsigned_value <= UINT32_MAX
                ? InitPropVariantFromUInt32(
                    static_cast<std::uint32_t>(entry.unsigned_value),
                    &value)
                : InitPropVariantFromUInt64(entry.unsigned_value, &value);
            break;
        case ImageMetadataValueKind::floating_point:
            if (std::isfinite(entry.floating_point))
            {
                initialized = InitPropVariantFromDouble(entry.floating_point, &value);
            }
            break;
        case ImageMetadataValueKind::timestamp:
            initialized = InitPropVariantFromFileTime(&entry.timestamp, &value);
            break;
        }
        if (FAILED(initialized))
        {
            return {};
        }

        PWSTR raw_value{};
        const auto format_result =
            PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &raw_value);
        PropVariantClear(&value);
        if (SUCCEEDED(format_result) && raw_value != nullptr && raw_value[0] != L'\0')
        {
            std::wstring formatted(raw_value);
            CoTaskMemFree(raw_value);
            return formatted;
        }
        CoTaskMemFree(raw_value);
        if (entry.value_kind == ImageMetadataValueKind::text)
        {
            return entry.text;
        }
        if (entry.value_kind == ImageMetadataValueKind::unsigned_integer)
        {
            return std::to_wstring(entry.unsigned_value);
        }
        if (entry.value_kind == ImageMetadataValueKind::floating_point)
        {
            auto formatted = std::to_wstring(entry.floating_point);
            while (formatted.ends_with(L'0'))
            {
                formatted.pop_back();
            }
            if (formatted.ends_with(L'.'))
            {
                formatted.pop_back();
            }
            return formatted;
        }
        return {};
    }

    std::wstring raw_property_value(const PROPVARIANT& value)
    {
        PWSTR raw{};
        if (FAILED(PropVariantToStringAlloc(value, &raw)) || raw == nullptr)
        {
            CoTaskMemFree(raw);
            return {};
        }
        std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }

    std::wstring raw_component_value(
        const glance::contracts::components::ImageMetadataEntry& entry)
    {
        using glance::contracts::components::ImageMetadataValueKind;
        switch (entry.value_kind)
        {
        case ImageMetadataValueKind::text:
        {
            const auto length = wcsnlen_s(entry.text, std::size(entry.text));
            return length == std::size(entry.text)
                ? std::wstring{}
                : std::wstring(entry.text, length);
        }
        case ImageMetadataValueKind::unsigned_integer:
            return std::to_wstring(entry.unsigned_value);
        case ImageMetadataValueKind::floating_point:
        {
            if (!std::isfinite(entry.floating_point))
            {
                return {};
            }
            std::wostringstream output;
            output << std::setprecision(std::numeric_limits<double>::max_digits10)
                   << entry.floating_point;
            return output.str();
        }
        case ImageMetadataValueKind::timestamp:
        {
            SYSTEMTIME value{};
            if (!FileTimeToSystemTime(&entry.timestamp, &value))
            {
                return {};
            }
            wchar_t formatted[32]{};
            swprintf_s(
                formatted,
                L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                value.wYear,
                value.wMonth,
                value.wDay,
                value.wHour,
                value.wMinute,
                value.wSecond,
                value.wMilliseconds);
            return formatted;
        }
        }
        return {};
    }
}

namespace glance::app
{
    ImageMetadata load_image_metadata(const std::wstring& path)
    {
        ImageMetadata result;
        ComPtr<IShellItem2> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            return result;
        }

        ComPtr<IPropertyStore> store;
        if (FAILED(item->GetPropertyStore(GPS_BESTEFFORT, IID_PPV_ARGS(&store))))
        {
            return result;
        }

        DWORD count{};
        if (FAILED(store->GetCount(&count)))
        {
            return result;
        }

        std::vector<PendingMetadataEntry> entries;
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
            if (!is_image_metadata(canonical_name) ||
                is_redundant_dimension_property(canonical_name) ||
                is_low_level_metadata_property(canonical_name))
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
            auto unlocalized_value = raw_property_value(value);
            const HRESULT format_result = PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &raw_value);
            PropVariantClear(&value);
            if (FAILED(format_result) || raw_value == nullptr || raw_value[0] == L'\0')
            {
                CoTaskMemFree(raw_value);
                continue;
            }

            std::wstring formatted_value(raw_value);
            CoTaskMemFree(raw_value);
            if (canonical_name == L"System.Photo.DateTaken")
            {
                result.taken_time = formatted_value;
            }
            entries.push_back(PendingMetadataEntry{
                .section = metadata_section(canonical_name),
                .priority = metadata_priority(canonical_name),
                .canonical_name = canonical_name,
                .name = property_display_name(key, canonical_name),
                .value = std::move(formatted_value),
                .raw_value = std::move(unlocalized_value),
            });
        }

        if (result.taken_time.empty())
        {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Photo_DateTaken, &value)) &&
                value.vt != VT_EMPTY && value.vt != VT_NULL)
            {
                auto unlocalized_value = raw_property_value(value);
                PWSTR raw_value{};
                if (SUCCEEDED(PSFormatForDisplayAlloc(
                        PKEY_Photo_DateTaken,
                        value,
                        PDFF_DEFAULT,
                        &raw_value)) &&
                    raw_value != nullptr && raw_value[0] != L'\0')
                {
                    result.taken_time = raw_value;
                    entries.push_back(PendingMetadataEntry{
                        .section = ImageMetadataSection::capture,
                        .priority = metadata_priority(L"System.Photo.DateTaken"),
                        .canonical_name = L"System.Photo.DateTaken",
                        .name = property_display_name(
                            PKEY_Photo_DateTaken,
                            L"System.Photo.DateTaken"),
                        .value = result.taken_time,
                        .raw_value = std::move(unlocalized_value),
                    });
                }
                CoTaskMemFree(raw_value);
            }
            PropVariantClear(&value);
        }

        std::ranges::sort(entries, [](const auto& left, const auto& right) {
            return metadata_entry_less(left, right);
        });
        result.entries.reserve(entries.size());
        for (auto& entry : entries)
        {
            result.entries.push_back(ImageMetadataEntry{
                .section = entry.section,
                .canonical_name = std::move(entry.canonical_name),
                .name = std::move(entry.name),
                .value = std::move(entry.value),
                .raw_value = std::move(entry.raw_value),
            });
        }
        return result;
    }

    void merge_component_image_metadata(
        ImageMetadata& metadata,
        std::span<const glance::contracts::components::ImageMetadataEntry> entries)
    {
        for (const auto& entry : entries)
        {
            const auto canonical_length =
                wcsnlen_s(entry.canonical_name, std::size(entry.canonical_name));
            if (canonical_length == std::size(entry.canonical_name))
            {
                continue;
            }
            const std::wstring canonical_name(entry.canonical_name, canonical_length);
            if (!is_image_metadata(canonical_name) ||
                is_redundant_dimension_property(canonical_name) ||
                is_low_level_metadata_property(canonical_name) ||
                std::ranges::any_of(metadata.entries, [&](const auto& existing) {
                    return _wcsicmp(
                        existing.canonical_name.c_str(),
                        canonical_name.c_str()) == 0;
                }))
            {
                continue;
            }

            PROPERTYKEY key{};
            if (FAILED(PSGetPropertyKeyFromName(canonical_name.c_str(), &key)))
            {
                continue;
            }
            auto formatted_value = format_component_value(key, entry);
            if (formatted_value.empty())
            {
                continue;
            }
            if (canonical_name == L"System.Photo.DateTaken" && metadata.taken_time.empty())
            {
                metadata.taken_time = formatted_value;
            }
            metadata.entries.push_back(ImageMetadataEntry{
                .section = metadata_section(canonical_name),
                .canonical_name = canonical_name,
                .name = property_display_name(key, canonical_name),
                .value = std::move(formatted_value),
                .raw_value = raw_component_value(entry),
            });
        }
        std::ranges::sort(metadata.entries, [](const auto& left, const auto& right) {
            return metadata_entry_less(left, right);
        });
    }

    std::uint32_t load_image_bit_depth(const std::wstring& path) noexcept
    {
        ComPtr<IShellItem2> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            return 0;
        }

        ULONG bit_depth{};
        return SUCCEEDED(item->GetUInt32(PKEY_Image_BitDepth, &bit_depth))
            ? bit_depth
            : 0;
    }
}
