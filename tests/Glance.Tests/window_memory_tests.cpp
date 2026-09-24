#include "window_size_store.h"
#include <iostream>

int run_window_memory_tests()
{
    using namespace glance::app;
    const auto path = L"Software\\Glance\\Tests\\WindowMemory." + std::to_wstring(GetCurrentProcessId());
    HKEY isolated{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &isolated, nullptr) != ERROR_SUCCESS) return 1;
    struct Cleanup
    {
        HKEY root;
        std::wstring path;
        ~Cleanup()
        {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            RegCloseKey(root);
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        }
    } cleanup{isolated, path};
    if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated) != ERROR_SUCCESS) return 1;
    unsigned failures{};
    const auto check = [&](bool result, const char* name) {
        std::cout << (result ? "PASS " : "FAIL ") << name << '\n';
        failures += !result;
    };
    const auto font = WindowPlacementIdentity::component(L"font");
    const auto executable = WindowPlacementIdentity::component(L"executable");
    const auto office = WindowPlacementIdentity::component(L"office");
    const auto web = WindowPlacementIdentity::builtin(PreviewKind::web);
    const auto model = WindowPlacementIdentity::component(L"model3d");
    save_window_placement(font, {SIZE{960, 700}, POINT{-120, 80}});
    save_window_placement(executable, {SIZE{720, 520}, POINT{40, -60}});
    const auto a = load_window_placement(font), b = load_window_placement(executable);
    check(a.size && a.size->cx == 960 && b.size && b.size->cx == 720,
        "component sizes remain independent");
    check(a.center_offset && a.center_offset->x == -120 && b.center_offset && b.center_offset->y == -60,
        "component positions remain independent and preserve negative offsets");
    check(!load_window_placement(office).size, "unseen component does not inherit another document size");
    save_window_placement(web, {SIZE{800, 600}, std::nullopt});
    check(!load_window_placement(model).size, "web component does not inherit built-in web size");
    const auto pending = WindowPlacementIdentity::builtin(PreviewKind::component);
    save_window_placement(pending, {SIZE{999, 999}, POINT{1, 1}});
    check(!load_window_placement(pending).size && !load_window_placement(pending).center_offset,
        "unresolved provider cannot read or write shared memory");
    save_window_placement(font, {std::nullopt, POINT{25, 35}});
    check(load_window_placement(font).size->cx == 960, "position-only update preserves size");
    check(WindowPlacementIdentity::builtin(PreviewKind::media, true) !=
        WindowPlacementIdentity::builtin(PreviewKind::media, false), "audio and video remain independent");
    check(clear_window_sizes() && !load_window_placement(font).size &&
        load_window_placement(font).center_offset.has_value(), "clearing sizes preserves positions");
    check(clear_window_positions() && !load_window_placement(font).center_offset,
        "clearing positions includes component memory");
    return failures ? 1 : 0;
}
