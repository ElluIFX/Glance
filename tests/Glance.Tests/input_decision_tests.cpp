#include "input_decision.h"
#include "glance/contracts/component_api.h"
#include "glance/contracts/ipc_protocol.h"
#include "media_preview_preferences.h"
#include "pan_interaction.h"
#include "pdf_render_client.h"
#include "text_font_fallback.h"
#include "../../src/version.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
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

    void append_be16(std::vector<unsigned char>& bytes, std::uint16_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value >> 8));
        bytes.push_back(static_cast<unsigned char>(value));
    }

    void append_be32(std::vector<unsigned char>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value >> 24));
        bytes.push_back(static_cast<unsigned char>(value >> 16));
        bytes.push_back(static_cast<unsigned char>(value >> 8));
        bytes.push_back(static_cast<unsigned char>(value));
    }

    std::vector<unsigned char> make_psd_thumbnail_fixture()
    {
        static constexpr std::array<unsigned char, 6> jpeg{
            0xFF, 0xD8, 0xFF, 0xD9, 0x00, 0x00 };
        std::vector<unsigned char> resource;
        resource.insert(resource.end(), { '8', 'B', 'I', 'M' });
        append_be16(resource, 1036);
        resource.insert(resource.end(), { 0, 0 });
        append_be32(resource, 28 + static_cast<std::uint32_t>(jpeg.size()));
        append_be32(resource, 1);
        append_be32(resource, 1);
        append_be32(resource, 1);
        append_be32(resource, 4);
        append_be32(resource, 4);
        append_be32(resource, static_cast<std::uint32_t>(jpeg.size()));
        append_be16(resource, 24);
        append_be16(resource, 1);
        resource.insert(resource.end(), jpeg.begin(), jpeg.end());

        std::vector<unsigned char> file;
        file.insert(file.end(), { '8', 'B', 'P', 'S' });
        append_be16(file, 1);
        file.insert(file.end(), 6, 0);
        append_be16(file, 3);
        append_be32(file, 1);
        append_be32(file, 1);
        append_be16(file, 8);
        append_be16(file, 3);
        append_be32(file, 0);
        append_be32(file, static_cast<std::uint32_t>(resource.size()));
        file.insert(file.end(), resource.begin(), resource.end());
        append_be32(file, 0);
        append_be16(file, 0);
        file.insert(file.end(), { 0x20, 0x80, 0xE0 });
        return file;
    }

    bool write_bytes(
        const std::filesystem::path& path,
        const std::vector<unsigned char>& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    std::vector<unsigned char> make_pdf_fixture()
    {
        std::string pdf = "%PDF-1.4\n";
        std::array<std::size_t, 5> offsets{};
        const auto append_object = [&](std::size_t number, std::string_view body) {
            offsets[number] = pdf.size();
            pdf += std::to_string(number) + " 0 obj\n";
            pdf.append(body);
            pdf += "\nendobj\n";
        };
        append_object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        append_object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        append_object(
            3,
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
            "/Resources << >> /Contents 4 0 R >>");
        append_object(4, "<< /Length 0 >>\nstream\n\nendstream");

        const auto xref_offset = pdf.size();
        std::ostringstream xref;
        xref << "xref\n0 5\n0000000000 65535 f \n";
        for (std::size_t index = 1; index < offsets.size(); ++index)
        {
            xref << std::setw(10) << std::setfill('0') << offsets[index]
                 << " 00000 n \n";
        }
        xref << "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n"
             << xref_offset << "\n%%EOF\n";
        pdf += xref.str();
        return { pdf.begin(), pdf.end() };
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

    using glance::contracts::heartbeat_acknowledged;
    expect(heartbeat_acknowledged(1, 1), "heartbeat ack matches pending");
    expect(heartbeat_acknowledged(2, 1), "heartbeat ack may lag one round");
    expect(heartbeat_acknowledged(1, 0), "heartbeat ack may cover the previous round");
    expect(heartbeat_acknowledged(10, 10), "heartbeat ack equals pending");
    expect(!heartbeat_acknowledged(10, 8), "heartbeat ack lagging two rounds fails");
    expect(!heartbeat_acknowledged(5, 0), "heartbeat without any ack fails");
    expect(!heartbeat_acknowledged(100, 98), "heartbeat ack two rounds behind fails");
    expect(
        glance::contracts::process_watchdog_interval_ms == 500 &&
            glance::contracts::process_watchdog_failure_limit == 4,
        "watchdog detects a stalled peer within two seconds");
    expect(
        glance::contracts::process_watchdog_connect_grace_ms >= 10000,
        "watchdog keeps a long connection grace for slow startup");

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

    expect(
        glance::app::normalize_rich_document_render_dimension(1024) == 1024 &&
            glance::app::normalize_rich_document_render_dimension(2048) == 2048 &&
            glance::app::normalize_rich_document_render_dimension(4096) == 4096 &&
            glance::app::normalize_rich_document_render_dimension(8192) == 8192,
        "rich document render dimensions");
    expect(
        glance::app::normalize_rich_document_render_dimension(0) == 4096 &&
            glance::app::normalize_rich_document_render_dimension(16384) == 4096,
        "rich document render dimension fallback");

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
                    ? L"Supports previewing Word, PowerPoint, and Excel files"
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
                    ? L"支持预览 Word、PowerPoint 与 Excel 文件"
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

    const auto adobe_directory =
        std::filesystem::path(executable_path).parent_path() / L"components" / L"adobe";
    const auto adobe_component_path =
        adobe_directory / L"Glance.AdobeComponent.dll";
    const auto adobe_host_path = adobe_directory / L"Glance.AdobeHost.exe";
    const auto adobe_descriptor_path = adobe_directory / L"component.json";
    const auto adobe_resource_path = adobe_directory / L"resources.pri";
    expect(
        std::filesystem::is_regular_file(adobe_component_path),
        "Adobe component DLL output");
    expect(
        std::filesystem::is_regular_file(adobe_host_path),
        "Adobe component host output");
    expect(
        std::filesystem::is_regular_file(adobe_descriptor_path),
        "Adobe component descriptor output");
    expect(
        std::filesystem::is_regular_file(adobe_resource_path),
        "Adobe component resource output");
    std::ifstream adobe_descriptor_input(adobe_descriptor_path, std::ios::binary);
    const std::string adobe_descriptor{
        std::istreambuf_iterator<char>(adobe_descriptor_input),
        std::istreambuf_iterator<char>() };
    expect(
        adobe_descriptor.find("\"id\": \"adobe\"") != std::string::npos &&
            adobe_descriptor.find("\"Glance.AdobeHost.exe\"") != std::string::npos &&
            adobe_descriptor.find("\"resources.pri\"") != std::string::npos &&
            adobe_descriptor.find("\"extensions\"") == std::string::npos,
        "Adobe component descriptor");

    const HMODULE adobe_component = LoadLibraryExW(
        adobe_component_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32);
    expect(adobe_component != nullptr, "load Adobe component DLL");
    if (adobe_component != nullptr)
    {
        using namespace glance::contracts::components;
        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(adobe_component, get_api_export));
        ComponentApi api;
        expect(
            get_api != nullptr && get_api(abi_version, &api) != FALSE,
            "Adobe component ABI negotiation");
        if (get_api != nullptr && api.initialize != nullptr)
        {
            std::vector<std::wstring> extensions;
            ComponentRegistrar registrar{
                .context = &extensions,
                .register_extension = collect_extension };
            ComponentRegistration registration;
            expect(
                api.initialize(&registrar, &registration) != FALSE,
                "Adobe component registration");
            expect(
                std::wstring_view(registration.component_id) == L"adobe",
                "Adobe component API id");
            expect(
                std::wstring_view(registration.target_app_version) ==
                    GLANCE_VERSION_WSTRING,
                "Adobe component target app version");
            expect(
                extensions == std::vector<std::wstring>{ L".psd", L".psb", L".ai" },
                "Adobe component extensions");

            ComponentStatusResult status;
            expect(
                api.query_status(L"zh-CN", &status) != FALSE &&
                    std::wstring_view(status.display_name) == L"Adobe 文档预览" &&
                    status.detail[0] != L'\0',
                "Adobe component localized status");

            ComponentLoadingTextResult loading;
            expect(
                api.query_loading_text(
                    L"C:\\GlanceComponentTest\\sample.psd",
                    L"zh-CN",
                    &loading) != FALSE &&
                    std::wstring_view(loading.text) ==
                        L"正在后台准备 Adobe 文档预览",
                "Adobe component localized loading text");

            void* configurable_pointer{};
            expect(
                api.query_interface(
                    &configurable_preview_api_id,
                    configurable_preview_api_version,
                    &configurable_pointer) != FALSE &&
                    configurable_pointer != nullptr,
                "Adobe configurable preview interface");
            const auto configurable =
                static_cast<ConfigurablePreviewApi*>(configurable_pointer);
            void* progressive_pointer{};
            expect(
                api.query_interface(
                    &progressive_preview_api_id,
                    progressive_preview_api_version,
                    &progressive_pointer) != FALSE &&
                    progressive_pointer != nullptr,
                "Adobe progressive preview interface");
            const auto progressive =
                static_cast<ProgressivePreviewApi*>(progressive_pointer);
            void* notice_pointer{};
            expect(
                api.query_interface(
                    &preview_notice_api_id,
                    preview_notice_api_version,
                    &notice_pointer) != FALSE &&
                    notice_pointer != nullptr,
                "Adobe preview notice interface");
            const auto preview_notice =
                static_cast<PreviewNoticeApi*>(notice_pointer);
            void* unsupported_pointer = reinterpret_cast<void*>(1);
            expect(
                api.query_interface(
                    &progressive_preview_api_id,
                    progressive_preview_api_version + 1,
                    &unsupported_pointer) == FALSE &&
                    unsupported_pointer == nullptr,
                "Adobe rejects unsupported interface version");

            const auto test_directory =
                std::filesystem::temp_directory_path() /
                L"GlanceAdobeComponentTests";
            std::error_code cleanup_error;
            std::filesystem::remove_all(test_directory, cleanup_error);
            std::filesystem::create_directories(test_directory);

            const auto psd_path = test_directory / L"embedded.psd";
            expect(
                write_bytes(psd_path, make_psd_thumbnail_fixture()),
                "write PSD thumbnail fixture");
            expect(
                api.can_preview(psd_path.c_str()) != FALSE,
                "Adobe component accepts embedded PSD preview");
            PreparedPreview psd_preview;
            PreviewPreparationOptions preview_options{
                .maximum_dimension = 1024 };
            expect(
                configurable != nullptr &&
                    configurable->prepare_preview(
                    psd_path.c_str(),
                    L"en-US",
                    &preview_options,
                    &psd_preview) == PrepareStatus::success &&
                    psd_preview.kind == PreviewContentKind::image &&
                    psd_preview.format == PreviewContentFormat::image_file &&
                    psd_preview.lease_token != 0 &&
                    std::filesystem::is_regular_file(psd_preview.path),
                "Adobe component extracts PSD thumbnail");
            const std::filesystem::path extracted_path{ psd_preview.path };

            ComponentLoadingTextResult refinement_text;
            expect(
                progressive != nullptr &&
                    progressive->can_refine(psd_preview.lease_token) != FALSE &&
                    progressive->query_refinement_text(
                        psd_preview.lease_token,
                        L"zh-CN",
                        &refinement_text) != FALSE &&
                    std::wstring_view(refinement_text.text) ==
                        L"正在准备高清预览",
                "Adobe component refinement availability");
            PreparedPreview refined_preview;
            expect(
                progressive != nullptr &&
                    progressive->prepare_refined_preview(
                        psd_preview.lease_token,
                        L"en-US",
                        &preview_options,
                        &refined_preview) == PrepareStatus::success &&
                    refined_preview.kind == PreviewContentKind::image &&
                    refined_preview.format == PreviewContentFormat::image_file &&
                    refined_preview.lease_token == 0 &&
                    std::filesystem::is_regular_file(refined_preview.path),
                "Adobe component prepares high-resolution PSD preview");
            if (std::filesystem::is_regular_file(refined_preview.path))
            {
                std::ifstream refined_input(
                    refined_preview.path,
                    std::ios::binary);
                std::array<unsigned char, 8> png_signature{};
                refined_input.read(
                    reinterpret_cast<char*>(png_signature.data()),
                    static_cast<std::streamsize>(png_signature.size()));
                expect(
                    png_signature == std::array<unsigned char, 8>{
                        0x89, 0x50, 0x4E, 0x47,
                        0x0D, 0x0A, 0x1A, 0x0A },
                    "Adobe refined preview PNG");
            }
            api.release_preview(psd_preview.lease_token);
            expect(
                !std::filesystem::exists(extracted_path),
                "Adobe component releases extracted preview");
            PreparedPreview cached_preview;
            expect(
                configurable != nullptr &&
                    configurable->prepare_preview(
                        psd_path.c_str(),
                        L"en-US",
                        &preview_options,
                        &cached_preview) == PrepareStatus::success &&
                    cached_preview.lease_token == 0 &&
                    std::filesystem::path(cached_preview.path) ==
                        std::filesystem::path(refined_preview.path),
                "Adobe component reuses high-resolution preview cache");

            const auto psb_path = test_directory / L"embedded.psb";
            auto psb_fixture = make_psd_thumbnail_fixture();
            psb_fixture[5] = 2;
            expect(
                write_bytes(psb_path, psb_fixture) &&
                    api.can_preview(psb_path.c_str()) != FALSE,
                "Adobe component accepts embedded PSB preview");
            PreparedPreview psb_preview;
            expect(
                api.prepare_preview(
                    psb_path.c_str(),
                    L"zh-CN",
                    &psb_preview) == PrepareStatus::success &&
                    psb_preview.lease_token != 0 &&
                    progressive->can_refine(psb_preview.lease_token) == FALSE,
                "Adobe component limits PSB to embedded preview");
            ComponentLoadingTextResult psb_notice;
            expect(
                preview_notice != nullptr &&
                    preview_notice->query_preview_notice(
                        psb_preview.lease_token,
                        L"zh-CN",
                        &psb_notice) != FALSE &&
                    std::wstring_view(psb_notice.text) ==
                        L"大型文件只支持低清预览",
                "Adobe component PSB low-resolution notice");
            api.release_preview(psb_preview.lease_token);

            const auto ai_path = test_directory / L"compatible.ai";
            const std::vector<unsigned char> ai_fixture{
                '%', 'P', 'D', 'F', '-', '1', '.', '7', '\n' };
            expect(
                write_bytes(ai_path, ai_fixture) &&
                    api.can_preview(ai_path.c_str()) != FALSE,
                "Adobe component accepts PDF-compatible AI");
            PreparedPreview ai_preview;
            expect(
                api.prepare_preview(
                    ai_path.c_str(),
                    L"en-US",
                    &ai_preview) == PrepareStatus::success &&
                    ai_preview.kind == PreviewContentKind::document &&
                    ai_preview.format == PreviewContentFormat::pdf &&
                    ai_preview.lease_token == 0 &&
                    std::filesystem::path(ai_preview.path) == ai_path,
                "Adobe component passes through PDF-compatible AI");

            const auto invalid_path = test_directory / L"invalid.txt";
            expect(
                write_bytes(invalid_path, {}) &&
                    api.can_preview(invalid_path.c_str()) == FALSE,
                "Adobe component rejects unrelated files");
            std::filesystem::remove_all(test_directory, cleanup_error);
            api.shutdown();
        }
        FreeLibrary(adobe_component);
    }

    {
        const auto test_directory =
            std::filesystem::temp_directory_path() /
            (L"GlancePdfTests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code cleanup_error;
        std::filesystem::remove_all(test_directory, cleanup_error);
        std::filesystem::create_directories(test_directory, cleanup_error);
        const auto pdf_path = test_directory / L"single-page.pdf";
        expect(
            !cleanup_error && write_bytes(pdf_path, make_pdf_fixture()),
            "PDF render fixture");
        if (std::filesystem::is_regular_file(pdf_path))
        {
            glance::app::PdfRenderClient client;
            const auto opened = client.open(pdf_path.wstring(), L"");
            expect(
                opened.status == glance::contracts::pdf::Status::success &&
                    opened.page_count == 1,
                "PDF RenderHost opens document");
            if (opened.status == glance::contracts::pdf::Status::success)
            {
                const auto rendered = client.render(0, 256, 256);
                expect(
                    rendered.status == glance::contracts::pdf::Status::success &&
                        rendered.pixel_width > 0 &&
                        rendered.pixel_height > 0 &&
                        !rendered.pixels.empty(),
                    "PDF RenderHost renders page");
            }
        }
        std::filesystem::remove_all(test_directory, cleanup_error);
    }

    if (failures == 0)
    {
        std::cout << "All regression tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
