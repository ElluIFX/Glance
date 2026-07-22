#include "core_application.h"
#include "glance/contracts/diagnostics.h"

#include <windows.h>

#include <cstdlib>
#include <string_view>

namespace
{
    DWORD app_process_id(PWSTR command_line) noexcept
    {
        constexpr std::wstring_view app_prefix{ L"--app-pid=" };
        constexpr std::wstring_view legacy_prefix{ L"--parent-pid=" };
        if (command_line == nullptr)
        {
            return 0;
        }

        const std::wstring_view arguments(command_line);
        const auto prefix = arguments.starts_with(app_prefix) ? app_prefix : legacy_prefix;
        if (!arguments.starts_with(prefix))
        {
            return 0;
        }

        wchar_t* end{};
        const auto value = _wcstoui64(command_line + prefix.size(), &end, 10);
        return end != command_line + prefix.size() && *end == L'\0' && value <= MAXDWORD
            ? static_cast<DWORD>(value)
            : 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int)
{
    glance::contracts::initialize_diagnostics(L"Glance.Core");
    glance::core::CoreApplication application;
    return application.run(instance, app_process_id(command_line));
}
