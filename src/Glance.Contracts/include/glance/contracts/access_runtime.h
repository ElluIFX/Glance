#pragma once

#include <windows.h>
#include <filesystem>
#include <string>

namespace glance::contracts::access
{
    enum class InstallationMode { installed, portable, unknown };

    [[nodiscard]] std::filesystem::path executable_directory();
    [[nodiscard]] bool same_directory(const std::filesystem::path& left, const std::filesystem::path& right) noexcept;
    [[nodiscard]] bool protected_installation(const std::filesystem::path& directory) noexcept;
    [[nodiscard]] InstallationMode installation_mode() noexcept;
    [[nodiscard]] DWORD start_service() noexcept;
    [[nodiscard]] bool service_process(DWORD process_id) noexcept;
    [[nodiscard]] DWORD configure_service(bool uninstall) noexcept;
    [[nodiscard]] bool transfer(HANDLE pipe, void* buffer, DWORD bytes, bool write, DWORD timeout = 2000, HANDLE cancel = nullptr) noexcept;
    [[nodiscard]] bool process_image(HANDLE process, const std::filesystem::path& expected) noexcept;
    [[nodiscard]] std::wstring process_sid(HANDLE process);
    [[nodiscard]] bool system_process(HANDLE process) noexcept;
}
