#include "pch.h"
#include "localization.h"
#include "preview_provider.h"

#include <windows.h>
#include <icu.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t maximum_encoding_detection_bytes = 16U * 1024U;
    constexpr std::size_t newline_search_bytes = 64U * 1024U;

    std::vector<std::byte> newline_sequence_for_encoding(std::string_view encoding)
    {
        if (encoding == "UTF-16LE")
        {
            return { std::byte{ 0x0A }, std::byte{ 0x00 } };
        }
        if (encoding == "UTF-16BE")
        {
            return { std::byte{ 0x00 }, std::byte{ 0x0A } };
        }
        if (encoding == "UTF-32LE")
        {
            return {
                std::byte{ 0x0A }, std::byte{ 0x00 },
                std::byte{ 0x00 }, std::byte{ 0x00 } };
        }
        if (encoding == "UTF-32BE")
        {
            return {
                std::byte{ 0x00 }, std::byte{ 0x00 },
                std::byte{ 0x00 }, std::byte{ 0x0A } };
        }
        return { std::byte{ 0x0A } };
    }

    std::optional<std::size_t> find_newline_end(
        const std::span<const std::byte> bytes,
        std::size_t start,
        const std::span<const std::byte> newline)
    {
        if (newline.empty() || start >= bytes.size())
        {
            return std::nullopt;
        }
        const std::size_t alignment = newline.size();
        if (const std::size_t remainder = start % alignment; remainder != 0)
        {
            start += alignment - remainder;
        }
        for (std::size_t index = start; index + newline.size() <= bytes.size(); index += alignment)
        {
            if (std::ranges::equal(bytes.subspan(index, newline.size()), newline))
            {
                return index + newline.size();
            }
        }
        return std::nullopt;
    }

    std::wstring lower_extension(const std::wstring& path)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return extension;
    }

    bool contains(const std::wstring_view value, const std::span<const std::wstring_view> values)
    {
        return std::ranges::find(values, value) != values.end();
    }

    bool valid_utf8(
        const std::span<const std::byte> bytes,
        bool allow_truncated_tail = false)
    {
        std::size_t index{};
        while (index < bytes.size())
        {
            const auto lead = std::to_integer<unsigned char>(bytes[index]);
            std::size_t continuation_count{};
            std::uint32_t code_point{};
            if (lead <= 0x7F)
            {
                ++index;
                continue;
            }
            if ((lead & 0xE0U) == 0xC0U)
            {
                continuation_count = 1;
                code_point = lead & 0x1FU;
                if (code_point < 2)
                {
                    return false;
                }
            }
            else if ((lead & 0xF0U) == 0xE0U)
            {
                continuation_count = 2;
                code_point = lead & 0x0FU;
            }
            else if ((lead & 0xF8U) == 0xF0U)
            {
                if (lead > 0xF4U)
                {
                    return false;
                }
                continuation_count = 3;
                code_point = lead & 0x07U;
            }
            else
            {
                return false;
            }
            if (index + continuation_count >= bytes.size())
            {
                for (std::size_t offset = 1; index + offset < bytes.size(); ++offset)
                {
                    const auto continuation =
                        std::to_integer<unsigned char>(bytes[index + offset]);
                    if ((continuation & 0xC0U) != 0x80U)
                    {
                        return false;
                    }
                }
                return allow_truncated_tail;
            }
            for (std::size_t offset = 1; offset <= continuation_count; ++offset)
            {
                const auto continuation = std::to_integer<unsigned char>(bytes[index + offset]);
                if ((continuation & 0xC0U) != 0x80U)
                {
                    return false;
                }
                code_point = (code_point << 6U) | (continuation & 0x3FU);
            }
            if ((continuation_count == 2 && code_point < 0x800U) ||
                (continuation_count == 3 && code_point < 0x10000U) ||
                code_point > 0x10FFFFU ||
                (code_point >= 0xD800U && code_point <= 0xDFFFU))
            {
                return false;
            }
            index += continuation_count + 1;
        }
        return true;
    }

    bool likely_utf16_without_bom(const std::span<const std::byte> bytes)
    {
        const std::size_t pair_count = bytes.size() / 2;
        if (pair_count < 4)
        {
            return false;
        }

        std::size_t even_zero_count{};
        std::size_t odd_zero_count{};
        for (std::size_t index = 0; index < pair_count; ++index)
        {
            even_zero_count += bytes[index * 2] == std::byte{} ? 1U : 0U;
            odd_zero_count += bytes[index * 2 + 1] == std::byte{} ? 1U : 0U;
        }
        const auto likely_lane = [pair_count](std::size_t zero_lane, std::size_t text_lane) {
            return zero_lane * 100U >= pair_count * 60U &&
                text_lane * 100U <= pair_count * 10U;
        };
        return likely_lane(even_zero_count, odd_zero_count) ||
            likely_lane(odd_zero_count, even_zero_count);
    }

    bool looks_like_binary_payload(
        std::span<const std::byte> bytes,
        bool utf8_bom,
        bool unicode_bom)
    {
        constexpr std::size_t maximum_sample_bytes = 64U * 1024U;
        if (bytes.size() >= 2 &&
            std::to_integer<unsigned char>(bytes[0]) == 0x4DU &&
            std::to_integer<unsigned char>(bytes[1]) == 0x5AU)
        {
            return true;
        }
        if (bytes.size() >= 4)
        {
            const std::array signature{
                std::to_integer<unsigned char>(bytes[0]),
                std::to_integer<unsigned char>(bytes[1]),
                std::to_integer<unsigned char>(bytes[2]),
                std::to_integer<unsigned char>(bytes[3]) };
            if (signature == std::array<unsigned char, 4>{ 0x7F, 0x45, 0x4C, 0x46 } ||
                signature == std::array<unsigned char, 4>{ 0x00, 0x61, 0x73, 0x6D } ||
                signature == std::array<unsigned char, 4>{ 0xCA, 0xFE, 0xBA, 0xBE } ||
                signature == std::array<unsigned char, 4>{ 0xFE, 0xED, 0xFA, 0xCE } ||
                signature == std::array<unsigned char, 4>{ 0xCE, 0xFA, 0xED, 0xFE } ||
                signature == std::array<unsigned char, 4>{ 0xFE, 0xED, 0xFA, 0xCF } ||
                signature == std::array<unsigned char, 4>{ 0xCF, 0xFA, 0xED, 0xFE })
            {
                return true;
            }
        }
        if (unicode_bom)
        {
            return false;
        }
        if (utf8_bom && bytes.size() >= 3)
        {
            bytes = bytes.subspan(3);
        }
        bytes = bytes.first(std::min(bytes.size(), maximum_sample_bytes));
        if (bytes.empty() || likely_utf16_without_bom(bytes))
        {
            return false;
        }

        std::size_t control_count{};
        for (const std::byte value : bytes)
        {
            const auto character = std::to_integer<unsigned char>(value);
            if (character == 0)
            {
                return true;
            }
            control_count +=
                (character < 0x20U && character != '\t' && character != '\n' &&
                 character != '\f' && character != '\r') || character == 0x7FU
                ? 1U
                : 0U;
        }
        return control_count * 100U > bytes.size();
    }

    bool looks_like_non_text_content(std::wstring_view content)
    {
        std::size_t invalid_count{};
        for (std::size_t index = 0; index < content.size(); ++index)
        {
            const wchar_t character = content[index];
            if (character == L'\0')
            {
                return true;
            }
            if (character >= 0xD800 && character <= 0xDBFF)
            {
                if (index + 1 >= content.size() ||
                    content[index + 1] < 0xDC00 || content[index + 1] > 0xDFFF)
                {
                    ++invalid_count;
                }
                else
                {
                    ++index;
                }
                continue;
            }
            if ((character >= 0xDC00 && character <= 0xDFFF) ||
                character == 0xFFFD || character == 0xFFFE || character == 0xFFFF ||
                (character < 0x20 && character != L'\t' && character != L'\n' &&
                 character != L'\f' && character != L'\r') || character == 0x7F)
            {
                ++invalid_count;
            }
        }
        return !content.empty() && invalid_count * 100U > content.size();
    }

    glance::app::PreviewKind sniff_unknown_file(const std::wstring& path)
    {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        if (!stream)
        {
            return glance::app::PreviewKind::generic;
        }
        std::array<std::byte, 4096> buffer{};
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::span<const std::byte> bytes(buffer.data(), static_cast<std::size_t>(stream.gcount()));
        if (bytes.empty())
        {
            return glance::app::PreviewKind::text;
        }

        const auto matches = [bytes](std::size_t offset, std::initializer_list<unsigned char> signature) {
            if (offset + signature.size() > bytes.size())
            {
                return false;
            }
            return std::ranges::equal(
                signature,
                bytes.subspan(offset, signature.size()),
                {},
                std::identity{},
                [](std::byte value) { return std::to_integer<unsigned char>(value); });
        };

        if (matches(0, { 0x89, 0x50, 0x4E, 0x47 }) ||
            matches(0, { 0xFF, 0xD8, 0xFF }) ||
            matches(0, { 0x47, 0x49, 0x46, 0x38 }) ||
            matches(0, { 0x42, 0x4D }))
        {
            return glance::app::PreviewKind::image;
        }
        if (matches(0, { 0x25, 0x50, 0x44, 0x46 }))
        {
            return glance::app::PreviewKind::pdf;
        }
        if (matches(0, { 0x50, 0x4B, 0x03, 0x04 }) || matches(0, { 0x37, 0x7A, 0xBC, 0xAF }))
        {
            return glance::app::PreviewKind::archive;
        }
        if (matches(0, { 0x1A, 0x45, 0xDF, 0xA3 }) ||
            matches(0, { 0x4F, 0x67, 0x67, 0x53 }) ||
            matches(0, { 0x66, 0x4C, 0x61, 0x43 }) ||
            matches(0, { 0x49, 0x44, 0x33 }) ||
            matches(4, { 0x66, 0x74, 0x79, 0x70 }) ||
            (matches(0, { 0x52, 0x49, 0x46, 0x46 }) &&
             (matches(8, { 0x57, 0x41, 0x56, 0x45 }) || matches(8, { 0x41, 0x56, 0x49, 0x20 }))))
        {
            return glance::app::PreviewKind::media;
        }

        if (!valid_utf8(bytes, true))
        {
            return glance::app::PreviewKind::generic;
        }
        const auto control_count = std::ranges::count_if(bytes, [](std::byte value) {
            const auto character = std::to_integer<unsigned char>(value);
            return character == 0 || (character < 0x09U) || (character > 0x0DU && character < 0x20U);
        });
        return static_cast<std::size_t>(control_count) * 100U <= bytes.size()
            ? glance::app::PreviewKind::text
            : glance::app::PreviewKind::generic;
    }

    struct DetectedEncoding
    {
        std::string converter_name;
        std::wstring display_name;
        std::int32_t confidence{};
    };

    struct CharsetDetectorCloser
    {
        void operator()(UCharsetDetector* detector) const noexcept
        {
            ucsdet_close(detector);
        }
    };

    struct ConverterCloser
    {
        void operator()(UConverter* converter) const noexcept
        {
            ucnv_close(converter);
        }
    };

    std::wstring chinese_encoding_name(const std::span<const std::byte> bytes)
    {
        bool requires_gbk{};
        for (std::size_t index = 0; index < bytes.size();)
        {
            const auto lead = std::to_integer<unsigned char>(bytes[index]);
            if (lead <= 0x7FU)
            {
                ++index;
                continue;
            }
            if (lead < 0x81U || lead > 0xFEU || index + 1 >= bytes.size())
            {
                return L"GB18030";
            }

            const auto trail = std::to_integer<unsigned char>(bytes[index + 1]);
            if (trail >= 0x30U && trail <= 0x39U)
            {
                if (index + 3 >= bytes.size())
                {
                    return L"GB18030";
                }
                const auto third = std::to_integer<unsigned char>(bytes[index + 2]);
                const auto fourth = std::to_integer<unsigned char>(bytes[index + 3]);
                if (third < 0x81U || third > 0xFEU || fourth < 0x30U || fourth > 0x39U)
                {
                    return L"GB18030";
                }
                return L"GB18030";
            }
            if (trail < 0x40U || trail == 0x7FU || trail > 0xFEU)
            {
                return L"GB18030";
            }
            requires_gbk = requires_gbk ||
                lead < 0xA1U || lead > 0xF7U || trail < 0xA1U;
            index += 2;
        }
        return requires_gbk ? L"GBK" : L"GB2312";
    }

    std::wstring display_encoding_name(
        const char* name,
        const std::span<const std::byte> bytes)
    {
        if (name == nullptr)
        {
            return {};
        }
        if (_stricmp(name, "UTF-16LE") == 0)
        {
            return L"UTF-16 LE";
        }
        if (_stricmp(name, "UTF-16BE") == 0)
        {
            return L"UTF-16 BE";
        }
        if (_stricmp(name, "UTF-32LE") == 0)
        {
            return L"UTF-32 LE";
        }
        if (_stricmp(name, "UTF-32BE") == 0)
        {
            return L"UTF-32 BE";
        }
        if (_stricmp(name, "GB18030") == 0)
        {
            return chinese_encoding_name(bytes);
        }
        if (_strnicmp(name, "windows-", 8) == 0)
        {
            std::wstring result{ L"Windows-" };
            for (name += 8; *name != '\0'; ++name)
            {
                result.push_back(static_cast<unsigned char>(*name));
            }
            return result;
        }

        std::wstring result;
        for (; *name != '\0'; ++name)
        {
            result.push_back(static_cast<unsigned char>(*name));
        }
        return result;
    }

    std::optional<DetectedEncoding> detect_text_encoding(const std::span<const std::byte> bytes)
    {
        if (bytes.empty())
        {
            return DetectedEncoding{ "UTF-8", L"UTF-8", 100 };
        }
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            return std::nullopt;
        }

        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<UCharsetDetector, CharsetDetectorCloser> detector(ucsdet_open(&status));
        if (U_FAILURE(status) || detector == nullptr)
        {
            return std::nullopt;
        }
        const auto detection_sample = bytes.first(std::min(
            bytes.size(),
            maximum_encoding_detection_bytes));
        ucsdet_setText(
            detector.get(),
            reinterpret_cast<const char*>(detection_sample.data()),
            static_cast<std::int32_t>(detection_sample.size()),
            &status);
        const UCharsetMatch* match = U_FAILURE(status) ? nullptr : ucsdet_detect(detector.get(), &status);
        if (U_FAILURE(status) || match == nullptr)
        {
            return std::nullopt;
        }

        const char* name = ucsdet_getName(match, &status);
        if (U_FAILURE(status) || name == nullptr)
        {
            return std::nullopt;
        }
        const std::int32_t confidence = ucsdet_getConfidence(match, &status);
        if (U_FAILURE(status))
        {
            return std::nullopt;
        }

        DetectedEncoding result;
        result.converter_name = name;
        result.display_name = display_encoding_name(name, detection_sample);
        result.confidence = confidence;
        return result.display_name.empty() ? std::nullopt : std::optional{ std::move(result) };
    }

    struct EncodingPlan
    {
        std::string converter_name;
        std::wstring display_name;
        std::size_t byte_offset{};
    };

    std::optional<EncodingPlan> select_encoding(
        std::span<const std::byte> sample,
        glance::app::TextEncoding encoding)
    {
        const bool utf8_bom = sample.size() >= 3 &&
            std::to_integer<unsigned char>(sample[0]) == 0xEF &&
            std::to_integer<unsigned char>(sample[1]) == 0xBB &&
            std::to_integer<unsigned char>(sample[2]) == 0xBF;
        const bool utf32_le_bom = sample.size() >= 4 &&
            std::to_integer<unsigned char>(sample[0]) == 0xFF &&
            std::to_integer<unsigned char>(sample[1]) == 0xFE &&
            std::to_integer<unsigned char>(sample[2]) == 0x00 &&
            std::to_integer<unsigned char>(sample[3]) == 0x00;
        const bool utf32_be_bom = sample.size() >= 4 &&
            std::to_integer<unsigned char>(sample[0]) == 0x00 &&
            std::to_integer<unsigned char>(sample[1]) == 0x00 &&
            std::to_integer<unsigned char>(sample[2]) == 0xFE &&
            std::to_integer<unsigned char>(sample[3]) == 0xFF;
        const bool utf16_le_bom = sample.size() >= 2 &&
            std::to_integer<unsigned char>(sample[0]) == 0xFF &&
            std::to_integer<unsigned char>(sample[1]) == 0xFE &&
            !utf32_le_bom;
        const bool utf16_be_bom = sample.size() >= 2 &&
            std::to_integer<unsigned char>(sample[0]) == 0xFE &&
            std::to_integer<unsigned char>(sample[1]) == 0xFF;

        if (encoding == glance::app::TextEncoding::utf8)
        {
            return EncodingPlan{ "UTF-8", L"UTF-8", utf8_bom ? 3U : 0U };
        }
        if (encoding == glance::app::TextEncoding::utf16_le)
        {
            return EncodingPlan{ "UTF-16LE", L"UTF-16 LE", utf16_le_bom ? 2U : 0U };
        }
        if (encoding == glance::app::TextEncoding::utf16_be)
        {
            return EncodingPlan{ "UTF-16BE", L"UTF-16 BE", utf16_be_bom ? 2U : 0U };
        }
        if (encoding == glance::app::TextEncoding::gb2312)
        {
            return EncodingPlan{ "windows-936", L"GB2312", 0 };
        }
        if (encoding == glance::app::TextEncoding::gbk)
        {
            return EncodingPlan{ "windows-936", L"GBK", 0 };
        }
        if (encoding == glance::app::TextEncoding::gb18030)
        {
            return EncodingPlan{ "GB18030", L"GB18030", 0 };
        }
        if (encoding == glance::app::TextEncoding::big5)
        {
            return EncodingPlan{ "windows-950", L"Big5", 0 };
        }
        if (encoding == glance::app::TextEncoding::system)
        {
            return EncodingPlan{
                "windows-" + std::to_string(GetACP()),
                glance::app::localize(L"SystemCodePage"),
                0 };
        }
        if (utf8_bom)
        {
            return EncodingPlan{ "UTF-8", L"UTF-8", 3 };
        }
        if (utf32_le_bom)
        {
            return EncodingPlan{ "UTF-32LE", L"UTF-32 LE", 4 };
        }
        if (utf32_be_bom)
        {
            return EncodingPlan{ "UTF-32BE", L"UTF-32 BE", 4 };
        }
        if (utf16_le_bom)
        {
            return EncodingPlan{ "UTF-16LE", L"UTF-16 LE", 2 };
        }
        if (utf16_be_bom)
        {
            return EncodingPlan{ "UTF-16BE", L"UTF-16 BE", 2 };
        }

        const bool utf8 = std::ranges::find(sample, std::byte{}) == sample.end() &&
            valid_utf8(sample, true);
        const bool ascii = utf8 && std::ranges::all_of(sample, [](std::byte value) {
            return std::to_integer<unsigned char>(value) <= 0x7FU;
        });
        if (ascii)
        {
            return EncodingPlan{ "UTF-8", L"UTF-8", 0 };
        }
        if (utf8)
        {
            return EncodingPlan{ "UTF-8", L"UTF-8", 0 };
        }
        if (auto detected = detect_text_encoding(sample))
        {
            return EncodingPlan{
                std::move(detected->converter_name),
                std::move(detected->display_name),
                0 };
        }
        return std::nullopt;
    }
}

