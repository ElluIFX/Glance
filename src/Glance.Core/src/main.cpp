#include "core_application.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    glance::core::CoreApplication application;
    return application.run(instance);
}
