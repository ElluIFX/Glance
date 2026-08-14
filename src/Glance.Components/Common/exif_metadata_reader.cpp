#include "exif_metadata_reader.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    using glance::contracts::components::ImageMetadataEntry;
    using glance::contracts::components::ImageMetadataValueKind;

    constexpr std::uint16_t type_ascii = 2;
    constexpr std::uint16_t type_short = 3;
    constexpr std::uint16_t type_long = 4;
    constexpr std::uint16_t type_rational = 5;
    constexpr std::uint16_t type_signed_long = 9;
    constexpr std::uint16_t type_signed_rational = 10;
    constexpr std::size_t maximum_ifd_entries = 512;

    struct IfdEntry
    {
        std::uint16_t tag{};
        std::uint16_t type{};
        std::uint32_t count{};
        std::span<const std::uint8_t> data;
    };

    struct Reader
    {
        std::span<const std::uint8_t> tiff;
        bool little_endian{};
        std::vector<ImageMetadataEntry> entries;
        std::optional<std::uint32_t> exif_ifd;
        std::optional<std::uint32_t> gps_ifd;
        std::optional<std::uint32_t> width;
        std::optional<std::uint32_t> height;
        std::array<double, 3> latitude{};
        std::array<double, 3> longitude{};
        bool has_latitude{};
        bool has_longitude{};
        wchar_t latitude_ref{};
        wchar_t longitude_ref{};
        std::wstring fallback_date;

        [[nodiscard]] std::optional<std::uint16_t> u16(std::size_t offset) const noexcept
        {
            if (offset > tiff.size() || tiff.size() - offset < 2)
            {
                return std::nullopt;
            }
            if (little_endian)
            {
                return static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(tiff[offset]) |
                    static_cast<std::uint16_t>(tiff[offset + 1]) << 8U);
            }
            return static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(tiff[offset]) << 8U |
                static_cast<std::uint16_t>(tiff[offset + 1]));
        }

        [[nodiscard]] std::optional<std::uint32_t> u32(std::size_t offset) const noexcept
        {
            if (offset > tiff.size() || tiff.size() - offset < 4)
            {
                return std::nullopt;
            }
            if (little_endian)
            {
                return static_cast<std::uint32_t>(tiff[offset]) |
                    static_cast<std::uint32_t>(tiff[offset + 1]) << 8U |
                    static_cast<std::uint32_t>(tiff[offset + 2]) << 16U |
                    static_cast<std::uint32_t>(tiff[offset + 3]) << 24U;
            }
            return static_cast<std::uint32_t>(tiff[offset]) << 24U |
                static_cast<std::uint32_t>(tiff[offset + 1]) << 16U |
                static_cast<std::uint32_t>(tiff[offset + 2]) << 8U |
                static_cast<std::uint32_t>(tiff[offset + 3]);
        }

        [[nodiscard]] std::optional<IfdEntry> entry(std::size_t offset) const noexcept
        {
            const auto tag = u16(offset);
            const auto type = u16(offset + 2);
            const auto count = u32(offset + 4);
            if (!tag.has_value() || !type.has_value() || !count.has_value())
            {
                return std::nullopt;
            }
            std::size_t element_size{};
            switch (*type)
            {
            case 1:
            case type_ascii:
            case 6:
            case 7:
                element_size = 1;
                break;
            case type_short:
            case 8:
                element_size = 2;
                break;
            case type_long:
            case type_signed_long:
            case 11:
                element_size = 4;
                break;
            case type_rational:
            case type_signed_rational:
            case 12:
                element_size = 8;
                break;
            default:
                return std::nullopt;
            }
            if (*count == 0 || element_size > std::numeric_limits<std::size_t>::max() / *count)
            {
                return std::nullopt;
            }
            const auto byte_count = element_size * *count;
            std::size_t value_offset = offset + 8;
            if (byte_count > 4)
            {
                const auto indirect_offset = u32(value_offset);
                if (!indirect_offset.has_value())
                {
                    return std::nullopt;
                }
                value_offset = *indirect_offset;
            }
            if (value_offset > tiff.size() || tiff.size() - value_offset < byte_count)
            {
                return std::nullopt;
            }
            return IfdEntry{
                .tag = *tag,
                .type = *type,
                .count = *count,
                .data = tiff.subspan(value_offset, byte_count) };
        }

        [[nodiscard]] std::optional<std::uint32_t> unsigned_value(
            const IfdEntry& value,
            std::size_t index = 0) const noexcept
        {
            if (index >= value.count)
            {
                return std::nullopt;
            }
            if (value.type == type_short)
            {
                const auto offset = static_cast<std::size_t>(value.data.data() - tiff.data()) +
                    index * 2;
                return u16(offset);
            }
            if (value.type == type_long)
            {
                const auto offset = static_cast<std::size_t>(value.data.data() - tiff.data()) +
                    index * 4;
                return u32(offset);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<double> rational_value(
            const IfdEntry& value,
            std::size_t index = 0) const noexcept
        {
            if (index >= value.count ||
                (value.type != type_rational && value.type != type_signed_rational))
            {
                return std::nullopt;
            }
            const auto offset = static_cast<std::size_t>(value.data.data() - tiff.data()) +
                index * 8;
            const auto numerator = u32(offset);
            const auto denominator = u32(offset + 4);
            if (!numerator.has_value() || !denominator.has_value() || *denominator == 0)
            {
                return std::nullopt;
            }
            if (value.type == type_signed_rational)
            {
                return static_cast<double>(static_cast<std::int32_t>(*numerator)) /
                    static_cast<double>(static_cast<std::int32_t>(*denominator));
            }
            return static_cast<double>(*numerator) / static_cast<double>(*denominator);
        }

        [[nodiscard]] std::wstring ascii_value(const IfdEntry& value) const
        {
            if (value.type != type_ascii || value.data.empty())
            {
                return {};
            }
            std::size_t length{};
            while (length < value.data.size() && value.data[length] != 0)
            {
                ++length;
            }
            while (length != 0 && value.data[length - 1] == ' ')
            {
                --length;
            }
            if (length == 0)
            {
                return {};
            }
            const int required = MultiByteToWideChar(
                CP_ACP,
                0,
                reinterpret_cast<const char*>(value.data.data()),
                static_cast<int>(length),
                nullptr,
                0);
            if (required <= 0)
            {
                return {};
            }
            std::wstring result(static_cast<std::size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_ACP,
                    0,
                    reinterpret_cast<const char*>(value.data.data()),
                    static_cast<int>(length),
                    result.data(),
                    required) != required)
            {
                return {};
            }
            return result;
        }

        bool contains(std::wstring_view canonical_name) const noexcept
        {
            return std::ranges::any_of(entries, [&](const auto& existing) {
                return std::wstring_view(existing.canonical_name) == canonical_name;
            });
        }

        void add_text(std::wstring_view canonical_name, std::wstring_view text)
        {
            if (text.empty() || contains(canonical_name) ||
                canonical_name.size() >= glance::contracts::components::image_metadata_property_capacity ||
                text.size() >= glance::contracts::components::image_metadata_text_capacity)
            {
                return;
            }
            ImageMetadataEntry result;
            result.value_kind = ImageMetadataValueKind::text;
            canonical_name.copy(result.canonical_name, canonical_name.size());
            text.copy(result.text, text.size());
            entries.push_back(result);
        }

        void add_unsigned(std::wstring_view canonical_name, std::uint64_t value)
        {
            if (contains(canonical_name) ||
                canonical_name.size() >= glance::contracts::components::image_metadata_property_capacity)
            {
                return;
            }
            ImageMetadataEntry result;
            result.value_kind = ImageMetadataValueKind::unsigned_integer;
            result.unsigned_value = value;
            canonical_name.copy(result.canonical_name, canonical_name.size());
            entries.push_back(result);
        }

        void add_double(std::wstring_view canonical_name, double value)
        {
            if (!std::isfinite(value) || contains(canonical_name) ||
                canonical_name.size() >= glance::contracts::components::image_metadata_property_capacity)
            {
                return;
            }
            ImageMetadataEntry result;
            result.value_kind = ImageMetadataValueKind::floating_point;
            result.floating_point = value;
            canonical_name.copy(result.canonical_name, canonical_name.size());
            entries.push_back(result);
        }

        void add_ascii(
            std::wstring_view canonical_name,
            const IfdEntry& value)
        {
            add_text(canonical_name, ascii_value(value));
        }

        void add_date(const IfdEntry& value)
        {
            auto text = ascii_value(value);
            if (text.size() >= 10 && text[4] == L':' && text[7] == L':')
            {
                text[4] = L'-';
                text[7] = L'-';
            }
            add_text(L"System.Photo.DateTaken", text);
        }

        void parse_primary_entry(const IfdEntry& value)
        {
            switch (value.tag)
            {
            case 0x010f:
                add_ascii(L"System.Photo.CameraManufacturer", value);
                break;
            case 0x0110:
                add_ascii(L"System.Photo.CameraModel", value);
                break;
            case 0x0112:
                if (const auto number = unsigned_value(value))
                {
                    add_unsigned(L"System.Photo.Orientation", *number);
                }
                break;
            case 0x0132:
                fallback_date = ascii_value(value);
                if (fallback_date.size() >= 10 &&
                    fallback_date[4] == L':' && fallback_date[7] == L':')
                {
                    fallback_date[4] = L'-';
                    fallback_date[7] = L'-';
                }
                break;
            case 0x013b:
                add_ascii(L"System.Author", value);
                break;
            case 0x8298:
                add_ascii(L"System.Copyright", value);
                break;
            case 0x8769:
                exif_ifd = unsigned_value(value);
                break;
            case 0x8825:
                gps_ifd = unsigned_value(value);
                break;
            }
        }

        void parse_exif_entry(const IfdEntry& value)
        {
            const auto add_rational = [&](std::wstring_view canonical_name) {
                if (const auto number = rational_value(value))
                {
                    add_double(canonical_name, *number);
                }
            };
            const auto add_integer = [&](std::wstring_view canonical_name) {
                if (const auto number = unsigned_value(value))
                {
                    add_unsigned(canonical_name, *number);
                }
            };
            switch (value.tag)
            {
            case 0x829a:
                add_rational(L"System.Photo.ExposureTime");
                break;
            case 0x829d:
                add_rational(L"System.Photo.FNumber");
                break;
            case 0x8822:
                add_integer(L"System.Photo.ExposureProgram");
                break;
            case 0x8827:
                add_integer(L"System.Photo.ISOSpeed");
                break;
            case 0x9003:
            case 0x9004:
                add_date(value);
                break;
            case 0x9204:
                add_rational(L"System.Photo.ExposureBias");
                break;
            case 0x9207:
                add_integer(L"System.Photo.MeteringMode");
                break;
            case 0x9208:
                add_integer(L"System.Photo.LightSource");
                break;
            case 0x9209:
                add_integer(L"System.Photo.Flash");
                break;
            case 0x920a:
                add_rational(L"System.Photo.FocalLength");
                break;
            case 0xa001:
                add_integer(L"System.Image.ColorSpace");
                break;
            case 0xa002:
                width = unsigned_value(value);
                break;
            case 0xa003:
                height = unsigned_value(value);
                break;
            case 0xa403:
                add_integer(L"System.Photo.WhiteBalance");
                break;
            case 0xa404:
                add_rational(L"System.Photo.DigitalZoom");
                break;
            case 0xa405:
                add_integer(L"System.Photo.FocalLengthInFilm");
                break;
            case 0xa408:
                add_integer(L"System.Photo.Contrast");
                break;
            case 0xa409:
                add_integer(L"System.Photo.Saturation");
                break;
            case 0xa40a:
                add_integer(L"System.Photo.Sharpness");
                break;
            case 0xa433:
                add_ascii(L"System.Photo.LensManufacturer", value);
                break;
            case 0xa434:
                add_ascii(L"System.Photo.LensModel", value);
                break;
            }
        }

        void parse_gps_entry(const IfdEntry& value)
        {
            switch (value.tag)
            {
            case 1:
            {
                const auto text = ascii_value(value);
                latitude_ref = text.empty() ? 0 : text.front();
                break;
            }
            case 2:
                if (value.count >= 3)
                {
                    const auto degrees = rational_value(value, 0);
                    const auto minutes = rational_value(value, 1);
                    const auto seconds = rational_value(value, 2);
                    if (degrees && minutes && seconds)
                    {
                        latitude = { *degrees, *minutes, *seconds };
                        has_latitude = true;
                    }
                }
                break;
            case 3:
            {
                const auto text = ascii_value(value);
                longitude_ref = text.empty() ? 0 : text.front();
                break;
            }
            case 4:
                if (value.count >= 3)
                {
                    const auto degrees = rational_value(value, 0);
                    const auto minutes = rational_value(value, 1);
                    const auto seconds = rational_value(value, 2);
                    if (degrees && minutes && seconds)
                    {
                        longitude = { *degrees, *minutes, *seconds };
                        has_longitude = true;
                    }
                }
                break;
            case 6:
                if (const auto altitude = rational_value(value))
                {
                    add_double(L"System.GPS.Altitude", *altitude);
                }
                break;
            }
        }

        template <typename Callback>
        void parse_ifd(std::uint32_t offset, Callback&& callback)
        {
            const auto count = u16(offset);
            if (!count.has_value() || *count > maximum_ifd_entries)
            {
                return;
            }
            const std::size_t entries_offset = static_cast<std::size_t>(offset) + 2;
            if (entries_offset > tiff.size() ||
                tiff.size() - entries_offset < static_cast<std::size_t>(*count) * 12)
            {
                return;
            }
            for (std::uint16_t index = 0; index < *count; ++index)
            {
                if (const auto value = entry(entries_offset + index * 12))
                {
                    callback(*value);
                }
            }
        }

        void finish()
        {
            if (!fallback_date.empty() && !contains(L"System.Photo.DateTaken"))
            {
                add_text(L"System.Photo.DateTaken", fallback_date);
            }
            if (width && height)
            {
                add_text(
                    L"System.Image.Dimensions",
                    std::to_wstring(*width) + L" x " + std::to_wstring(*height));
            }
            if (has_latitude)
            {
                auto value = latitude[0] + latitude[1] / 60.0 + latitude[2] / 3600.0;
                if (latitude_ref == L'S')
                {
                    value = -value;
                }
                add_double(L"System.GPS.Latitude", value);
            }
            if (has_longitude)
            {
                auto value = longitude[0] + longitude[1] / 60.0 + longitude[2] / 3600.0;
                if (longitude_ref == L'W')
                {
                    value = -value;
                }
                add_double(L"System.GPS.Longitude", value);
            }
        }
    };

    std::optional<std::size_t> find_tiff_header(
        std::span<const std::uint8_t> data) noexcept
    {
        const auto maximum_offset = std::min<std::size_t>(data.size(), 32);
        for (std::size_t offset = 0; offset + 4 <= maximum_offset; ++offset)
        {
            const bool little = data[offset] == 'I' && data[offset + 1] == 'I' &&
                data[offset + 2] == 42 && data[offset + 3] == 0;
            const bool big = data[offset] == 'M' && data[offset + 1] == 'M' &&
                data[offset + 2] == 0 && data[offset + 3] == 42;
            if (little || big)
            {
                return offset;
            }
        }
        return std::nullopt;
    }
}

namespace glance::components
{
    std::vector<ImageMetadataEntry> read_exif_metadata(
        std::span<const std::uint8_t> data) noexcept
    {
        try
        {
            const auto header = find_tiff_header(data);
            if (!header.has_value())
            {
                return {};
            }
            Reader reader{
                .tiff = data.subspan(*header),
                .little_endian = data[*header] == 'I' };
            const auto marker = reader.u16(2);
            const auto primary_offset = reader.u32(4);
            if (!marker.has_value() || *marker != 42 || !primary_offset.has_value())
            {
                return {};
            }
            reader.parse_ifd(*primary_offset, [&](const auto& entry) {
                reader.parse_primary_entry(entry);
            });
            if (reader.exif_ifd)
            {
                reader.parse_ifd(*reader.exif_ifd, [&](const auto& entry) {
                    reader.parse_exif_entry(entry);
                });
            }
            if (reader.gps_ifd)
            {
                reader.parse_ifd(*reader.gps_ifd, [&](const auto& entry) {
                    reader.parse_gps_entry(entry);
                });
            }
            reader.finish();
            return reader.entries;
        }
        catch (...)
        {
            return {};
        }
    }
}