namespace glance::app
{
    static_assert(sizeof(UChar) == sizeof(wchar_t));

    class IncrementalTextReader final : public std::enable_shared_from_this<IncrementalTextReader>
    {
    public:
        IncrementalTextReader(
            std::wstring path,
            std::uint64_t file_size,
            std::uint64_t byte_offset,
            std::wstring display_encoding,
            std::vector<std::byte> newline_sequence,
            std::unique_ptr<UConverter, ConverterCloser> converter)
            : path_(std::move(path)),
              file_size_(file_size),
              byte_offset_(byte_offset),
              display_encoding_(std::move(display_encoding)),
              newline_sequence_(std::move(newline_sequence)),
              converter_(std::move(converter))
        {
        }

        TextPreview read_next(std::size_t chunk_bytes)
        {
            std::scoped_lock lock(mutex_);
            TextPreview result;
            result.reader = shared_from_this();
            result.encoding = display_encoding_;
            if (failed_ || byte_offset_ >= file_size_)
            {
                return result;
            }

            const auto remaining = file_size_ - byte_offset_;
            const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining,
                std::max<std::size_t>(1, chunk_bytes)));
            std::vector<std::byte> bytes(requested);
            {
                std::ifstream stream(std::filesystem::path(path_), std::ios::binary);
                if (!stream)
                {
                    failed_ = true;
                    result.error = localize(L"TextFileOpenError");
                    return result;
                }
                stream.seekg(static_cast<std::streamoff>(byte_offset_), std::ios::beg);
                stream.read(
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<std::size_t>(stream.gcount()));
                while (byte_offset_ + bytes.size() < file_size_)
                {
                    if (const auto newline_end = find_newline_end(
                            bytes,
                            requested,
                            newline_sequence_))
                    {
                        bytes.resize(*newline_end);
                        break;
                    }

                    const auto extension = static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            file_size_ - byte_offset_ - bytes.size(),
                            newline_search_bytes));
                    const std::size_t previous_size = bytes.size();
                    bytes.resize(previous_size + extension);
                    stream.read(
                        reinterpret_cast<char*>(bytes.data() + previous_size),
                        static_cast<std::streamsize>(extension));
                    const auto bytes_read = static_cast<std::size_t>(stream.gcount());
                    bytes.resize(previous_size + bytes_read);
                    if (bytes_read == 0)
                    {
                        break;
                    }
                    if (const auto newline_end = find_newline_end(
                            bytes,
                            requested,
                            newline_sequence_))
                    {
                        bytes.resize(*newline_end);
                        break;
                    }
                }
            }
            if (bytes.empty())
            {
                failed_ = true;
                result.error = localize(L"TextFileOpenError");
                return result;
            }

            byte_offset_ += bytes.size();
            const bool end_of_file = byte_offset_ >= file_size_;
            std::vector<UChar> decoded(bytes.size() * 2U + 32U);
            const char* source = reinterpret_cast<const char*>(bytes.data());
            const char* source_limit = source + bytes.size();
            UChar* target = decoded.data();
            const UChar* target_limit = decoded.data() + decoded.size();
            UErrorCode status = U_ZERO_ERROR;
            ucnv_toUnicode(
                converter_.get(),
                &target,
                target_limit,
                &source,
                source_limit,
                nullptr,
                end_of_file,
                &status);
            if (U_FAILURE(status) || source != source_limit)
            {
                failed_ = true;
                result.error = localize(L"TextDecodeError");
                return result;
            }

            result.content.assign(
                reinterpret_cast<const wchar_t*>(decoded.data()),
                static_cast<std::size_t>(target - decoded.data()));
            if (looks_like_non_text_content(result.content))
            {
                failed_ = true;
                result.content.clear();
                result.error = localize(L"TextBinaryError");
                return result;
            }
            result.has_more = !end_of_file;
            return result;
        }

    private:
        std::wstring path_;
        std::uint64_t file_size_{};
        std::uint64_t byte_offset_{};
        std::wstring display_encoding_;
        std::vector<std::byte> newline_sequence_;
        std::unique_ptr<UConverter, ConverterCloser> converter_;
        std::mutex mutex_;
        bool failed_{};
    };

    bool can_try_preview_as_text(const std::wstring& path)
    {
        const auto extension = lower_extension(path);
        static constexpr std::array excluded_extensions{
            std::wstring_view(L".exe"), std::wstring_view(L".dll"),
            std::wstring_view(L".sys"), std::wstring_view(L".com"),
            std::wstring_view(L".scr"), std::wstring_view(L".cpl"),
            std::wstring_view(L".ocx"), std::wstring_view(L".msi"),
            std::wstring_view(L".msp"), std::wstring_view(L".msix"),
            std::wstring_view(L".appx"), std::wstring_view(L".appxbundle"),
            std::wstring_view(L".msixbundle"), std::wstring_view(L".obj"),
            std::wstring_view(L".lib"), std::wstring_view(L".pdb"),
            std::wstring_view(L".ilk"), std::wstring_view(L".pyc"),
            std::wstring_view(L".class"), std::wstring_view(L".ttf"),
            std::wstring_view(L".otf"), std::wstring_view(L".woff"),
            std::wstring_view(L".woff2"), std::wstring_view(L".iso"),
            std::wstring_view(L".vhd"), std::wstring_view(L".vhdx") };
        return !contains(extension, excluded_extensions);
    }

    PreviewKind resolve_preview_kind(const std::wstring& path)
    {
        const auto extension = lower_extension(path);
        static constexpr std::array text_extensions{
            std::wstring_view(L".txt"), std::wstring_view(L".log"), std::wstring_view(L".ini"),
            std::wstring_view(L".cfg"), std::wstring_view(L".conf"), std::wstring_view(L".json"),
            std::wstring_view(L".xml"), std::wstring_view(L".yaml"), std::wstring_view(L".yml"),
            std::wstring_view(L".toml"), std::wstring_view(L".csv"), std::wstring_view(L".tsv"),
            std::wstring_view(L".c"), std::wstring_view(L".h"), std::wstring_view(L".cpp"),
            std::wstring_view(L".hpp"), std::wstring_view(L".cc"), std::wstring_view(L".py"),
            std::wstring_view(L".rs"), std::wstring_view(L".go"), std::wstring_view(L".java"),
            std::wstring_view(L".js"), std::wstring_view(L".ts"), std::wstring_view(L".tsx"),
            std::wstring_view(L".jsx"), std::wstring_view(L".html"), std::wstring_view(L".css"),
            std::wstring_view(L".scss"), std::wstring_view(L".sql"), std::wstring_view(L".sh"),
            std::wstring_view(L".ps1"), std::wstring_view(L".bat"), std::wstring_view(L".cmd"),
            std::wstring_view(L".cmake"), std::wstring_view(L".vcxproj"), std::wstring_view(L".sln") };
        static constexpr std::array image_extensions{
            std::wstring_view(L".png"), std::wstring_view(L".jpg"), std::wstring_view(L".jpeg"),
            std::wstring_view(L".bmp"), std::wstring_view(L".gif"), std::wstring_view(L".tif"),
            std::wstring_view(L".tiff"), std::wstring_view(L".webp"), std::wstring_view(L".ico"),
            std::wstring_view(L".heic"), std::wstring_view(L".heif") };
        static constexpr std::array media_extensions{
            std::wstring_view(L".mp4"), std::wstring_view(L".mkv"), std::wstring_view(L".mov"),
            std::wstring_view(L".avi"), std::wstring_view(L".webm"), std::wstring_view(L".wmv"),
            std::wstring_view(L".mp3"), std::wstring_view(L".flac"), std::wstring_view(L".wav"),
            std::wstring_view(L".m4a"), std::wstring_view(L".aac"), std::wstring_view(L".ogg") };
        static constexpr std::array archive_extensions{
            std::wstring_view(L".zip"), std::wstring_view(L".7z"), std::wstring_view(L".rar"),
            std::wstring_view(L".tar"), std::wstring_view(L".gz"), std::wstring_view(L".bz2"),
            std::wstring_view(L".xz"), std::wstring_view(L".tgz"), std::wstring_view(L".tbz"),
            std::wstring_view(L".tbz2"), std::wstring_view(L".txz"), std::wstring_view(L".zst"),
            std::wstring_view(L".cab") };
        static constexpr std::array office_extensions{
            std::wstring_view(L".doc"), std::wstring_view(L".docx"), std::wstring_view(L".xls"),
            std::wstring_view(L".xlsx"), std::wstring_view(L".ppt"), std::wstring_view(L".pptx") };

        if (extension == L".md" || extension == L".markdown")
        {
            return PreviewKind::markdown;
        }
        if (contains(extension, text_extensions))
        {
            return PreviewKind::text;
        }
        if (contains(extension, image_extensions))
        {
            return PreviewKind::image;
        }
        if (contains(extension, media_extensions))
        {
            return PreviewKind::media;
        }
        if (extension == L".pdf")
        {
            return PreviewKind::pdf;
        }
        if (contains(extension, archive_extensions))
        {
            return PreviewKind::archive;
        }
        if (contains(extension, office_extensions))
        {
            return PreviewKind::office;
        }
        return sniff_unknown_file(path);
    }

    TextPreview load_text_preview(
        const std::wstring& path,
        std::size_t chunk_bytes,
        TextEncoding encoding)
    {
        TextPreview result;
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        if (!stream)
        {
            result.error = localize(L"TextFileOpenError");
            return result;
        }

        stream.seekg(0, std::ios::end);
        const auto file_size = stream.tellg();
        stream.seekg(0, std::ios::beg);
        if (file_size < 0)
        {
            result.error = localize(L"TextFileSizeError");
            return result;
        }

        const auto file_size_bytes = static_cast<std::uint64_t>(file_size);
        const auto bytes_to_read = std::min<std::uint64_t>(
            file_size_bytes,
            maximum_encoding_detection_bytes);
        std::vector<std::byte> bytes(static_cast<std::size_t>(bytes_to_read));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        stream.close();

        const std::span<const std::byte> payload = bytes;
        const bool utf8_bom = payload.size() >= 3 &&
            std::to_integer<unsigned char>(payload[0]) == 0xEF &&
            std::to_integer<unsigned char>(payload[1]) == 0xBB &&
            std::to_integer<unsigned char>(payload[2]) == 0xBF;
        const bool utf32_le_bom = payload.size() >= 4 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFF &&
            std::to_integer<unsigned char>(payload[1]) == 0xFE &&
            std::to_integer<unsigned char>(payload[2]) == 0x00 &&
            std::to_integer<unsigned char>(payload[3]) == 0x00;
        const bool utf32_be_bom = payload.size() >= 4 &&
            std::to_integer<unsigned char>(payload[0]) == 0x00 &&
            std::to_integer<unsigned char>(payload[1]) == 0x00 &&
            std::to_integer<unsigned char>(payload[2]) == 0xFE &&
            std::to_integer<unsigned char>(payload[3]) == 0xFF;
        const bool utf16_le_bom = payload.size() >= 2 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFF &&
            std::to_integer<unsigned char>(payload[1]) == 0xFE &&
            !utf32_le_bom;
        const bool utf16_be_bom = payload.size() >= 2 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFE &&
            std::to_integer<unsigned char>(payload[1]) == 0xFF;

        if (looks_like_binary_payload(
                payload,
                utf8_bom,
                utf16_le_bom || utf16_be_bom || utf32_le_bom || utf32_be_bom))
        {
            result.error = localize(L"TextBinaryError");
            return result;
        }

        const auto plan = select_encoding(payload, encoding);
        if (!plan)
        {
            result.error = localize(L"TextDecodeError");
            return result;
        }

        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<UConverter, ConverterCloser> converter(
            ucnv_open(plan->converter_name.c_str(), &status));
        if (U_FAILURE(status) || converter == nullptr)
        {
            result.error = localize(L"TextDecodeError");
            return result;
        }
        status = U_ZERO_ERROR;
        ucnv_setToUCallBack(
            converter.get(),
            UCNV_TO_U_CALLBACK_STOP,
            nullptr,
            nullptr,
            nullptr,
            &status);
        if (U_FAILURE(status))
        {
            result.error = localize(L"TextDecodeError");
            return result;
        }

        auto reader = std::make_shared<IncrementalTextReader>(
            path,
            file_size_bytes,
            plan->byte_offset,
            plan->display_name,
            newline_sequence_for_encoding(plan->converter_name),
            std::move(converter));
        return reader->read_next(chunk_bytes);
    }

    TextPreview load_next_text_preview_chunk(
        const std::shared_ptr<IncrementalTextReader>& reader,
        std::size_t chunk_bytes)
    {
        if (reader == nullptr)
        {
            TextPreview result;
            result.error = localize(L"TextDecodeError");
            return result;
        }
        return reader->read_next(chunk_bytes);
    }
}
