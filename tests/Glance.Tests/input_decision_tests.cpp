#include "input_decision.h"
#include "glance/contracts/component_api.h"
#include "pan_interaction.h"
#include "text_font_fallback.h"
#include "../../src/version.h"

#include <array>
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

    BOOL WINAPI collect_extension(void* context, const wchar_t* extension) noexcept
    {
        if (context == nullptr || extension == nullptr)
        {
            return FALSE;
        }
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(extension);
        return TRUE;
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
    const auto resource_path = component_directory / L"resources.pri";
    expect(std::filesystem::is_regular_file(component_path), "Office component DLL output");
    expect(std::filesystem::is_regular_file(host_path), "Office component host output");
    expect(std::filesystem::is_regular_file(descriptor_path), "Office component descriptor output");
    expect(std::filesystem::is_regular_file(resource_path), "Office component resource output");

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
    expect(
        descriptor.find("\"extensions\"") == std::string::npos,
        "Office descriptor has no extension catalog");
    expect(
        descriptor.find("\"resources.pri\"") != std::string::npos,
        "Office descriptor resource payload");

    const HMODULE component = LoadLibraryExW(
        component_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32);
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
            expect(api.initialize != nullptr, "Office component initialize function");
            expect(api.query_status != nullptr, "Office component status function");
            expect(
                api.query_loading_text != nullptr,
                "Office component loading text function");
            expect(api.can_preview != nullptr, "Office component preview function");
            expect(api.prepare_preview != nullptr, "Office component prepare function");
            expect(api.release_preview != nullptr, "Office component release function");
            expect(api.query_interface != nullptr, "Office component extension function");
            expect(api.shutdown != nullptr, "Office component shutdown function");
            if (api.initialize != nullptr)
            {
                std::vector<std::wstring> extensions;
                ComponentRegistrar registrar{
                    .context = &extensions,
                    .register_extension = collect_extension };
                ComponentRegistration registration;
                expect(
                    api.initialize(&registrar, &registration) != FALSE,
                    "Office component registration");
                expect(
                    std::wstring_view(registration.component_id) == L"office",
                    "Office component API id");
                expect(
                    std::wstring_view(registration.target_app_version) ==
                        GLANCE_VERSION_WSTRING,
                    "Office component target app version");
                expect(
                    registration.preferred_kind == PreviewContentKind::document &&
                        registration.preferred_format == PreviewContentFormat::pdf,
                    "Office component preferred output");
                expect(extensions.size() == 6, "Office component extension count");
            }
            if (api.query_status != nullptr)
            {
                ComponentStatusResult english_status;
                expect(
                    api.query_status(L"en-US", &english_status) != FALSE,
                    "Office component English status query");
                expect(
                    std::wstring_view(english_status.display_name) ==
                        L"Microsoft Office preview",
                    "Office component English display name");
                const std::wstring_view expected_english_detail =
                    english_status.severity == HealthSeverity::healthy
                    ? L"Office COM automation available"
                    : english_status.capability_mask == 0
                        ? L"Office COM automation unavailable"
                        : L"Some Office COM applications are unavailable";
                expect(
                    std::wstring_view(english_status.detail) ==
                        expected_english_detail,
                    "Office component English status detail");

                ComponentStatusResult chinese_status;
                expect(
                    api.query_status(L"zh-CN", &chinese_status) != FALSE,
                    "Office component Chinese status query");
                expect(
                    std::wstring_view(chinese_status.display_name) ==
                        L"Microsoft Office 预览",
                    "Office component Chinese display name");
                const std::wstring_view expected_chinese_detail =
                    chinese_status.severity == HealthSeverity::healthy
                    ? L"Office COM 自动化可用"
                    : chinese_status.capability_mask == 0
                        ? L"未检测到可用的 Office COM 自动化"
                        : L"部分 Office COM 自动化不可用";
                expect(
                    std::wstring_view(chinese_status.detail) ==
                        expected_chinese_detail,
                    "Office component Chinese status detail");

                ComponentStatusResult fallback_status;
                expect(
                    api.query_status(L"not_a_locale", &fallback_status) != FALSE,
                    "Office component fallback status query");
                expect(
                    std::wstring_view(fallback_status.display_name) ==
                        L"Microsoft Office preview",
                    "Office component invalid language fallback");
            }
            if (api.query_loading_text != nullptr)
            {
                static constexpr std::array office_extensions{
                    L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx" };
                for (const auto* extension : office_extensions)
                {
                    ComponentLoadingTextResult loading_text;
                    const auto path = L"C:\\GlanceComponentTest\\sample" +
                        std::wstring(extension);
                    expect(
                        api.query_loading_text(
                            path.c_str(),
                            L"en-US",
                            &loading_text) != FALSE,
                        "Office component loading text query");
                    expect(
                        std::wstring_view(loading_text.text) ==
                            L"Preparing this file preview in the background, you can return later",
                        "Office component English loading text");
                }

                ComponentLoadingTextResult chinese_loading_text;
                expect(
                    api.query_loading_text(
                        L"C:\\GlanceComponentTest\\sample.docx",
                        L"zh-CN",
                        &chinese_loading_text) != FALSE,
                    "Office component Chinese loading text query");
                expect(
                    std::wstring_view(chinese_loading_text.text) ==
                        L"正在后台准备该文件的预览，可稍后返回",
                    "Office component Chinese loading text");

                ComponentLoadingTextResult alias_loading_text;
                expect(
                    api.query_loading_text(
                        L"C:\\GlanceComponentTest\\sample.docx",
                        L"zh",
                        &alias_loading_text) != FALSE,
                    "Office component language alias query");
                expect(
                    std::wstring_view(alias_loading_text.text) ==
                        L"正在后台准备该文件的预览，可稍后返回",
                    "Office component language alias");

                ComponentLoadingTextResult fallback_loading_text;
                expect(
                    api.query_loading_text(
                        L"C:\\GlanceComponentTest\\sample.docx",
                        nullptr,
                        &fallback_loading_text) != FALSE,
                    "Office component default language query");
                expect(
                    std::wstring_view(fallback_loading_text.text) ==
                        L"Preparing this file preview in the background, you can return later",
                    "Office component default language fallback");
                expect(
                    api.query_loading_text(
                        nullptr,
                        L"en-US",
                        &chinese_loading_text) == FALSE,
                    "Office component loading text rejects null path");
            }
            if (api.query_interface != nullptr)
            {
                void* interface_pointer = reinterpret_cast<void*>(1);
                const GUID unknown_interface{};
                expect(
                    api.query_interface(
                        &unknown_interface,
                        1,
                        &interface_pointer) == FALSE &&
                        interface_pointer == nullptr,
                    "Office component unknown extension interface");
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
