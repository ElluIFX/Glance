#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace glance::contracts::dependencies
{
    enum class ExecutionStatus { completed, cancelled, timed_out, output_limit, failed };

    struct ExecutionOptions
    {
        std::uint32_t timeout_ms{10000};
        std::size_t maximum_output_bytes{8 * 1024 * 1024};
        std::function<bool()> cancelled;
    };

    struct ExecutionResult
    {
        ExecutionStatus status{ExecutionStatus::failed};
        DWORD error{};
        DWORD exit_code{STILL_ACTIVE};
        std::string output;
        std::string errors;
        [[nodiscard]] bool succeeded() const noexcept
        { return status == ExecutionStatus::completed && exit_code == 0; }
    };

    [[nodiscard]] std::wstring quote_argument(std::wstring_view value);
    // Blocking; invoke on a worker thread. Arguments are individual argv entries.
    [[nodiscard]] ExecutionResult execute(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        const ExecutionOptions& options = {});

    class Library
    {
    public:
        explicit Library(const std::filesystem::path& path);
        ~Library();
        Library(const Library&) = delete;
        Library& operator=(const Library&) = delete;
        Library(Library&& other) noexcept;
        Library& operator=(Library&& other) noexcept;
        [[nodiscard]] FARPROC symbol(const char* name) const noexcept;
    private:
        HMODULE module_{};
    };
}
