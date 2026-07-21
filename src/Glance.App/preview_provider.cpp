#include "pch.h"
#include "localization.h"
#include "preview_provider.h"

#include <windows.h>
#include <icu.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace
{
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

    bool valid_utf8(const std::span<const std::byte> bytes)
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
                continuation_count = 3;
                code_point = lead & 0x07U;
            }
            else
            {
                return false;
            }
            if (index + continuation_count >= bytes.size())
            {
                return false;
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

        if (!valid_utf8(bytes))
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

    std::wstring decode_multibyte(const std::span<const std::byte> bytes, UINT code_page)
    {
        if (bytes.empty())
        {
            return {};
        }
        const auto size = static_cast<int>(bytes.size());
        const int required = MultiByteToWideChar(
            code_page,
            code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
            reinterpret_cast<const char*>(bytes.data()),
            size,
            nullptr,
            0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(
            code_page,
            code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
            reinterpret_cast<const char*>(bytes.data()),
            size,
            result.data(),
            required);
        return result;
    }

    std::wstring decode_utf16(std::span<const std::byte> bytes, bool big_endian)
    {
        bytes = bytes.first(bytes.size() - bytes.size() % 2);
        std::wstring result(bytes.size() / 2, L'\0');
        for (std::size_t index = 0; index < result.size(); ++index)
        {
            const auto first = std::to_integer<unsigned char>(bytes[index * 2]);
            const auto second = std::to_integer<unsigned char>(bytes[index * 2 + 1]);
            result[index] = static_cast<wchar_t>(
                big_endian
                    ? (static_cast<unsigned int>(first) << 8U) | second
                    : (static_cast<unsigned int>(second) << 8U) | first);
        }
        return result;
    }

    struct DetectedText
    {
        std::wstring content;
        std::wstring encoding;
        std::int32_t confidence{};
    };

    struct CharsetDetectorCloser
    {
        void operator()(UCharsetDetector* detector) const noexcept
        {
            ucsdet_close(detector);
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

    std::optional<DetectedText> detect_text_encoding(const std::span<const std::byte> bytes)
    {
        if (bytes.empty())
        {
            return DetectedText{ {}, L"UTF-8", 100 };
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
        ucsdet_setText(
            detector.get(),
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::int32_t>(bytes.size()),
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

        status = U_ZERO_ERROR;
        const std::int32_t required = ucsdet_getUChars(match, nullptr, 0, &status);
        if (required < 0 || (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)))
        {
            return std::nullopt;
        }
        status = U_ZERO_ERROR;
        std::vector<UChar> converted(static_cast<std::size_t>(required) + 1U);
        const std::int32_t written = ucsdet_getUChars(
            match,
            converted.data(),
            static_cast<std::int32_t>(converted.size()),
            &status);
        if (U_FAILURE(status) || written < 0)
        {
            return std::nullopt;
        }

        static_assert(sizeof(UChar) == sizeof(wchar_t));
        DetectedText result;
        result.content.resize(static_cast<std::size_t>(written));
        std::memcpy(
            result.content.data(),
            converted.data(),
            static_cast<std::size_t>(written) * sizeof(UChar));
        result.encoding = display_encoding_name(name, bytes);
        result.confidence = confidence;
        return result.encoding.empty() ? std::nullopt : std::optional{ std::move(result) };
    }
}

namespace glance::app
{
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
        std::size_t maximum_bytes,
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

        const auto bytes_to_read = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(file_size),
            maximum_bytes);
        std::vector<std::byte> bytes(static_cast<std::size_t>(bytes_to_read));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        result.truncated = static_cast<std::uint64_t>(file_size) > bytes.size();

        std::span<const std::byte> payload = bytes;
        const bool utf8_bom = payload.size() >= 3 &&
            std::to_integer<unsigned char>(payload[0]) == 0xEF &&
            std::to_integer<unsigned char>(payload[1]) == 0xBB &&
            std::to_integer<unsigned char>(payload[2]) == 0xBF;
        const bool utf32_le_bom = payload.size() >= 4 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFF &&
            std::to_integer<unsigned char>(payload[1]) == 0xFE &&
            std::to_integer<unsigned char>(payload[2]) == 0x00 &&
            std::to_integer<unsigned char>(payload[3]) == 0x00;
        const bool utf16_le_bom = payload.size() >= 2 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFF &&
            std::to_integer<unsigned char>(payload[1]) == 0xFE &&
            !utf32_le_bom;
        const bool utf16_be_bom = payload.size() >= 2 &&
            std::to_integer<unsigned char>(payload[0]) == 0xFE &&
            std::to_integer<unsigned char>(payload[1]) == 0xFF;

        if (encoding == TextEncoding::utf8 ||
            (encoding == TextEncoding::automatic && utf8_bom))
        {
            if (utf8_bom)
            {
                payload = payload.subspan(3);
            }
            result.encoding = L"UTF-8";
            result.content = decode_multibyte(payload, CP_UTF8);
        }
        else if (encoding == TextEncoding::utf16_le ||
                 (encoding == TextEncoding::automatic && utf16_le_bom))
        {
            if (utf16_le_bom)
            {
                payload = payload.subspan(2);
            }
            result.encoding = L"UTF-16 LE";
            result.content = decode_utf16(payload, false);
        }
        else if (encoding == TextEncoding::utf16_be ||
                 (encoding == TextEncoding::automatic && utf16_be_bom))
        {
            if (utf16_be_bom)
            {
                payload = payload.subspan(2);
            }
            result.encoding = L"UTF-16 BE";
            result.content = decode_utf16(payload, true);
        }
        else if (encoding == TextEncoding::gb2312)
        {
            result.encoding = L"GB2312";
            result.content = decode_multibyte(payload, 936);
        }
        else if (encoding == TextEncoding::gbk)
        {
            result.encoding = L"GBK";
            result.content = decode_multibyte(payload, 936);
        }
        else if (encoding == TextEncoding::gb18030)
        {
            result.encoding = L"GB18030";
            result.content = decode_multibyte(payload, 54936);
        }
        else if (encoding == TextEncoding::big5)
        {
            result.encoding = L"Big5";
            result.content = decode_multibyte(payload, 950);
        }
        else if (encoding == TextEncoding::system)
        {
            result.encoding = L"System code page";
            result.content = decode_multibyte(payload, CP_ACP);
        }
        else if (encoding == TextEncoding::automatic)
        {
            const bool utf8 = std::ranges::find(payload, std::byte{}) == payload.end() &&
                valid_utf8(payload);
            const bool ascii = utf8 && std::ranges::all_of(payload, [](std::byte value) {
                return std::to_integer<unsigned char>(value) <= 0x7FU;
            });
            if (ascii)
            {
                result.encoding = L"UTF-8";
                result.content = decode_multibyte(payload, CP_UTF8);
            }
            else if (auto detected = detect_text_encoding(payload))
            {
                if (utf8 && detected->encoding != L"UTF-8" && detected->confidence < 50)
                {
                    result.encoding = L"UTF-8";
                    result.content = decode_multibyte(payload, CP_UTF8);
                }
                else
                {
                    result.encoding = std::move(detected->encoding);
                    result.content = std::move(detected->content);
                }
            }
            else if (utf8)
            {
                result.encoding = L"UTF-8";
                result.content = decode_multibyte(payload, CP_UTF8);
            }
            else
            {
                result.error = localize(L"TextDecodeError");
            }
        }
        else
        {
            result.encoding = L"System code page";
            result.content = decode_multibyte(payload, CP_ACP);
        }

        if (!payload.empty() && result.content.empty())
        {
            result.error = localize(L"TextDecodeError");
        }
        return result;
    }
}
