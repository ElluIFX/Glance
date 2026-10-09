#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef GLANCE_PORTABLE
#error GlanceDistribution must be defined by the build
#endif

namespace glance::contracts::storage
{
    inline constexpr bool portable = GLANCE_PORTABLE != 0;
    [[nodiscard]] std::filesystem::path application_directory();
    [[nodiscard]] std::filesystem::path installed_data_directory();
    [[nodiscard]] std::filesystem::path data_directory();
    [[nodiscard]] bool binary_matches(const std::filesystem::path &file, bool expected_portable,
                                      std::wstring_view version) noexcept;

    struct OrdinalLess
    {
        bool operator()(const std::wstring &left, const std::wstring &right) const noexcept;
    };
    struct Value
    {
        DWORD type{};
        std::vector<std::uint8_t> bytes;
        bool operator==(const Value &) const = default;
    };
    using Values = std::map<std::wstring, Value, OrdinalLess>;
    using Snapshot = std::map<std::wstring, Values, OrdinalLess>;

    enum class Backend
    {
        registry,
        json
    };
    struct Change
    {
        std::wstring group;
        std::wstring name;
        std::optional<Value> value;
    };

    class SettingsStore
    {
    public:
        SettingsStore(Backend backend, std::filesystem::path file = {},
                      std::wstring registry_root = L"Software\\Glance");
        [[nodiscard]] LSTATUS read(std::wstring_view group, std::wstring_view name, DWORD expected_type, void *data,
                                   DWORD *bytes) noexcept;
        [[nodiscard]] LSTATUS apply(std::span<const Change> changes) noexcept;
        [[nodiscard]] LSTATUS clear(std::wstring_view group) noexcept;
        [[nodiscard]] LSTATUS snapshot(Snapshot &result) noexcept;
        [[nodiscard]] LSTATUS error() const noexcept;
        void invalidate() noexcept;

    private:
        Backend backend_;
        std::filesystem::path file_;
        std::wstring registry_root_;
        mutable std::mutex mutex_;
        Snapshot cache_;
        struct FileStamp
        {
            std::uint64_t modified{}, created{}, size{}, identity{};
            DWORD volume{};
            bool operator==(const FileStamp &) const = default;
        };
        std::optional<FileStamp> stamp_;
        bool loaded_{};
        LSTATUS error_{};
        LSTATUS load_json();
        LSTATUS save_json(const Snapshot &values);
    };

    [[nodiscard]] SettingsStore &settings();
    [[nodiscard]] LSTATUS read_value(std::wstring_view group, std::wstring_view name, DWORD expected_type, void *data,
                                     DWORD *bytes) noexcept;
    [[nodiscard]] DWORD read_dword(std::wstring_view group, std::wstring_view name, DWORD fallback) noexcept;
    [[nodiscard]] std::wstring read_string(std::wstring_view group, std::wstring_view name,
                                           std::wstring_view fallback = {});
    [[nodiscard]] LSTATUS delete_value(std::wstring_view group, std::wstring_view name) noexcept;
    [[nodiscard]] LSTATUS clear_group(std::wstring_view group) noexcept;
    [[nodiscard]] LSTATUS write_snapshot(const std::filesystem::path &file, const Snapshot &values) noexcept;
    [[nodiscard]] LSTATUS read_snapshot(const std::filesystem::path &file, Snapshot &values) noexcept;

    class Batch
    {
    public:
        explicit Batch(std::wstring_view group, SettingsStore &store = settings()) noexcept;
        void set(std::wstring_view name, DWORD type, const void *data, DWORD bytes) noexcept;
        void erase(std::wstring_view name) noexcept;
        [[nodiscard]] LSTATUS commit() noexcept;

    private:
        std::wstring group_;
        std::vector<Change> changes_;
        LSTATUS error_{};
        SettingsStore &store_;
    };
} // namespace glance::contracts::storage
