#include "../protocol.h"
#include <shellapi.h>
#include <objbase.h>
#include <gdiplus.h>
namespace glance::executable { int inspect(Transfer&, HANDLE) noexcept; }
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 4) { if (arguments) LocalFree(arguments); return 2; }
    const auto mapping = reinterpret_cast<HANDLE>(_wcstoui64(arguments[2], nullptr, 10));
    const auto cancellation = reinterpret_cast<HANDLE>(_wcstoui64(arguments[3], nullptr, 10));
    const bool valid = wcscmp(arguments[1], L"--inspect") == 0;
    LocalFree(arguments);
    if (!valid || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { CoUninitialize(); return 3; }
    auto* transfer = static_cast<glance::executable::Transfer*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(glance::executable::Transfer)));
    const int result = transfer ? glance::executable::inspect(*transfer, cancellation) : 3;
    if (transfer) UnmapViewOfFile(transfer);
    Gdiplus::GdiplusShutdown(token); CoUninitialize(); return result;
}
