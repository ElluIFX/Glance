#pragma once

#include "glance/contracts/component_api.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    class IncrementalTextReader;

    enum class PreviewKind
    {
        generic,
        text,
        markdown,
        web,
        image,
        media,
        document,
        native_document,
        archive,
        component,
    };

    struct UndecodableByte
    {
        std::size_t offset{}; // UTF-16 offset of the replacement character in this chunk.
        std::uint8_t value{};
    };

    struct TextPreview
    {
        std::wstring content;
        std::vector<UndecodableByte> undecodable_bytes;
        std::wstring encoding;
        std::wstring line_endings;
        std::wstring error;
        std::shared_ptr<IncrementalTextReader> reader;
        bool has_more{};
        bool replace_content{};
        bool retry_later{};
        std::uint64_t bytes_read{};
    };

    struct MaterializedShellFile
    {
        std::wstring path;
        std::shared_ptr<void> lease;
        std::uint64_t size{};
        std::uint64_t creation_time{};
        std::uint64_t last_write_time{};
        std::int32_t error{};
        std::wstring error_stage;
        bool cancelled{};
    };

    enum class TextEncoding
    {
        automatic,
        utf8,
        utf16_le,
        utf16_be,
        utf32_le,
        utf32_be,
        system,
        gb2312,
        gbk,
        gb18030,
        big5,
        shift_jis,
        euc_jp,
        iso2022_jp,
        euc_kr,
        windows949,
        windows1250,
        windows1251,
        windows1252,
        windows1253,
        windows1254,
        windows1255,
        windows1256,
        windows1257,
        windows1258,
        iso8859_1,
        iso8859_2,
        iso8859_5,
        iso8859_6,
        iso8859_7,
        iso8859_8,
        iso8859_9,
        iso8859_15,
        koi8_r,
        koi8_u,
        tis620,
        ibm437,
        ibm850,
        ibm866,
        mac_roman,
        ascii,
    };

    struct TextEncodingDescriptor
    {
        TextEncoding encoding;
        const char* converter_name;
        const wchar_t* label_key;
    };

    inline constexpr std::array text_encodings{
        TextEncodingDescriptor{ TextEncoding::utf8, "UTF-8", L"Encoding_utf8" },
        TextEncodingDescriptor{ TextEncoding::utf16_le, "UTF-16LE", L"Encoding_utf16_le" },
        TextEncodingDescriptor{ TextEncoding::utf16_be, "UTF-16BE", L"Encoding_utf16_be" },
        TextEncodingDescriptor{ TextEncoding::utf32_le, "UTF-32LE", L"Encoding_utf32_le" },
        TextEncodingDescriptor{ TextEncoding::utf32_be, "UTF-32BE", L"Encoding_utf32_be" },
        TextEncodingDescriptor{ TextEncoding::system, "", L"Encoding_system" },
        TextEncodingDescriptor{ TextEncoding::gb2312, "windows-936", L"Encoding_gb2312" },
        TextEncodingDescriptor{ TextEncoding::gbk, "windows-936", L"Encoding_gbk" },
        TextEncodingDescriptor{ TextEncoding::gb18030, "GB18030", L"Encoding_gb18030" },
        TextEncodingDescriptor{ TextEncoding::big5, "windows-950", L"Encoding_big5" },
        TextEncodingDescriptor{ TextEncoding::shift_jis, "windows-932", L"Encoding_shift_jis" },
        TextEncodingDescriptor{ TextEncoding::euc_jp, "EUC-JP", L"Encoding_euc_jp" },
        TextEncodingDescriptor{ TextEncoding::iso2022_jp, "ISO-2022-JP", L"Encoding_iso2022_jp" },
        TextEncodingDescriptor{ TextEncoding::euc_kr, "EUC-KR", L"Encoding_euc_kr" },
        TextEncodingDescriptor{ TextEncoding::windows949, "windows-949", L"Encoding_windows949" },
        TextEncodingDescriptor{ TextEncoding::windows1250, "windows-1250", L"Encoding_windows1250" },
        TextEncodingDescriptor{ TextEncoding::windows1251, "windows-1251", L"Encoding_windows1251" },
        TextEncodingDescriptor{ TextEncoding::windows1252, "windows-1252", L"Encoding_windows1252" },
        TextEncodingDescriptor{ TextEncoding::windows1253, "windows-1253", L"Encoding_windows1253" },
        TextEncodingDescriptor{ TextEncoding::windows1254, "windows-1254", L"Encoding_windows1254" },
        TextEncodingDescriptor{ TextEncoding::windows1255, "windows-1255", L"Encoding_windows1255" },
        TextEncodingDescriptor{ TextEncoding::windows1256, "windows-1256", L"Encoding_windows1256" },
        TextEncodingDescriptor{ TextEncoding::windows1257, "windows-1257", L"Encoding_windows1257" },
        TextEncodingDescriptor{ TextEncoding::windows1258, "windows-1258", L"Encoding_windows1258" },
        TextEncodingDescriptor{ TextEncoding::iso8859_1, "ISO-8859-1", L"Encoding_iso8859_1" },
        TextEncodingDescriptor{ TextEncoding::iso8859_2, "ISO-8859-2", L"Encoding_iso8859_2" },
        TextEncodingDescriptor{ TextEncoding::iso8859_5, "ISO-8859-5", L"Encoding_iso8859_5" },
        TextEncodingDescriptor{ TextEncoding::iso8859_6, "ISO-8859-6", L"Encoding_iso8859_6" },
        TextEncodingDescriptor{ TextEncoding::iso8859_7, "ISO-8859-7", L"Encoding_iso8859_7" },
        TextEncodingDescriptor{ TextEncoding::iso8859_8, "ISO-8859-8", L"Encoding_iso8859_8" },
        TextEncodingDescriptor{ TextEncoding::iso8859_9, "ISO-8859-9", L"Encoding_iso8859_9" },
        TextEncodingDescriptor{ TextEncoding::iso8859_15, "ISO-8859-15", L"Encoding_iso8859_15" },
        TextEncodingDescriptor{ TextEncoding::koi8_r, "KOI8-R", L"Encoding_koi8_r" },
        TextEncodingDescriptor{ TextEncoding::koi8_u, "KOI8-U", L"Encoding_koi8_u" },
        TextEncodingDescriptor{ TextEncoding::tis620, "TIS-620", L"Encoding_tis620" },
        TextEncodingDescriptor{ TextEncoding::ibm437, "IBM437", L"Encoding_ibm437" },
        TextEncodingDescriptor{ TextEncoding::ibm850, "IBM850", L"Encoding_ibm850" },
        TextEncodingDescriptor{ TextEncoding::ibm866, "IBM866", L"Encoding_ibm866" },
        TextEncodingDescriptor{ TextEncoding::mac_roman, "macintosh", L"Encoding_mac_roman" },
        TextEncodingDescriptor{ TextEncoding::ascii, "US-ASCII", L"Encoding_ascii" }
    };

    [[nodiscard]] PreviewKind resolve_preview_kind(const std::wstring& path);
    [[nodiscard]] PreviewKind probe_preview_kind(const std::wstring& path);
    [[nodiscard]] glance::contracts::components::GalleryMediaKind gallery_media_kind(
        const std::wstring& path);
    [[nodiscard]] std::vector<std::wstring> gallery_extensions(
        glance::contracts::components::GalleryMediaKind kind);
    [[nodiscard]] bool can_decode_text_sample(const std::wstring& path);
    [[nodiscard]] MaterializedShellFile materialize_shell_file(
        std::wstring_view parsing_name,
        std::wstring_view display_name,
        std::span<const std::uint8_t> shell_id_list,
        const std::shared_ptr<std::atomic_bool>& cancellation) noexcept;
    [[nodiscard]] TextPreview load_text_preview(
        const std::wstring& path,
        std::size_t chunk_bytes = 256U * 1024U,
        TextEncoding encoding = TextEncoding::automatic,
        bool monitor = false);
    [[nodiscard]] TextPreview load_next_text_preview_chunk(
        const std::shared_ptr<IncrementalTextReader>& reader,
        std::size_t chunk_bytes = 256U * 1024U);
    void cancel_text_preview_read(
        const std::shared_ptr<IncrementalTextReader>& reader) noexcept;
}
