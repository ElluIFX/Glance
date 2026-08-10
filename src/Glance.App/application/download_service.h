#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace glance::app
{
    enum class FileDownloadStatus
    {
        succeeded,
        cancelled,
        network_error,
        file_error,
        integrity_error,
    };

    struct FileDownloadRequest
    {
        std::wstring url;
        std::filesystem::path destination_path;
        std::wstring sha256;
        std::uint64_t expected_size{};
        std::uint64_t maximum_size{};
    };

    struct FileDownloadResult
    {
        FileDownloadStatus status{ FileDownloadStatus::network_error };
        std::filesystem::path path;
    };

    using FileDownloadProgressCallback =
        std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

    [[nodiscard]] FileDownloadResult download_file(
        const FileDownloadRequest& request,
        const std::atomic_bool& cancelled,
        const FileDownloadProgressCallback& progress) noexcept;
}
