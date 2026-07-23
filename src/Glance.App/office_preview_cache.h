#pragma once

#include "office_preview_client.h"
#include "office_preview_preferences.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace glance::app
{
    enum class OfficePreviewCacheKind
    {
        word,
        pdf,
    };

    struct OfficePreviewCacheEntry final
    {
        ~OfficePreviewCacheEntry();

        std::wstring source_path;
        std::uint64_t source_size{};
        std::uint64_t source_modified_time{};
        OfficePreviewCacheKind kind{ OfficePreviewCacheKind::pdf };
        std::shared_ptr<OfficePreviewClient> word_client;
        std::wstring pdf_path;
        std::atomic_uint32_t page_count{};
        std::atomic_bool ready{};
    };

    using OfficePreviewCacheHandle = std::shared_ptr<OfficePreviewCacheEntry>;

    void configure_office_preview_cache(const OfficePreviewPreferences& preferences) noexcept;
    [[nodiscard]] OfficePreviewCacheHandle take_office_preview_cache(
        const std::wstring& source_path,
        std::uint64_t source_size,
        std::uint64_t source_modified_time) noexcept;
    void return_office_preview_cache(OfficePreviewCacheHandle entry) noexcept;
    void shutdown_office_preview_cache() noexcept;
}
