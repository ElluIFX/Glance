#include <windows.h>

#include <pdfium/fpdf_doc.h>
#include <pdfium/fpdfview.h>

#include "glance/contracts/paged_document_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace glance::contracts::document;

    bool read_exact(HANDLE handle, void* destination, std::size_t size)
    {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size != 0)
        {
            DWORD read{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(handle, bytes, request, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE handle, const void* source, std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(source);
        while (size != 0)
        {
            DWORD written{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(handle, bytes, request, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    template <typename T>
    void append_value(std::vector<std::byte>& output, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), begin, begin + sizeof(T));
    }

    void append_utf16(std::vector<std::byte>& output, std::wstring_view value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        output.insert(output.end(), begin, begin + value.size() * sizeof(wchar_t));
    }

    std::string utf8_from_utf16(std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (size <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr);
        return result;
    }

    std::vector<std::byte> read_document_bytes(const std::wstring& path)
    {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return {};
        }
        const auto length = stream.tellg();
        if (length <= 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::size_t>::max())
        {
            return {};
        }
        if (static_cast<std::uint64_t>(length) > maximum_document_size)
        {
            return {};
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), length))
        {
            return {};
        }
        return bytes;
    }

    std::wstring bookmark_title(FPDF_BOOKMARK bookmark)
    {
        const unsigned long byte_count = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
        if (byte_count <= sizeof(wchar_t) || byte_count % sizeof(wchar_t) != 0)
        {
            return {};
        }
        std::wstring title(byte_count / sizeof(wchar_t), L'\0');
        const unsigned long copied = FPDFBookmark_GetTitle(bookmark, title.data(), byte_count);
        if (copied == 0)
        {
            return {};
        }
        while (!title.empty() && title.back() == L'\0')
        {
            title.pop_back();
        }
        return title;
    }

    int bookmark_page(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark)
    {
        FPDF_DEST destination = FPDFBookmark_GetDest(document, bookmark);
        if (destination == nullptr)
        {
            if (const FPDF_ACTION action = FPDFBookmark_GetAction(bookmark); action != nullptr)
            {
                destination = FPDFAction_GetDest(document, action);
            }
        }
        return destination == nullptr ? -1 : FPDFDest_GetDestPageIndex(document, destination);
    }

    void append_outline(
        FPDF_DOCUMENT document,
        FPDF_BOOKMARK parent,
        std::uint32_t depth,
        std::vector<std::byte>& output,
        std::uint32_t& count)
    {
        if (depth >= 32 || count >= 4096)
        {
            return;
        }
        for (FPDF_BOOKMARK bookmark = FPDFBookmark_GetFirstChild(document, parent);
             bookmark != nullptr && count < 4096;
             bookmark = FPDFBookmark_GetNextSibling(document, bookmark))
        {
            auto title = bookmark_title(bookmark);
            if (title.size() > 4096)
            {
                title.resize(4096);
            }
            const OutlineEntry entry{
                .depth = depth,
                .page_index = bookmark_page(document, bookmark),
                .title_characters = static_cast<std::uint32_t>(title.size()),
            };
            append_value(output, entry);
            append_utf16(output, title);
            ++count;
            append_outline(document, bookmark, depth + 1, output, count);
        }
    }

    class RenderServer
    {
    public:
        RenderServer(HANDLE request, HANDLE response, HANDLE mapping)
            : request_(request), response_(response), mapping_(mapping)
        {
            bitmap_memory_ = static_cast<std::byte*>(MapViewOfFile(
                mapping_,
                FILE_MAP_WRITE,
                0,
                0,
                shared_bitmap_size));
        }

        ~RenderServer()
        {
            close_document();
            if (bitmap_memory_ != nullptr)
            {
                UnmapViewOfFile(bitmap_memory_);
            }
        }

        int run()
        {
            if (bitmap_memory_ == nullptr)
            {
                return 4;
            }
            for (;;)
            {
                RequestHeader request{};
                if (!read_exact(request_, &request, sizeof(request)))
                {
                    return 0;
                }
                if (request.magic != protocol_magic || request.version != protocol_version ||
                    request.payload_size > maximum_payload_size)
                {
                    return 5;
                }
                std::vector<std::byte> payload(request.payload_size);
                if (!payload.empty() && !read_exact(request_, payload.data(), payload.size()))
                {
                    return 6;
                }
                if (request.command == Command::shutdown)
                {
                    send_response(Status::success, {});
                    return 0;
                }
                if (request.command == Command::open_document)
                {
                    handle_open(payload);
                }
                else if (request.command == Command::render_page)
                {
                    handle_render(payload);
                }
                else if (request.command == Command::close_document)
                {
                    close_document();
                    send_response(Status::success, {});
                }
                else
                {
                    send_response(Status::invalid_request, {});
                }
            }
        }

    private:
        bool ensure_bitmap_capacity(std::uint64_t byte_count)
        {
            if (byte_count > shared_bitmap_size || bitmap_memory_ == nullptr)
            {
                return false;
            }
            SYSTEM_INFO system_info{};
            GetSystemInfo(&system_info);
            const std::uint64_t page_size = system_info.dwPageSize;
            const std::uint64_t required =
                (byte_count + page_size - 1U) / page_size * page_size;
            if (required <= committed_bitmap_size_)
            {
                return true;
            }
            const std::uint64_t additional = required - committed_bitmap_size_;
            auto* const start = bitmap_memory_ + committed_bitmap_size_;
            if (VirtualAlloc(
                    start,
                    static_cast<SIZE_T>(additional),
                    MEM_COMMIT,
                    PAGE_READWRITE) == nullptr)
            {
                return false;
            }
            committed_bitmap_size_ = required;
            return true;
        }

        void close_document()
        {
            if (document_ != nullptr)
            {
                FPDF_CloseDocument(document_);
                document_ = nullptr;
            }
            document_bytes_.clear();
            document_bytes_.shrink_to_fit();
        }

        bool send_response(Status status, const std::vector<std::byte>& payload)
        {
            const ResponseHeader response{
                .status = status,
                .payload_size = static_cast<std::uint32_t>(payload.size()),
            };
            return write_exact(response_, &response, sizeof(response)) &&
                (payload.empty() || write_exact(response_, payload.data(), payload.size()));
        }

        void handle_open(const std::vector<std::byte>& payload)
        {
            close_document();
            if (payload.size() < sizeof(OpenRequest))
            {
                send_response(Status::invalid_request, {});
                return;
            }
            OpenRequest request{};
            std::memcpy(&request, payload.data(), sizeof(request));
            const std::uint64_t character_count =
                static_cast<std::uint64_t>(request.path_characters) + request.password_characters;
            if (character_count * sizeof(wchar_t) != payload.size() - sizeof(request))
            {
                send_response(Status::invalid_request, {});
                return;
            }
            const auto* characters = reinterpret_cast<const wchar_t*>(payload.data() + sizeof(request));
            const std::wstring path(characters, request.path_characters);
            const std::wstring_view password(
                characters + request.path_characters,
                request.password_characters);
            document_bytes_ = read_document_bytes(path);
            if (document_bytes_.empty())
            {
                send_response(Status::open_failed, {});
                return;
            }
            const std::string password_utf8 = utf8_from_utf16(password);
            document_ = FPDF_LoadMemDocument64(
                document_bytes_.data(),
                document_bytes_.size(),
                password_utf8.empty() ? nullptr : password_utf8.c_str());
            if (document_ == nullptr)
            {
                const auto error = FPDF_GetLastError();
                document_bytes_.clear();
                document_bytes_.shrink_to_fit();
                send_response(
                    error == FPDF_ERR_PASSWORD
                        ? (password.empty() ? Status::password_required : Status::invalid_password)
                        : Status::open_failed,
                    {});
                return;
            }

            std::vector<std::byte> response(sizeof(OpenResponse));
            std::uint32_t outline_count{};
            append_outline(document_, nullptr, 0, response, outline_count);
            const OpenResponse metadata{
                .page_count = static_cast<std::uint32_t>(std::max(0, FPDF_GetPageCount(document_))),
                .outline_count = outline_count,
            };
            std::memcpy(response.data(), &metadata, sizeof(metadata));
            send_response(Status::success, response);
        }

        void handle_render(const std::vector<std::byte>& payload)
        {
            if (document_ == nullptr || payload.size() != sizeof(RenderRequest))
            {
                send_response(Status::invalid_request, {});
                return;
            }
            RenderRequest request{};
            std::memcpy(&request, payload.data(), sizeof(request));
            const int page_count = FPDF_GetPageCount(document_);
            if (request.page_index >= static_cast<std::uint32_t>(std::max(0, page_count)))
            {
                send_response(Status::invalid_page, {});
                return;
            }
            FPDF_PAGE page = FPDF_LoadPage(document_, static_cast<int>(request.page_index));
            if (page == nullptr)
            {
                send_response(Status::render_failed, {});
                return;
            }
            const float page_width = FPDF_GetPageWidthF(page);
            const float page_height = FPDF_GetPageHeightF(page);
            if (!std::isfinite(page_width) || !std::isfinite(page_height) ||
                page_width <= 0.0F || page_height <= 0.0F)
            {
                FPDF_ClosePage(page);
                send_response(Status::render_failed, {});
                return;
            }
            const std::uint32_t maximum_width = std::clamp(
                request.maximum_width,
                1U,
                maximum_bitmap_dimension);
            const std::uint32_t maximum_height = std::clamp(
                request.maximum_height,
                1U,
                maximum_bitmap_dimension);
            const double scale = std::min(
                maximum_width / std::max(1.0, static_cast<double>(page_width)),
                maximum_height / std::max(1.0, static_cast<double>(page_height)));
            const int width = std::max(1, static_cast<int>(std::lround(page_width * scale)));
            const int height = std::max(1, static_cast<int>(std::lround(page_height * scale)));
            FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, 1);
            if (bitmap == nullptr)
            {
                FPDF_ClosePage(page);
                send_response(Status::render_failed, {});
                return;
            }
            FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFFU);
            FPDF_RenderPageBitmap(
                bitmap,
                page,
                0,
                0,
                width,
                height,
                0,
                FPDF_ANNOT | FPDF_LCD_TEXT);
            const int stride = FPDFBitmap_GetStride(bitmap);
            const std::uint64_t byte_count =
                static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(height);
            if (stride <= 0 || !ensure_bitmap_capacity(byte_count))
            {
                FPDFBitmap_Destroy(bitmap);
                FPDF_ClosePage(page);
                send_response(Status::render_failed, {});
                return;
            }
            std::memcpy(bitmap_memory_, FPDFBitmap_GetBuffer(bitmap), static_cast<std::size_t>(byte_count));
            const RenderResponse response{
                .page_index = request.page_index,
                .pixel_width = static_cast<std::uint32_t>(width),
                .pixel_height = static_cast<std::uint32_t>(height),
                .stride = static_cast<std::uint32_t>(stride),
                .page_width_points = page_width,
                .page_height_points = page_height,
            };
            std::vector<std::byte> response_payload;
            append_value(response_payload, response);
            FPDFBitmap_Destroy(bitmap);
            FPDF_ClosePage(page);
            send_response(Status::success, response_payload);
        }

        HANDLE request_{};
        HANDLE response_{};
        HANDLE mapping_{};
        std::byte* bitmap_memory_{};
        std::uint64_t committed_bitmap_size_{};
        std::vector<std::byte> document_bytes_;
        FPDF_DOCUMENT document_{};
    };

    HANDLE parse_handle(const wchar_t* value)
    {
        wchar_t* end{};
        const auto numeric = _wcstoui64(value, &end, 10);
        return end != value && *end == L'\0'
            ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(numeric))
            : nullptr;
    }

    int self_test(const std::wstring& path, std::wstring_view password)
    {
        auto bytes = read_document_bytes(path);
        if (bytes.empty())
        {
            return 20;
        }
        const std::string password_utf8 = utf8_from_utf16(password);
        FPDF_DOCUMENT document = FPDF_LoadMemDocument64(
            bytes.data(),
            bytes.size(),
            password_utf8.empty() ? nullptr : password_utf8.c_str());
        if (document == nullptr)
        {
            return 21;
        }
        const int pages = FPDF_GetPageCount(document);
        FPDF_PAGE page = pages > 0 ? FPDF_LoadPage(document, 0) : nullptr;
        if (page == nullptr)
        {
            FPDF_CloseDocument(document);
            return 22;
        }
        FPDF_BITMAP bitmap = FPDFBitmap_Create(256, 256, 1);
        if (bitmap == nullptr)
        {
            FPDF_ClosePage(page);
            FPDF_CloseDocument(document);
            return 23;
        }
        FPDFBitmap_FillRect(bitmap, 0, 0, 256, 256, 0xFFFFFFFFU);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, 256, 256, 0, FPDF_ANNOT);
        std::wcout << L"pages=" << pages << L" first_page="
                   << FPDF_GetPageWidthF(page) << L"x" << FPDF_GetPageHeightF(page) << L'\n';
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);
        FPDF_CloseDocument(document);
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    FPDF_LIBRARY_CONFIG configuration{};
    configuration.version = 2;
    FPDF_InitLibraryWithConfig(&configuration);
    int result{};
    if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--self-test")
    {
        result = self_test(argv[2], argc == 4 ? std::wstring_view(argv[3]) : std::wstring_view{});
    }
    else if (argc == 4)
    {
        const HANDLE request = parse_handle(argv[1]);
        const HANDLE response = parse_handle(argv[2]);
        const HANDLE mapping = parse_handle(argv[3]);
        result = request != nullptr && response != nullptr && mapping != nullptr
            ? RenderServer(request, response, mapping).run()
            : 2;
    }
    else
    {
        result = 1;
    }
    FPDF_DestroyLibrary();
    return result;
}
