#include "pch.h"
#include "window_size_store.h"
#include <cstdint>

namespace
{
    constexpr wchar_t size_registry_path[] = L"Software\\Glance\\WindowSizes";
    constexpr wchar_t position_registry_path[] = L"Software\\Glance\\WindowPositions";

    std::optional<std::uint64_t> read(const wchar_t* path, const std::wstring& name)
    {
        std::uint64_t value{};
        DWORD bytes = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, path, name.c_str(), RRF_RT_REG_QWORD,
            nullptr, &value, &bytes) != ERROR_SUCCESS) return std::nullopt;
        return value;
    }
    void write(const wchar_t* path, const std::wstring& name, LONG x, LONG y) noexcept
    {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        const std::uint64_t value = (std::uint64_t(static_cast<std::uint32_t>(x)) << 32U) |
            static_cast<std::uint32_t>(y);
        RegSetValueExW(key, name.c_str(), 0, REG_QWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
    bool clear(const wchar_t* path) noexcept
    {
        const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, path);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }
}

namespace glance::app
{
    WindowPlacementMemory load_window_placement(const WindowPlacementIdentity& identity)
    {
        WindowPlacementMemory memory;
        if (identity.key.empty()) return memory;
        if (const auto value = read(size_registry_path, identity.key))
        {
            const SIZE size{static_cast<LONG>(*value >> 32U), static_cast<LONG>(*value & 0xffffffffU)};
            if (size.cx > 0 && size.cy > 0) memory.size = size;
        }
        if (const auto value = read(position_registry_path, L"CenterOffset." + identity.key))
            memory.center_offset = POINT{
                static_cast<LONG>(static_cast<std::int32_t>(*value >> 32U)),
                static_cast<LONG>(static_cast<std::int32_t>(*value & 0xffffffffU))};
        return memory;
    }
    void save_window_placement(const WindowPlacementIdentity& identity, const WindowPlacementMemory& memory) noexcept
    {
        if (identity.key.empty()) return;
        if (memory.size && memory.size->cx > 0 && memory.size->cy > 0)
            write(size_registry_path, identity.key, memory.size->cx, memory.size->cy);
        if (memory.center_offset)
            write(position_registry_path, L"CenterOffset." + identity.key,
                memory.center_offset->x, memory.center_offset->y);
    }
    bool clear_window_sizes() noexcept { return clear(size_registry_path); }
    bool clear_window_positions() noexcept { return clear(position_registry_path); }
}
