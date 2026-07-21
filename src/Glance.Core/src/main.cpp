#include "core_application.h"
#include "glance/contracts/diagnostics.h"

#include <windows.h>

#include <cstdlib>
#include <string_view>

namespace
{
    DWORD parent_process_id(PWSTR command_line) noexcept
    {
        constexpr std::wstring_view prefix{ L"--parent-pid=" };
        if (command_line == nullptr || !std::wstring_view(command_line).starts_with(prefix))
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
    return application.run(instance, parent_process_id(command_line));
}
