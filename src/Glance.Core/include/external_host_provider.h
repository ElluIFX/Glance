#pragma once

#include "glance/contracts/file_descriptor.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
        glance::contracts::HostKind host_kind{ glance::contracts::HostKind::unsupported };
        bool accepts_hotkey{};
        std::wstring filesystem_path;
    };

    class ExternalHostProviderRegistry final
    {
    public:
        ExternalHostProviderRegistry();
        ~ExternalHostProviderRegistry();

        ExternalHostProviderRegistry(const ExternalHostProviderRegistry&) = delete;
        ExternalHostProviderRegistry& operator=(const ExternalHostProviderRegistry&) = delete;

        [[nodiscard]] std::optional<ExternalHostSelection> query(const ExternalHostContext& context);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
