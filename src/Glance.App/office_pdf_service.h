#pragma once

#include <string>

namespace glance::app
{
    enum class OfficePdfStatus
    {
        success,
        unavailable,
        conversion_failed,
        cancelled,
    };

    struct OfficePdfResult
    {
        OfficePdfStatus status{ OfficePdfStatus::unavailable };
        std::wstring cache_key;
        std::wstring pdf_path;
    };

    [[nodiscard]] OfficePdfResult prepare_office_pdf(const std::wstring& source_path);
    void shutdown_office_pdf_service() noexcept;
}
