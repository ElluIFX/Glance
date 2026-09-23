#pragma once

#include "glance/contracts/paged_document_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace glance::app
{
    struct PagedDocumentOutlineEntry
    {
        std::uint32_t depth{};
        std::int32_t page_index{ -1 };
        std::wstring title;
    };

    struct PagedDocumentOpenResult
    {
        glance::contracts::document::Status status{
            glance::contracts::document::Status::open_failed };
        std::uint32_t page_count{};
        std::vector<PagedDocumentOutlineEntry> outline;
    };

    struct PagedDocumentRenderResult
    {
        glance::contracts::document::Status status{
            glance::contracts::document::Status::render_failed };
        std::uint32_t page_index{};
        std::uint32_t pixel_width{};
        std::uint32_t pixel_height{};
        std::uint32_t stride{};
        float page_width_points{};
        float page_height_points{};
        std::vector<std::byte> pixels;
    };

    class PagedDocumentRenderClient final
    {
    public:
        PagedDocumentRenderClient(
            std::wstring host_path,
            std::shared_ptr<void> renderer_lease);
        ~PagedDocumentRenderClient();

        PagedDocumentRenderClient(const PagedDocumentRenderClient&) = delete;
        PagedDocumentRenderClient& operator=(const PagedDocumentRenderClient&) = delete;

        [[nodiscard]] PagedDocumentOpenResult open(
            const std::wstring& path,
            const std::wstring& password,
            std::uint64_t generation = 0);
        [[nodiscard]] std::uint64_t begin_document() noexcept;
        [[nodiscard]] PagedDocumentRenderResult render(
            std::uint32_t page_index,
            std::uint32_t maximum_width,
            std::uint32_t maximum_height,
            std::uint64_t generation = 0);
        [[nodiscard]] std::uint64_t cancel_document() noexcept;
        [[nodiscard]] std::shared_ptr<const PagedDocumentRenderResult> render_cached(
            std::uint32_t page_index,
            std::uint32_t dimension,
            std::uint64_t generation,
            const std::atomic_bool& cancellation);
        void close_document(std::uint64_t generation = 0) noexcept;

    private:
        struct CachedPage
        {
            std::uint32_t dimension{};
            std::shared_ptr<const PagedDocumentRenderResult> result;
        };
        std::vector<CachedPage> page_cache_;
        std::mutex cache_mutex_;
        std::uint64_t cache_generation_{};
        [[nodiscard]] PagedDocumentRenderResult render_locked(
            std::uint32_t page_index,
            std::uint32_t maximum_width,
            std::uint32_t maximum_height,
            std::uint64_t generation);
        static void CALLBACK idle_timeout_callback(
            PTP_CALLBACK_INSTANCE,
            void* context,
            PTP_TIMER) noexcept;
        [[nodiscard]] bool start();
        void stop_idle_timer() noexcept;
        void arm_idle_timer() noexcept;
        void close_process_locked(bool graceful) noexcept;
        void terminate_process() noexcept;
        [[nodiscard]] bool send_control_command_locked(
            glance::contracts::document::Command command) noexcept;
        [[nodiscard]] bool transact(
            glance::contracts::document::Command command,
            const std::vector<std::byte>& request,
            glance::contracts::document::Status& status,
            std::vector<std::byte>& response);
        [[nodiscard]] PagedDocumentRenderResult consume_render_response(
            glance::contracts::document::Status status,
            const std::vector<std::byte>& response);

        std::wstring host_path_;
        std::shared_ptr<void> renderer_lease_;
        std::mutex mutex_;
        std::mutex process_handle_mutex_;
        std::atomic_bool cancelled_{};
        std::atomic_uint64_t document_generation_{};
        PTP_TIMER idle_timer_{};
        HANDLE process_{};
        HANDLE request_pipe_{};
        HANDLE response_pipe_{};
        HANDLE mapping_{};
        std::byte* bitmap_memory_{};
    };

    [[nodiscard]] std::shared_ptr<PagedDocumentRenderClient>
        acquire_paged_document_render_client();
}
