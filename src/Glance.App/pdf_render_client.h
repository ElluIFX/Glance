#pragma once

#include "glance/contracts/pdf_render_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace glance::app
{
    struct PdfOutlineEntry
    {
        std::uint32_t depth{};
        std::int32_t page_index{ -1 };
        std::wstring title;
    };

    struct PdfOpenResult
    {
        glance::contracts::pdf::Status status{ glance::contracts::pdf::Status::open_failed };
        std::uint32_t page_count{};
        std::vector<PdfOutlineEntry> outline;
    };

    struct PdfRenderResult
    {
        glance::contracts::pdf::Status status{ glance::contracts::pdf::Status::render_failed };
        std::uint32_t page_index{};
        std::uint32_t pixel_width{};
        std::uint32_t pixel_height{};
        std::uint32_t stride{};
        float page_width_points{};
        float page_height_points{};
        std::vector<std::byte> pixels;
    };

    class PdfRenderClient final
    {
    public:
        PdfRenderClient() = default;
        ~PdfRenderClient();

        PdfRenderClient(const PdfRenderClient&) = delete;
        PdfRenderClient& operator=(const PdfRenderClient&) = delete;

        [[nodiscard]] PdfOpenResult open(
            const std::wstring& path,
            const std::wstring& password);
        [[nodiscard]] PdfRenderResult render(
            std::uint32_t page_index,
            std::uint32_t maximum_width,
            std::uint32_t maximum_height);
        [[nodiscard]] PdfRenderResult render_emf(
            const std::wstring& path,
            std::uint32_t page_index,
            std::uint32_t maximum_width,
            std::uint32_t maximum_height);
        void cancel() noexcept;
        [[nodiscard]] bool prewarm();

    private:
        [[nodiscard]] bool start();
        void close_locked() noexcept;
        [[nodiscard]] bool transact(
            glance::contracts::pdf::Command command,
            const std::vector<std::byte>& request,
            glance::contracts::pdf::Status& status,
            std::vector<std::byte>& response);
        [[nodiscard]] PdfRenderResult consume_render_response(
            glance::contracts::pdf::Status status,
            const std::vector<std::byte>& response);

        std::mutex mutex_;
        std::mutex process_handle_mutex_;
        std::atomic_bool cancelled_{};
        HANDLE process_{};
        HANDLE request_pipe_{};
        HANDLE response_pipe_{};
        HANDLE mapping_{};
        std::byte* bitmap_memory_{};
    };

    void prewarm_pdf_render_client();
    [[nodiscard]] std::shared_ptr<PdfRenderClient> acquire_pdf_render_client();
}
