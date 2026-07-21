#include "core_application.h"
#include "glance/contracts/diagnostics.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    glance::contracts::initialize_diagnostics(L"Glance.Core");
    glance::core::CoreApplication application;
    return application.run(instance);
}
