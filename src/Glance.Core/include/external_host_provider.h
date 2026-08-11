#pragma once

#include "glance/contracts/file_descriptor.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glance::core
{
    struct ExternalHostContext
    {
        HWND root_window{};
        DWORD process_id{};
        DWORD thread_id{};
        GUITHREADINFO thread_info{ sizeof(GUITHREADINFO) };
        std::wstring_view process_name;
        std::wstring_view window_class;
    };

    struct ExternalHostSelection
    {
        bool accepts_hotkey{};
        bool text_input_active{};
        std::wstring source_id;
        std::uint64_t capabilities{};
        std::wstring filesystem_path;
    };

    struct ExternalHostItem
    {
        std::uint64_t item_id{};
        std::wstring filesystem_path;
    };

    struct ExternalHostStatus
    {
        std::wstring source_id;
        std::wstring display_name;
        std::wstring detail;
        std::uint32_t severity{};
        std::uint32_t code{};
        std::uint64_t capabilities{};
    };

    class ExternalHostProviderRegistry final
    {
    public:
        ExternalHostProviderRegistry();
        ~ExternalHostProviderRegistry();

        ExternalHostProviderRegistry(const ExternalHostProviderRegistry&) = delete;
        ExternalHostProviderRegistry& operator=(const ExternalHostProviderRegistry&) = delete;

        [[nodiscard]] std::optional<ExternalHostSelection> query(
            const ExternalHostContext& context);
        [[nodiscard]] bool query_item_count(
            std::wstring_view source_id,
            std::uintptr_t source_window,
            std::uint32_t& item_count,
            std::uint32_t& focused_offset,
            std::uint64_t& focused_item_id);
        [[nodiscard]] std::vector<ExternalHostItem> enumerate_items(
            std::wstring_view source_id,
            std::uintptr_t source_window,
            std::uint32_t offset,
            std::uint32_t limit);
        [[nodiscard]] bool focus_item(
            std::wstring_view source_id,
            std::uintptr_t source_window,
            std::uint64_t item_id,
            std::wstring_view expected_path);
        [[nodiscard]] std::vector<ExternalHostStatus> statuses(std::wstring_view language_tag);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
