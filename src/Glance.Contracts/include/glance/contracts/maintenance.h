#pragma once
#include "storage.h"

namespace glance::contracts::maintenance
{
    struct MigrationOptions
    {
        bool remove_source{};
        std::wstring warning_title;
        std::wstring cleanup_failure_message;
        std::wstring restart_failure_message;
    };

    [[nodiscard]] bool installed_data_available() noexcept;
    void prepare_migration(const std::filesystem::path &application);
    void prepare_update(const std::filesystem::path &application, const std::filesystem::path &archive,
                        std::wstring_view version);
    void launch_worker(const std::filesystem::path &application, const MigrationOptions &options = {});
    void recover(const std::filesystem::path &application);
    [[nodiscard]] bool recovery_required(const std::filesystem::path &application);
    [[nodiscard]] int run_worker(const std::filesystem::path &application, DWORD parent) noexcept;
    [[nodiscard]] DWORD failure(const std::filesystem::path &application);
    // Source cleanup is allowed only while it still matches the verified snapshot.
    void delete_migration_source(const std::filesystem::path &application);
    void finish_migration(const std::filesystem::path &application);
} // namespace glance::contracts::maintenance
