#pragma once

#include "glance/contracts/dependency_api.h"
#include "glance/contracts/dependency_runtime.h"
#include "glance/contracts/network_protocol.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace glance::app::dependencies
{
    struct File
    {
        std::wstring archive_path, installed_path, sha256;
        bool operator==(const File&) const = default;
    };
    struct Entry
    {
        std::wstring id, installed_path, external_file_name;
        contracts::dependencies::EntryKind kind{};
        std::wstring legacy_relative_path;
        bool operator==(const Entry&) const = default;
    };
    struct Definition
    {
        std::wstring id, version, display_name, url, archive_name, sha256;
        std::uint64_t archive_size{};
        std::vector<File> files;
        std::vector<Entry> entries;
        bool operator==(const Definition&) const = default;
    };
    struct Snapshot
    {
        Definition definition;
        std::vector<std::wstring> consumers;
        contracts::dependencies::Availability availability{};
    };
    struct Lease
    {
        std::filesystem::path path;
        std::shared_ptr<void> guard;
    };
    struct TransferState
    {
        contracts::dependencies::Availability availability{contracts::dependencies::Availability::downloading};
        std::uint64_t downloaded{}, total{};
        HRESULT error{S_OK};
        bool complete{};
    };
    class Transfer
    {
    public:
        ~Transfer();
        [[nodiscard]] TransferState state() const;
        void cancel() noexcept;
        void subscribe(std::function<void()> changed);
        void publish(TransferState value);
        std::atomic_bool cancellation{};
        std::thread worker;
    private:
        mutable std::mutex mutex_;
        TransferState state_;
        std::vector<std::function<void()>> observers_;
    };
    using Downloader = std::function<contracts::NetworkDownloadResult(
        const contracts::NetworkDownloadRequest&, const std::atomic_bool&,
        const std::function<void(std::uint64_t, std::uint64_t)>&)>;

    [[nodiscard]] std::filesystem::path storage_root();
    [[nodiscard]] std::vector<Snapshot> snapshot();
    [[nodiscard]] Lease acquire(std::wstring_view dependency_id, std::wstring_view entry_id);
    void install_archive(std::wstring_view dependency_id, const std::filesystem::path& archive,
        const std::function<bool()>& cancelled = {});
    void uninstall(std::wstring_view dependency_id);
    [[nodiscard]] std::shared_ptr<Transfer> begin_install(std::wstring_view dependency_id, Downloader download);
    [[nodiscard]] std::shared_ptr<Transfer> begin_install(std::wstring_view dependency_id);
    void configure_downloader(Downloader download);
    void shutdown();
    void migrate_legacy_installations();
    void unregister_consumer(std::wstring_view consumer);
    void register_media_dependency();
    [[nodiscard]] std::shared_ptr<Transfer> transfer(std::wstring_view dependency_id);
    [[nodiscard]] const contracts::dependencies::HostApi& host_api();
}
