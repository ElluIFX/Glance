#include "input_decision.h"
#include "glance/contracts/component_api.h"
#include "pan_interaction.h"
#include "text_font_fallback.h"
#include "../../src/version.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    int failures{};

    void expect(bool condition, std::string_view name)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << name << '\n';
            ++failures;
        }
    }
}

int main()
{
    using glance::core::should_capture_key;

    expect(should_capture_key(VK_SPACE, true, false, true, false, false), "eligible Space");
    expect(should_capture_key(VK_SPACE, true, true, false, false, false), "active Space");
    expect(!should_capture_key(VK_SPACE, false, false, true, false, false), "disconnected Space");
    expect(!should_capture_key(VK_SPACE, true, false, false, false, false), "ineligible Space");
    expect(!should_capture_key(VK_SPACE, true, false, true, false, true), "modified Space");
    expect(!should_capture_key(VK_SPACE, true, false, true, true, false), "eligible text input Space");
    expect(!should_capture_key(VK_SPACE, true, true, false, true, false), "active text input Space");

    expect(should_capture_key(VK_ESCAPE, true, true, false, true, false), "active Escape");
    expect(!should_capture_key(VK_ESCAPE, true, false, true, false, false), "inactive Escape");
    expect(!should_capture_key(VK_ESCAPE, true, true, false, false, true), "modified Escape");
    expect(!should_capture_key('A', true, true, true, false, false), "unrelated key");

    expect(!glance::app::zoom_allows_pan(1.0F), "fit zoom does not pan");
    expect(!glance::app::zoom_allows_pan(1.001F), "zoom tolerance does not pan");
    expect(glance::app::zoom_allows_pan(1.01F), "enlarged preview pans");

    const auto offsets = glance::app::calculate_pan_offsets(
        { 120.0, 80.0 },
        { 300.0, 200.0 },
        { 260.0, 230.0 });
    expect(std::abs(offsets.horizontal - 160.0) < 0.001, "horizontal pan offset");
    expect(std::abs(offsets.vertical - 50.0) < 0.001, "vertical pan offset");

    const auto clamped_offsets = glance::app::calculate_pan_offsets(
        { 10.0, 10.0 },
        { 20.0, 20.0 },
        { 100.0, 100.0 });
    expect(clamped_offsets.horizontal == 0.0, "horizontal pan clamp");
    expect(clamped_offsets.vertical == 0.0, "vertical pan clamp");

    const std::vector<std::wstring> fonts_without_cascadia{
        L"Arial",
        L"consolas",
        L"Courier New"
    };
    expect(
        glance::app::select_default_text_font_family(fonts_without_cascadia) == L"consolas",
        "font fallback prefers Consolas");

    const std::vector<std::wstring> fonts_without_consolas{
        L"Arial",
        L"Courier New",
        L"Lucida Console"
    };
    expect(
        glance::app::select_default_text_font_family(fonts_without_consolas) == L"Courier New",
        "font fallback prefers Courier New");

    const std::vector<std::wstring> no_fonts;
    expect(
        glance::app::select_default_text_font_family(no_fonts) == L"Cascadia Mono",
        "font fallback keeps primary default");

    std::wstring executable_path(32768, L'\0');
    const DWORD executable_length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    executable_path.resize(executable_length);
    const auto component_directory =
        std::filesystem::path(executable_path).parent_path() / L"components" / L"office";
    const auto component_path = component_directory / L"Glance.OfficeComponent.dll";
    const auto host_path = component_directory / L"Glance.OfficeHost.exe";
    const auto descriptor_path = component_directory / L"component.json";
    expect(std::filesystem::is_regular_file(component_path), "Office component DLL output");
    expect(std::filesystem::is_regular_file(host_path), "Office component host output");
    expect(std::filesystem::is_regular_file(descriptor_path), "Office component descriptor output");

    std::ifstream descriptor_input(descriptor_path, std::ios::binary);
    const std::string descriptor{
        std::istreambuf_iterator<char>(descriptor_input),
        std::istreambuf_iterator<char>() };
    expect(
        descriptor.find("\"id\": \"office\"") != std::string::npos,
        "Office descriptor id");
    expect(
        descriptor.find("\"version\"") == std::string::npos,
        "Office descriptor has no independent version");

    const HMODULE component = LoadLibraryExW(
        component_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    expect(component != nullptr, "load Office component DLL");
    if (component != nullptr)
    {
        using namespace glance::contracts::components;
        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(component, get_api_export));
        expect(get_api != nullptr, "Office component API export");
        if (get_api != nullptr)
        {
            ComponentApi api;
            expect(get_api(abi_version, &api) != FALSE, "Office component ABI negotiation");
            expect(std::wstring_view(api.component_id) == L"office", "Office component API id");
            expect(
                std::wstring_view(api.target_app_version) == GLANCE_VERSION_WSTRING,
                "Office component target app version");
            expect(api.output_kind == PreviewOutputKind::pdf_file, "Office component output kind");
            expect(api.query_health != nullptr, "Office component health function");
            expect(api.can_preview != nullptr, "Office component preview function");
            expect(api.prepare_preview != nullptr, "Office component prepare function");
            expect(api.shutdown != nullptr, "Office component shutdown function");
            if (api.query_health != nullptr)
            {
                HealthResult health;
                expect(
                    api.query_health(L"en-US", &health) != FALSE,
                    "Office component health query");
                expect(health.detail[0] != L'\0', "Office component health detail");
            }
            api.shutdown();
        }
        FreeLibrary(component);
    }

    if (failures == 0)
    {
        std::cout << "All regression tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
