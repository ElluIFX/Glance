#pragma once

#include "glance/contracts/office_preview_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace glance::app
{
    struct OfficePageResult
    {
        glance::contracts::office::Status status{
            glance::contracts::office::Status::render_failed };
        std::uint32_t page_index{};
        float page_width_points{};
        float page_height_points{};
        std::wstring emf_path;
    };

    struct OfficePageCountResult
    {
        glance::contracts::office::Status status{
            glance::contracts::office::Status::open_failed };
        std::uint32_t page_count{};
    };

    class OfficePreviewClient final
    {
    public:
        OfficePreviewClient() = default;
        ~OfficePreviewClient();

        OfficePreviewClient(const OfficePreviewClient&) = delete;
        OfficePreviewClient& operator=(const OfficePreviewClient&) = delete;

        [[nodiscard]] bool open_word(const std::wstring& path);
        [[nodiscard]] OfficePageResult render_page(std::uint32_t page_index);
        [[nodiscard]] OfficePageCountResult page_count();
        void cancel() noexcept;

    private:
        [[nodiscard]] bool launch_word_host();
        [[nodiscard]] bool transact(
            glance::contracts::office::Command command,
            const std::vector<std::byte>& request,
            glance::contracts::office::Status& status,
            std::vector<std::byte>& response);
        void close_locked() noexcept;

        std::mutex mutex_;
        std::mutex process_handle_mutex_;
        std::atomic_bool cancelled_{};
        HANDLE process_{};
        HANDLE job_{};
        HANDLE request_pipe_{};
        HANDLE response_pipe_{};
        std::filesystem::path session_directory_;
        std::filesystem::path staged_input_;
        std::filesystem::path cache_directory_;
    };
}
