#include "input_decision.h"
#include "glance/contracts/component_api.h"
#include "glance/contracts/ipc_protocol.h"
#include "glance/contracts/network_protocol.h"
#include "glance/contracts/source_api.h"
#include "gallery_navigation.h"
#include "footer_preferences.h"
#include "fullscreen_interaction.h"
#include "media_preview_preferences.h"
#include "office_preview_benchmark.h"
#include "pan_interaction.h"
#include "paged_document_render_client.h"
#include "text_font_fallback.h"
#include "update_preferences.h"
#include "../../src/version.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ranges>
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

    BOOL WINAPI accept_renderer(
        void*,
        glance::contracts::components::PreviewContentKind kind,
        glance::contracts::components::PreviewContentFormat format,
        const GUID* interface_id,
        std::uint32_t interface_version) noexcept
    {
        if (interface_id == nullptr)
        {
            return FALSE;
        }
        return (kind == glance::contracts::components::PreviewContentKind::document &&
                format == glance::contracts::components::PreviewContentFormat::pdf &&
                IsEqualGUID(
                    *interface_id,
                    glance::contracts::components::paged_document_renderer_api_id) &&
                interface_version ==
                    glance::contracts::components::paged_document_renderer_api_version) ||
            (kind == glance::contracts::components::PreviewContentKind::document &&
             format == glance::contracts::components::PreviewContentFormat::native_surface &&
             IsEqualGUID(
                 *interface_id,
                 glance::contracts::components::native_preview_renderer_api_id) &&
             interface_version ==
                 glance::contracts::components::native_preview_renderer_api_version) ||
            (kind == glance::contracts::components::PreviewContentKind::directory &&
             format ==
                 glance::contracts::components::PreviewContentFormat::file_directory &&
             IsEqualGUID(
                 *interface_id,
                 glance::contracts::components::file_directory_preview_api_id) &&
             interface_version ==
                 glance::contracts::components::file_directory_preview_api_version);
    }

    struct ImageCodecComponentCase
    {
        const wchar_t* id;
        const wchar_t* component_file;
        const wchar_t* host_file;
        std::vector<std::wstring> extensions;
        const wchar_t* loading_extension;
        const wchar_t* loading_text;
        std::vector<std::wstring> payload_files;
    };

    void test_image_codec_component(
        const std::filesystem::path& component_root,
        const ImageCodecComponentCase& test_case)
    {
        using namespace glance::contracts::components;

        const std::wstring id{ test_case.id };
        std::string label;
        label.reserve(id.size());
        for (const auto character : id)
        {
            label.push_back(static_cast<char>(character));
        }
        const auto check = [&label](bool condition, std::string_view description)
        {
            expect(condition, label + " component " + std::string(description));
        };
        const auto directory = component_root / id;
        const auto component_path = directory / test_case.component_file;
        const auto descriptor_path = directory / L"component.json";
        check(std::filesystem::is_regular_file(component_path), "DLL output");
        check(
            std::filesystem::is_regular_file(directory / test_case.host_file),
            "host output");
        check(std::filesystem::is_regular_file(descriptor_path), "descriptor output");
        for (const auto& payload_file : test_case.payload_files)
        {
            check(
                std::filesystem::is_regular_file(directory / payload_file),
                "runtime payload");
        }

        std::ifstream descriptor_input(descriptor_path, std::ios::binary);
        const std::string descriptor{
            std::istreambuf_iterator<char>(descriptor_input),
            std::istreambuf_iterator<char>() };
        check(
            descriptor.find("\"id\": \"" + label + "\"") != std::string::npos &&
                descriptor.find("\"extensions\"") == std::string::npos,
            "descriptor");

        const HMODULE component = LoadLibraryExW(
            component_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(component != nullptr, "load");
        if (component == nullptr)
        {
            return;
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(component, get_api_export));
        ComponentApi api;
        check(
            get_api != nullptr && get_api(abi_version, &api) != FALSE,
            "ABI negotiation");
        if (get_api == nullptr || api.initialize == nullptr)
        {
            FreeLibrary(component);
            return;
        }

        std::vector<std::wstring> extensions;
        ComponentRegistrar registrar{
            .context = &extensions,
            .register_extension = collect_extension,
            .register_renderer = accept_renderer };
        ComponentRegistration registration;
        const bool initialized = api.initialize(&registrar, &registration) != FALSE;
        check(initialized, "registration");
        if (!initialized)
        {
            FreeLibrary(component);
            return;
        }
        check(
            std::wstring_view(registration.component_id) == id &&
                std::wstring_view(registration.target_app_version) ==
                    GLANCE_VERSION_WSTRING &&
                registration.preferred_kind == PreviewContentKind::image &&
                registration.preferred_format == PreviewContentFormat::image_file &&
                extensions == test_case.extensions,
            "registration contract");

        ComponentStatusResult status;
        check(
            api.query_status != nullptr &&
                api.query_status(L"zh-CN", &status) != FALSE &&
                status.severity == HealthSeverity::healthy &&
                status.display_name[0] != L'\0' && status.detail[0] != L'\0',
            "localized status");

        ComponentLoadingTextResult loading;
        const auto preview_path = std::wstring{ L"C:\\GlanceComponentTest\\sample" } +
            test_case.loading_extension;
        check(
            api.query_loading_text != nullptr &&
                api.query_loading_text(
                    preview_path.c_str(), L"zh-CN", &loading) != FALSE &&
                std::wstring_view(loading.text) == test_case.loading_text &&
                std::wstring_view(loading.text).ends_with(L"..."),
            "localized loading text");
        check(
            api.can_preview != nullptr &&
                api.can_preview(preview_path.c_str()) != FALSE,
            "extension match");

        void* gallery_pointer{};
        check(
            api.query_interface != nullptr &&
                api.query_interface(
                    &gallery_media_api_id,
                    gallery_media_api_version,
                    &gallery_pointer) != FALSE &&
                gallery_pointer != nullptr,
            "gallery interface");
        if (gallery_pointer != nullptr)
        {
            const auto gallery = static_cast<const GalleryMediaApi*>(gallery_pointer);
            bool classifications_match =
                gallery->classify_extension(L".txt") == GalleryMediaKind::none;
            for (const auto& extension : test_case.extensions)
            {
                classifications_match = classifications_match &&
                    gallery->classify_extension(extension.c_str()) ==
                        GalleryMediaKind::image;
            }
            check(classifications_match, "gallery classification");
        }

        void* image_metadata_pointer{};
        check(
            api.query_interface != nullptr &&
                api.query_interface(
                    &image_metadata_api_id,
                    image_metadata_api_version,
                    &image_metadata_pointer) != FALSE &&
                image_metadata_pointer != nullptr &&
                static_cast<const ImageMetadataApi*>(image_metadata_pointer)->query_metadata !=
                    nullptr,
            "image metadata interface");

        void* configurable_pointer = reinterpret_cast<void*>(1);
        check(
            api.query_interface != nullptr &&
                api.query_interface(
                    &configurable_preview_api_id,
                    configurable_preview_api_version,
                    &configurable_pointer) == FALSE &&
                configurable_pointer == nullptr,
            "rejects document configuration");

        if (api.shutdown != nullptr)
        {
            api.shutdown();
        }
        FreeLibrary(component);
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

    void append_le16(std::vector<unsigned char>& bytes, std::uint16_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value));
        bytes.push_back(static_cast<unsigned char>(value >> 8));
    }

    void append_le32(std::vector<unsigned char>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value));
        bytes.push_back(static_cast<unsigned char>(value >> 8));
        bytes.push_back(static_cast<unsigned char>(value >> 16));
        bytes.push_back(static_cast<unsigned char>(value >> 24));
    }

    std::vector<unsigned char> make_zip_fixture()
    {
        static constexpr std::string_view name = "folder/hello.txt";
        static constexpr std::string_view content = "hello";
        static constexpr std::uint32_t crc32 = 0x3610A686;
        std::vector<unsigned char> file;
        append_le32(file, 0x04034B50);
        append_le16(file, 20);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le32(file, crc32);
        append_le32(file, static_cast<std::uint32_t>(content.size()));
        append_le32(file, static_cast<std::uint32_t>(content.size()));
        append_le16(file, static_cast<std::uint16_t>(name.size()));
        append_le16(file, 0);
        file.insert(file.end(), name.begin(), name.end());
        file.insert(file.end(), content.begin(), content.end());

        const auto central_offset = static_cast<std::uint32_t>(file.size());
        append_le32(file, 0x02014B50);
        append_le16(file, 20);
        append_le16(file, 20);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le32(file, crc32);
        append_le32(file, static_cast<std::uint32_t>(content.size()));
        append_le32(file, static_cast<std::uint32_t>(content.size()));
        append_le16(file, static_cast<std::uint16_t>(name.size()));
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le32(file, 0);
        append_le32(file, 0);
        file.insert(file.end(), name.begin(), name.end());
        const auto central_size =
            static_cast<std::uint32_t>(file.size()) - central_offset;

        append_le32(file, 0x06054B50);
        append_le16(file, 0);
        append_le16(file, 0);
        append_le16(file, 1);
        append_le16(file, 1);
        append_le32(file, central_size);
        append_le32(file, central_offset);
        append_le16(file, 0);
        return file;
    }

    struct CollectedDirectoryEntry
    {
        std::uint64_t id{};
        std::wstring name;
        bool folder{};
        bool has_children{};
    };

    BOOL WINAPI collect_directory_entry(
        void* context,
        const glance::contracts::components::FileDirectoryEntry* entry) noexcept
    {
        if (context == nullptr || entry == nullptr || entry->name == nullptr)
        {
            return FALSE;
        }
        static_cast<std::vector<CollectedDirectoryEntry>*>(context)->push_back(
            CollectedDirectoryEntry{
                .id = entry->node_id,
                .name = entry->name,
                .folder = entry->is_folder != FALSE,
                .has_children = entry->has_children != FALSE });
        return TRUE;
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

    void test_model3d_component(const std::filesystem::path& component_root)
    {
        using namespace glance::contracts::components;

        const auto directory = component_root / L"model3d";
        const auto component_path = directory / L"Glance.Model3DComponent.dll";
        expect(std::filesystem::is_regular_file(component_path), "Model3D component DLL output");
        for (const auto* payload : {
                 L"component.json",
                 L"resources.pri",
                 L"three-LICENSE.txt",
                 L"occt-import-js-LICENSE.md",
                 L"web/cad.html",
                 L"web/index.html",
                 L"web/viewer.css",
                 L"web/vendor/draco/draco_decoder.wasm",
                 L"web/vendor/draco/draco_wasm_wrapper.js",
                 L"web/vendor/occt/occt-import-js.wasm" })
        {
            expect(
                std::filesystem::is_regular_file(directory / payload),
                "Model3D component runtime output");
        }

        const HMODULE component = LoadLibraryExW(
            component_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        expect(component != nullptr, "load Model3D component DLL");
        if (component == nullptr)
        {
            return;
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(component, get_api_export));
        ComponentApi api;
        expect(
            get_api != nullptr && get_api(abi_version, &api) != FALSE,
            "Model3D component ABI negotiation");
        if (get_api == nullptr || api.initialize == nullptr)
        {
            FreeLibrary(component);
            return;
        }

        std::vector<std::wstring> extensions;
        ComponentRegistrar registrar{
            .context = &extensions,
            .register_extension = collect_extension,
            .register_renderer = accept_renderer };
        ComponentRegistration registration;
        const bool initialized = api.initialize(&registrar, &registration) != FALSE;
        expect(initialized, "Model3D component registration");
        if (!initialized)
        {
            FreeLibrary(component);
            return;
        }
        const std::vector<std::wstring> expected_extensions{
            L".stl", L".3mf", L".obj", L".ply", L".gltf", L".glb", L".fbx",
            L".step", L".stp", L".iges", L".igs", L".brep", L".brp" };
        expect(
            std::wstring_view(registration.component_id) == L"model3d" &&
                std::wstring_view(registration.target_app_version) == GLANCE_VERSION_WSTRING &&
                registration.preferred_kind == PreviewContentKind::web &&
                registration.preferred_format == PreviewContentFormat::html &&
                extensions == expected_extensions,
            "Model3D component registration contract");

        ComponentStatusResult status;
        expect(
            api.query_status != nullptr &&
                api.query_status(L"zh-CN", &status) != FALSE &&
                status.severity == HealthSeverity::healthy &&
                status.display_name[0] != L'\0' && status.detail[0] != L'\0',
            "Model3D component localized status");
        ComponentLoadingTextResult loading;
        expect(
            api.query_loading_text != nullptr &&
                api.query_loading_text(L"C:\\sample.STL", L"zh-CN", &loading) != FALSE &&
                std::wstring_view(loading.text) == L"正在加载 3D 模型..." &&
                api.query_loading_text(L"C:\\sample.STEP", L"zh-CN", &loading) != FALSE &&
                std::wstring_view(loading.text) == L"正在加载 CAD 模型...",
            "Model3D component localized loading text");

        void* interface_pointer{};
        expect(
            api.query_interface != nullptr &&
                api.query_interface(
                    &web_preview_api_id,
                    web_preview_api_version,
                    &interface_pointer) != FALSE &&
                interface_pointer != nullptr,
            "Model3D component web preview interface");
        void* unsupported_interface = reinterpret_cast<void*>(1);
        expect(
            api.query_interface(
                &web_preview_api_id,
                web_preview_api_version + 1,
                &unsupported_interface) == FALSE &&
                unsupported_interface == nullptr,
            "Model3D component rejects unsupported web preview version");

        const auto test_directory = std::filesystem::temp_directory_path() /
            (L"GlanceModel3DTests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code cleanup_error;
        std::filesystem::remove_all(test_directory, cleanup_error);
        std::filesystem::create_directories(test_directory, cleanup_error);
        const auto model_path = test_directory / L"sample model.STL";
        expect(
            !cleanup_error && write_bytes(model_path, {}),
            "Model3D preview fixture");
        if (interface_pointer != nullptr && std::filesystem::is_regular_file(model_path))
        {
            auto preview = std::make_unique<PreparedPreview>();
            expect(
                api.can_preview(model_path.c_str()) != FALSE &&
                    api.prepare_preview(model_path.c_str(), L"en-US", preview.get()) ==
                        PrepareStatus::success &&
                    preview->kind == PreviewContentKind::web &&
                    preview->format == PreviewContentFormat::html &&
                    preview->lease_token != 0 &&
                    std::filesystem::path(preview->path).filename() == L"index.html",
                "Model3D component prepares standard web output");

            WebPreviewOptions options;
            options.color_scheme = PreviewColorScheme::dark;
            auto descriptor = std::make_unique<WebPreviewDescriptor>();
            const auto web_api = static_cast<const WebPreviewApi*>(interface_pointer);
            expect(
                web_api->query_preview(preview->lease_token, &options, descriptor.get()) != FALSE &&
                    descriptor->mapping_count == 2 &&
                    std::wstring_view(descriptor->navigation_uri).starts_with(
                        L"https://glance-model-viewer.invalid/index.html?") &&
                    std::wstring_view(descriptor->navigation_uri).find(L"sample%2520model.STL") !=
                        std::wstring_view::npos &&
                    std::wstring_view(descriptor->navigation_uri).find(L"theme=dark") !=
                        std::wstring_view::npos &&
                    std::wstring_view(descriptor->mappings[0].host_name) ==
                        L"glance-model-viewer.invalid" &&
                    descriptor->mappings[0].access_kind == WebResourceAccessKind::deny_cors &&
                    std::wstring_view(descriptor->mappings[1].host_name) ==
                        L"glance-model-source.invalid" &&
                    descriptor->mappings[1].access_kind == WebResourceAccessKind::allow,
                "Model3D component web preview descriptor");
            api.release_preview(preview->lease_token);
        }
        expect(api.can_preview(L"C:\\missing.STL") == FALSE, "Model3D rejects missing files");
        std::filesystem::remove_all(test_directory, cleanup_error);
        api.shutdown();
        FreeLibrary(component);
    }

    void test_media_info_component(const std::filesystem::path& component_root)
    {
        using namespace glance::contracts::components;

        const auto directory = component_root / L"media-info";
        const auto component_path = directory / L"Glance.MediaInfoComponent.dll";
        expect(std::filesystem::is_regular_file(component_path), "MediaInfo component DLL output");
        expect(
            std::filesystem::is_regular_file(directory / L"component.json") &&
                std::filesystem::is_regular_file(directory / L"resources.pri"),
            "MediaInfo component runtime output");

        const HMODULE component = LoadLibraryExW(
            component_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        expect(component != nullptr, "load MediaInfo component DLL");
        if (component == nullptr)
        {
            return;
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(component, get_api_export));
        ComponentApi api;
        expect(
            get_api != nullptr && get_api(abi_version, &api) != FALSE,
            "MediaInfo component ABI negotiation");
        if (get_api == nullptr || api.initialize == nullptr)
        {
            FreeLibrary(component);
            return;
        }

        std::vector<std::wstring> extensions;
        ComponentRegistrar registrar{
            .context = &extensions,
            .register_extension = collect_extension,
            .register_renderer = accept_renderer };
        ComponentRegistration registration;
        const bool initialized = api.initialize(&registrar, &registration) != FALSE;
        expect(initialized, "MediaInfo component registration");
        if (!initialized)
        {
            FreeLibrary(component);
            return;
        }
        expect(
            std::wstring_view(registration.component_id) == L"media-info" &&
                std::wstring_view(registration.target_app_version) == GLANCE_VERSION_WSTRING &&
                registration.preferred_kind == PreviewContentKind::none &&
                registration.preferred_format == PreviewContentFormat::none &&
                extensions.empty() && api.can_preview == nullptr &&
                api.prepare_preview == nullptr && api.release_preview == nullptr,
            "MediaInfo component auxiliary registration contract");

        ComponentStatusResult status;
        expect(
            api.query_status != nullptr && api.query_status(L"zh-CN", &status) != FALSE &&
                status.display_name[0] != L'\0' && status.detail[0] != L'\0',
            "MediaInfo component localized status");
        const bool ffprobe_available = status.capability_mask == 1;
        expect(
            status.capability_mask <= 1 &&
                status.severity == (ffprobe_available
                    ? HealthSeverity::healthy
                    : HealthSeverity::error),
            "MediaInfo component capability status");

        const auto query_interface = [&api](
                                         const GUID& id,
                                         std::uint32_t version,
                                         std::string_view name)
        {
            void* pointer{};
            expect(
                api.query_interface != nullptr &&
                    api.query_interface(&id, version, &pointer) != FALSE &&
                    pointer != nullptr,
                name);
            return pointer;
        };
        const auto hover = static_cast<const HoverInfoLayerApi*>(query_interface(
            hover_info_layer_api_id,
            hover_info_layer_api_version,
            "MediaInfo component hover interface"));
        const auto shortcuts = static_cast<const StatusBarShortcutApi*>(query_interface(
            status_bar_shortcut_api_id,
            status_bar_shortcut_api_version,
            "MediaInfo component shortcut interface"));
        const auto shortcut_data = static_cast<const StatusBarShortcutDataApi*>(query_interface(
            status_bar_shortcut_data_api_id,
            status_bar_shortcut_data_api_version,
            "MediaInfo component shortcut data interface"));
        const auto actions = static_cast<const ComponentManagementActionApi*>(query_interface(
            component_management_action_api_id,
            component_management_action_api_version,
            "MediaInfo component management interface"));
        expect(
            hover != nullptr && hover->query_info != nullptr &&
                shortcut_data != nullptr && shortcut_data->query_json != nullptr,
            "MediaInfo component information callbacks");

        if (shortcuts != nullptr)
        {
            std::uint32_t count{};
            expect(
                shortcuts->enumerate_shortcuts(L"zh-CN", nullptr, 0, &count) != FALSE &&
                    count == 1,
                "MediaInfo component shortcut count");
            StatusBarShortcutDescriptor shortcut;
            expect(
                shortcuts->enumerate_shortcuts(L"zh-CN", &shortcut, 1, &count) != FALSE &&
                    std::wstring_view(shortcut.shortcut_id) == L"advanced-media-info" &&
                    shortcut.target_kind == PreviewContentKind::media &&
                    shortcut.target_format == PreviewContentFormat::media_file &&
                    shortcut.order == 500 && shortcut.fluent_icon_glyph == 0xe946 &&
                    std::wstring_view(shortcut.tooltip).find(L'\n') != std::wstring_view::npos,
                "MediaInfo component shortcut descriptor");
            expect(
                shortcuts->query_state(
                    L"advanced-media-info",
                    L"C:\\sample.mp4",
                    PreviewContentKind::media,
                    PreviewContentFormat::media_file) ==
                    (ffprobe_available
                        ? StatusBarShortcutState::ready
                        : StatusBarShortcutState::setup_required) &&
                    shortcuts->query_state(
                        L"invalid",
                        L"C:\\sample.mp4",
                        PreviewContentKind::media,
                        PreviewContentFormat::media_file) == StatusBarShortcutState::hidden,
                "MediaInfo component shortcut state");

            StatusBarShortcutActivationResult activation;
            expect(
                shortcuts->activate(
                    L"advanced-media-info",
                    L"C:\\sample.mp4",
                    L"zh-CN",
                    TRUE,
                    &activation) != FALSE &&
                    activation.activation == (ffprobe_available
                        ? StatusBarShortcutActivation::toggle_hover_info
                        : StatusBarShortcutActivation::request_component_action) &&
                    (ffprobe_available
                        ? std::wstring_view(activation.hover_info_id) ==
                              L"advanced-media-info" && activation.checked != FALSE &&
                              std::wstring_view(activation.loading_text).ends_with(L"...")
                        : std::wstring_view(activation.component_action_id) ==
                              L"prepare-ffprobe"),
                "MediaInfo component shortcut activation");
        }

        if (actions != nullptr)
        {
            std::uint32_t count{};
            expect(
                actions->enumerate_actions(L"zh-CN", nullptr, 0, &count) != FALSE &&
                    count == (ffprobe_available ? 0U : 1U),
                "MediaInfo component management action count");
            if (!ffprobe_available)
            {
                ComponentManagementActionDescriptor action;
                expect(
                    actions->enumerate_actions(L"zh-CN", &action, 1, &count) != FALSE &&
                        std::wstring_view(action.action_id) == L"prepare-ffprobe" &&
                        action.button_text[0] != L'\0' &&
                        action.confirmation_message[0] != L'\0',
                    "MediaInfo component management action descriptor");
                ComponentDownloadRequest download;
                expect(
                    actions->prepare_action(L"prepare-ffprobe", L"zh-CN", &download) != FALSE &&
                        std::wstring_view(download.url) ==
                            L"https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-essentials_build.7z" &&
                        std::wstring_view(download.file_name) ==
                            L"ffmpeg-8.1.2-essentials_build.7z" &&
                        std::wstring_view(download.sha256) ==
                            L"e25b682664025d49034c981afb4bae36238a40f29a3cc1c713ad9a8b5b3528f6" &&
                        download.expected_size == 33876939,
                    "MediaInfo component download contract");
            }
        }

        void* unsupported_interface = reinterpret_cast<void*>(1);
        expect(
            api.query_interface(
                &status_bar_shortcut_api_id,
                status_bar_shortcut_api_version + 1,
                &unsupported_interface) == FALSE &&
                unsupported_interface == nullptr,
            "MediaInfo component rejects unsupported shortcut version");
        api.shutdown();
        FreeLibrary(component);
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

int wmain(int argument_count, wchar_t* arguments[])
{
    if (argument_count > 1 &&
        std::wstring_view(arguments[1]) == L"--office-benchmark")
    {
        return glance::tests::run_office_preview_benchmark(
            argument_count,
            arguments);
    }

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

    expect(
        glance::app::can_toggle_preview_fullscreen(true, true, false, false),
        "fullscreen toggle eligibility");
    expect(
        !glance::app::can_toggle_preview_fullscreen(false, true, false, false) &&
            !glance::app::can_toggle_preview_fullscreen(true, false, false, false) &&
            !glance::app::can_toggle_preview_fullscreen(true, true, true, false) &&
            !glance::app::can_toggle_preview_fullscreen(true, true, false, true),
        "fullscreen toggle guards");
    expect(
        glance::app::should_handle_xaml_fullscreen_double_tap(false, false, false),
        "XAML fullscreen double tap eligibility");
    expect(
        !glance::app::should_handle_xaml_fullscreen_double_tap(true, false, false) &&
            !glance::app::should_handle_xaml_fullscreen_double_tap(false, true, false) &&
            !glance::app::should_handle_xaml_fullscreen_double_tap(false, false, true),
        "XAML fullscreen double tap guards");

    expect(
        glance::app::gallery_target_index(1, 1, 4, true) == 2,
        "gallery advances to the next image");
    expect(
        glance::app::gallery_target_index(0, -1, 4, true) == 3,
        "gallery wraps backward from the first image");
    expect(
        glance::app::gallery_target_index(3, 1, 4, true) == 0,
        "gallery wraps forward from the last image");
    expect(
        glance::app::gallery_target_index(1, 10, 4, true) == 3 &&
            glance::app::gallery_target_index(1, -10, 4, true) == 3,
        "gallery folds rapid multi-step navigation");
    expect(
        glance::app::gallery_target_index(7, 1, 0, true) == 7,
        "gallery leaves an empty sequence unchanged");
    expect(
        glance::app::gallery_target_index(0, -1, 4, false) == 0 &&
            glance::app::gallery_target_index(3, 1, 4, false) == 3,
        "gallery stops at boundaries when looping is disabled");
    expect(
        glance::app::gallery_target_index(1, 10, 4, false) == 3 &&
            glance::app::gallery_target_index(1, -10, 4, false) == 0,
        "gallery clamps rapid navigation when looping is disabled");
    expect(
        glance::app::normalize_gallery_extension(L".JpG") == L".jpg" &&
            glance::app::normalize_gallery_extension(L".RAW") == L".raw",
        "gallery extension matching is case insensitive");

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

    glance::contracts::FrameHeader frame_header;
    frame_header.type = glance::contracts::MessageType::open_active_preview;
    frame_header.payload_size = glance::contracts::maximum_payload_size;
    expect(glance::contracts::valid_header(frame_header), "IPC frame maximum payload");
    auto invalid_frame = frame_header;
    invalid_frame.magic ^= 1;
    expect(!glance::contracts::valid_header(invalid_frame), "IPC frame rejects invalid magic");
    invalid_frame = frame_header;
    ++invalid_frame.version;
    expect(!glance::contracts::valid_header(invalid_frame), "IPC frame rejects invalid version");
    invalid_frame = frame_header;
    ++invalid_frame.payload_size;
    expect(!glance::contracts::valid_header(invalid_frame), "IPC frame rejects oversized payload");

    const glance::app::UpdatePreferences default_update_preferences;
    expect(
        default_update_preferences.automatic_check_enabled &&
            default_update_preferences.frequency == glance::app::UpdateCheckFrequency::daily,
        "automatic updates default to daily checks");
    expect(
        glance::app::update_check_interval_seconds(
            glance::app::UpdateCheckFrequency::hourly) == 60ULL * 60ULL &&
            glance::app::update_check_interval_seconds(
                glance::app::UpdateCheckFrequency::daily) == 24ULL * 60ULL * 60ULL &&
            glance::app::update_check_interval_seconds(
                glance::app::UpdateCheckFrequency::weekly) == 7ULL * 24ULL * 60ULL * 60ULL &&
            glance::app::update_check_interval_seconds(
                glance::app::UpdateCheckFrequency::monthly) == 30ULL * 24ULL * 60ULL * 60ULL,
        "automatic update intervals");

    auto update_preferences = default_update_preferences;
    expect(
        glance::app::automatic_update_check_due(update_preferences, 1000),
        "automatic update is due without a successful check");
    update_preferences.last_successful_check = 1000;
    expect(
        !glance::app::automatic_update_check_due(
            update_preferences,
            1000 + 24ULL * 60ULL * 60ULL - 1) &&
            glance::app::automatic_update_check_due(
                update_preferences,
                1000 + 24ULL * 60ULL * 60ULL),
        "automatic update daily boundary");
    update_preferences.retry_after = 1000 + 25ULL * 60ULL * 60ULL;
    expect(
        !glance::app::automatic_update_check_due(
            update_preferences,
            update_preferences.retry_after - 1) &&
            glance::app::automatic_update_check_due(
                update_preferences,
                update_preferences.retry_after),
        "automatic update retry boundary");
    update_preferences.automatic_check_enabled = false;
    expect(
        !glance::app::automatic_update_check_due(update_preferences, UINT64_MAX),
        "disabled automatic update never checks");
    update_preferences.skipped_version = L"v2026.8.15";
    expect(
        glance::app::update_version_is_skipped(update_preferences, L"V2026.8.15") &&
            !glance::app::update_version_is_skipped(update_preferences, L"v2026.8.16"),
        "automatic update skips only the selected version");

    using glance::contracts::UpdateCheckStatus;
    expect(
        glance::contracts::cached_update_result_is_newer(101, 100) &&
            !glance::contracts::cached_update_result_is_newer(100, 100) &&
            !glance::contracts::cached_update_result_is_newer(0, 0),
        "Core update cache freshness");
    expect(
        glance::contracts::should_publish_automatic_update_result(
            UpdateCheckStatus::update_available,
            true) &&
            !glance::contracts::should_publish_automatic_update_result(
                UpdateCheckStatus::up_to_date,
                false) &&
            glance::contracts::should_publish_automatic_update_result(
                UpdateCheckStatus::unavailable,
                false),
        "Core automatic update publication policy");

    const glance::contracts::UpdateCheckRequest update_request{
        42, 1234, true, L"2026.8.15" };
    const auto encoded_update_request =
        glance::contracts::encode_update_check_request(update_request);
    const auto decoded_update_request =
        glance::contracts::decode_update_check_request(encoded_update_request);
    expect(
        decoded_update_request && decoded_update_request->request_id == 42 &&
            decoded_update_request->last_successful_check == 1234 &&
            decoded_update_request->automatic &&
            decoded_update_request->current_version == L"2026.8.15",
        "update request IPC round trip");
    expect(
        !glance::contracts::decode_update_check_request(
            std::string_view(encoded_update_request).substr(
                0,
                encoded_update_request.size() - 1)) &&
            !glance::contracts::decode_update_check_request(
                encoded_update_request + std::string(1, '\0')),
        "update request IPC rejects malformed payloads");

    glance::contracts::UpdateCheckResult available_update;
    available_update.status = UpdateCheckStatus::update_available;
    available_update.latest_version = L"v2026.8.16";
    available_update.release_url = L"https://github.com/ElluIFX/Glance/releases/tag/v2026.8.16";
    available_update.installer = {
        L"2026.8.16",
        L"Glance-Setup-2026.8.16-x64.exe",
        L"https://github.com/ElluIFX/Glance/releases/download/v2026.8.16/Glance-Setup-2026.8.16-x64.exe",
        std::wstring(64, L'a'),
        123456 };
    available_update.checked_at = 5678;
    const auto decoded_update_response = glance::contracts::decode_update_check_response(
        glance::contracts::encode_update_check_response({ 43, available_update }));
    expect(
        decoded_update_response && decoded_update_response->request_id == 43 &&
            decoded_update_response->result.status == UpdateCheckStatus::update_available &&
            decoded_update_response->result.installer.file_name ==
                L"Glance-Setup-2026.8.16-x64.exe" &&
            decoded_update_response->result.checked_at == 5678,
        "update response IPC round trip");
    const auto deferred_update = glance::contracts::encode_update_check_deferred(43);
    expect(
        glance::contracts::decode_update_check_deferred(deferred_update) == 43 &&
            !glance::contracts::decode_update_check_deferred(
                deferred_update + std::string(1, '\0')),
        "deferred update IPC round trip");

    const glance::contracts::NetworkDownloadMessage download_request{
        44,
        { L"https://example.com/file.zip", L"file.zip", std::wstring(64, L'b'), 2048 } };
    const auto decoded_download_request = glance::contracts::decode_network_download_request(
        glance::contracts::encode_network_download_request(download_request));
    expect(
        decoded_download_request && decoded_download_request->request_id == 44 &&
            decoded_download_request->request.file_name == L"file.zip" &&
            decoded_download_request->request.expected_size == 2048,
        "download request IPC round trip");
    const auto decoded_download_progress = glance::contracts::decode_network_download_progress(
        glance::contracts::encode_network_download_progress({ 44, 1024, 2048 }));
    expect(
        decoded_download_progress && decoded_download_progress->request_id == 44 &&
            decoded_download_progress->downloaded == 1024 &&
            decoded_download_progress->total == 2048,
        "download progress IPC round trip");
    const auto decoded_download_response = glance::contracts::decode_network_download_response(
        glance::contracts::encode_network_download_response(
            { 44,
              { glance::contracts::NetworkDownloadStatus::succeeded,
                L"C:\\Temp\\Glance\\file.zip" } }));
    expect(
        decoded_download_response && decoded_download_response->request_id == 44 &&
            decoded_download_response->result.status ==
                glance::contracts::NetworkDownloadStatus::succeeded &&
            decoded_download_response->result.path == L"C:\\Temp\\Glance\\file.zip",
        "download response IPC round trip");
    expect(
        glance::contracts::decode_network_download_cancel(
            glance::contracts::encode_network_download_cancel(44)) == 44,
        "download cancellation IPC round trip");

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
        glance::app::normalize_pdf_preview_render_dimension(1024) == 1024 &&
            glance::app::normalize_pdf_preview_render_dimension(2048) == 2048 &&
            glance::app::normalize_pdf_preview_render_dimension(4096) == 4096 &&
            glance::app::normalize_pdf_preview_render_dimension(8192) == 8192,
        "PDF preview render dimensions");
    expect(
        glance::app::normalize_pdf_preview_render_dimension(0) == 4096 &&
            glance::app::normalize_pdf_preview_render_dimension(16384) == 4096,
        "PDF preview render dimension fallback");
    const glance::app::MediaPreviewPreferences default_media_preferences;
    expect(
        default_media_preferences.middle_click_gallery_mode &&
            default_media_preferences.loop_gallery_scrolling &&
            !default_media_preferences.gallery_same_extension_only,
        "gallery media preference defaults");
    const glance::app::FooterPreferences default_footer_preferences;
    expect(
        default_footer_preferences.order[2] == glance::app::FooterField::taken_time &&
            default_footer_preferences.order.back() == glance::app::FooterField::media_info &&
            !glance::app::footer_field_enabled(
                default_footer_preferences,
                glance::app::FooterField::taken_time),
        "capture time footer preference defaults");

    std::wstring executable_path(32768, L'\0');
    const DWORD executable_length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    executable_path.resize(executable_length);
    const auto component_root =
        std::filesystem::path(executable_path).parent_path() / L"components";
    test_image_codec_component(
        component_root,
        {
            .id = L"heic",
            .component_file = L"Glance.HeicComponent.dll",
            .host_file = L"Glance.HeicHost.exe",
            .extensions = { L".heic", L".heif", L".hif" },
            .loading_extension = L".heic",
            .loading_text = L"正在加载 HEIC 图片...",
            .payload_files = {
                L"resources.pri",
                L"heif.dll",
                L"libde265.dll",
                L"libheif-LICENSE.txt",
                L"libde265-LICENSE.txt" } });
    test_image_codec_component(
        component_root,
        {
            .id = L"avif",
            .component_file = L"Glance.AvifComponent.dll",
            .host_file = L"Glance.AvifHost.exe",
            .extensions = { L".avif" },
            .loading_extension = L".avif",
            .loading_text = L"正在加载 AVIF 图片...",
            .payload_files = {
                L"resources.pri",
                L"libavif-LICENSE.txt",
                L"dav1d-LICENSE.txt" } });
    test_image_codec_component(
        component_root,
        {
            .id = L"raw",
            .component_file = L"Glance.RawComponent.dll",
            .host_file = L"Glance.RawHost.exe",
            .extensions = {
                L".cr2", L".cr3", L".nef", L".nrw", L".arw", L".srf",
                L".sr2", L".orf", L".rw2", L".raf", L".dng", L".pef",
                L".srw", L".x3f", L".erf", L".3fr", L".fff", L".mef",
                L".mos", L".raw" },
            .loading_extension = L".dng",
            .loading_text = L"正在加载相机 RAW 文件...",
            .payload_files = {
                L"resources.pri",
                L"libraw.dll",
                L"libraw-LICENSE.txt" } });
    test_model3d_component(component_root);
    test_media_info_component(component_root);
    const auto source_directory =
        std::filesystem::path(executable_path).parent_path() / L"sources" / L"everything";
    const auto source_path = source_directory / L"Glance.EverythingSource.dll";
    const auto source_descriptor_path = source_directory / L"source.json";
    const auto source_resource_path = source_directory / L"resources.pri";
    expect(std::filesystem::is_regular_file(source_path), "Everything source DLL output");
    expect(std::filesystem::is_regular_file(source_descriptor_path), "Everything source descriptor output");
    expect(std::filesystem::is_regular_file(source_resource_path), "Everything source resource output");
    const HMODULE source = LoadLibraryExW(
        source_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32);
    expect(source != nullptr, "load Everything source DLL");
    if (source != nullptr)
    {
        using namespace glance::contracts::sources;
        const auto get_source_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(source, get_api_export));
        SourceApi api;
        expect(get_source_api != nullptr, "Everything source API export");
        expect(
            get_source_api != nullptr && get_source_api(abi_version, &api) != FALSE,
            "Everything source ABI negotiation");
        SourceRegistration registration;
        expect(
            api.initialize != nullptr && api.initialize(&registration) != FALSE,
            "Everything source registration");
        expect(
            std::wstring_view(registration.source_id) == L"everything" &&
                std::wstring_view(registration.target_app_version) == GLANCE_VERSION_WSTRING,
            "Everything source identity");
        expect(
            (registration.capability_mask & static_cast<std::uint64_t>(Capability::selection)) != 0,
            "Everything source selection capability");
        SourceStatusResult status;
        expect(
            api.query_status != nullptr && api.query_status(L"en-US", &status) != FALSE &&
                status.severity == HealthSeverity::healthy,
            "Everything source ready status");
        if (api.shutdown != nullptr)
        {
            api.shutdown();
        }
        FreeLibrary(source);
    }
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
    expect(
        descriptor.find("\"schema_version\": 3") != std::string::npos &&
            descriptor.find("\"dependencies\": []") != std::string::npos &&
            descriptor.find("\"pdf\"") == std::string::npos,
        "Office descriptor has no PDF dependency");

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
                    .register_extension = collect_extension,
                    .register_renderer = accept_renderer };
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
                        registration.preferred_format ==
                            PreviewContentFormat::native_surface,
                    "Office component preferred output");
                expect(extensions.size() == 6, "Office component extension count");
                expect(
                    api.can_preview(L"C:\\GlanceComponentTest\\sample.rtf") == FALSE,
                    "Office component rejects unregistered formats");
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
                        ? L"Office preview handlers are unavailable"
                        : L"Some Office preview handlers are unavailable";
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
                        ? L"Office 预览处理器不可用"
                        : L"部分 Office 预览处理器不可用";
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
                            L"Loading Office document...",
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
                        L"正在加载 Office 文档...",
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
                        L"正在加载 Office 文档...",
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
                        L"Loading Office document...",
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
                void* renderer_pointer{};
                expect(
                    api.query_interface(
                        &native_preview_renderer_api_id,
                        native_preview_renderer_api_version,
                        &renderer_pointer) != FALSE && renderer_pointer != nullptr,
                    "Office component native renderer interface");
                if (renderer_pointer != nullptr)
                {
                    NativePreviewHostDescriptor host_descriptor;
                    const auto renderer = static_cast<const NativePreviewRendererApi*>(
                        renderer_pointer);
                    expect(
                        renderer->query_host(&host_descriptor) != FALSE &&
                            std::wstring_view(host_descriptor.host_executable) ==
                                L"Glance.OfficeHost.exe",
                        "Office component native host descriptor");
                }
                void* notice_pointer{};
                expect(
                    api.query_interface(
                        &preview_notice_api_id,
                        preview_notice_api_version,
                        &notice_pointer) != FALSE && notice_pointer != nullptr,
                    "Office component preview notice interface");
                if (notice_pointer != nullptr)
                {
                    const auto test_directory =
                        std::filesystem::temp_directory_path() /
                        (L"GlanceOfficeTests-" +
                         std::to_wstring(GetCurrentProcessId()));
                    std::error_code cleanup_error;
                    std::filesystem::remove_all(test_directory, cleanup_error);
                    std::filesystem::create_directories(test_directory, cleanup_error);
                    const auto protected_path = test_directory / L"protected.pptx";
                    const auto zone_path = std::filesystem::path(
                        protected_path.wstring() + L":Zone.Identifier");
                    const std::vector<unsigned char> zone_data{
                        '[', 'Z', 'o', 'n', 'e', 'T', 'r', 'a', 'n', 's', 'f', 'e', 'r', ']',
                        '\r', '\n', 'Z', 'o', 'n', 'e', 'I', 'd', '=', '3', '\r', '\n' };
                    expect(
                        !cleanup_error &&
                            write_bytes(protected_path, { 'P', 'P', 'T', 'X' }) &&
                            write_bytes(zone_path, zone_data),
                        "Office protected preview fixture");
                    if (api.can_preview(protected_path.c_str()) != FALSE)
                    {
                        PreparedPreview protected_preview;
                        expect(
                            api.prepare_preview(
                                protected_path.c_str(),
                                L"en-US",
                                &protected_preview) == PrepareStatus::success &&
                                protected_preview.lease_token != 0 &&
                                std::filesystem::path(protected_preview.path) !=
                                    protected_path &&
                                std::filesystem::is_regular_file(
                                    protected_preview.path) &&
                                GetPrivateProfileIntW(
                                    L"ZoneTransfer",
                                    L"ZoneId",
                                    0,
                                    (std::wstring(protected_preview.path) +
                                     L":Zone.Identifier").c_str()) == 0,
                            "Office protected preview uses an unblocked copy");
                        PreviewNoticeResult notice;
                        const auto notice_api = static_cast<const PreviewNoticeApi*>(
                            notice_pointer);
                        expect(
                            notice_api->query_preview_notice(
                                protected_preview.lease_token,
                                L"zh-CN",
                                &notice) != FALSE &&
                                notice.severity == PreviewNoticeSeverity::warning &&
                                notice.duration_ms == 1000 &&
                                std::wstring_view(notice.text) ==
                                    L"正在预览被阻止的网络文件",
                            "Office protected preview warning notice");
                        const std::filesystem::path copied_path(
                            protected_preview.path);
                        api.release_preview(protected_preview.lease_token);
                        expect(
                            !std::filesystem::exists(copied_path),
                            "Office protected preview lease cleanup");
                    }
                    std::filesystem::remove_all(test_directory, cleanup_error);
                }
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
                .register_extension = collect_extension,
                .register_renderer = accept_renderer };
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

            void* gallery_media_pointer{};
            expect(
                api.query_interface(
                    &gallery_media_api_id,
                    gallery_media_api_version,
                    &gallery_media_pointer) != FALSE &&
                    gallery_media_pointer != nullptr,
                "Adobe gallery media interface");
            if (gallery_media_pointer != nullptr)
            {
                const auto gallery_media = static_cast<GalleryMediaApi*>(gallery_media_pointer);
                expect(
                    gallery_media->classify_extension(L".psd") == GalleryMediaKind::image &&
                        gallery_media->classify_extension(L".PSB") == GalleryMediaKind::image &&
                        gallery_media->classify_extension(L".ai") == GalleryMediaKind::none,
                    "Adobe gallery media classification");
            }

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
                        L"正在加载 Adobe 文档...",
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
                        L"正在加载高清预览...",
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
            PreviewNoticeResult psb_notice;
            expect(
                preview_notice != nullptr &&
                    preview_notice->query_preview_notice(
                        psb_preview.lease_token,
                        L"zh-CN",
                        &psb_notice) != FALSE &&
                    psb_notice.severity == PreviewNoticeSeverity::informational &&
                    psb_notice.duration_ms == 0 &&
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
            const auto pdf_component_directory =
                std::filesystem::path(executable_path).parent_path() /
                L"components" / L"pdf";
            const auto pdf_component_path =
                pdf_component_directory / L"Glance.PdfComponent.dll";
            const auto pdf_host_path =
                pdf_component_directory / L"Glance.PdfHost.exe";
            expect(
                std::filesystem::is_regular_file(pdf_component_path),
                "PDF component DLL output");
            expect(
                std::filesystem::is_regular_file(pdf_host_path),
                "PDF component host output");
            expect(
                std::filesystem::is_regular_file(pdf_component_directory / L"pdfium.dll"),
                "PDF component PDFium output");
            expect(
                std::filesystem::is_regular_file(pdf_component_directory / L"resources.pri"),
                "PDF component resource output");

            const HMODULE pdf_component = LoadLibraryExW(
                pdf_component_path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                    LOAD_LIBRARY_SEARCH_SYSTEM32);
            expect(pdf_component != nullptr, "load PDF component DLL");
            if (pdf_component != nullptr)
            {
                using namespace glance::contracts::components;
                const auto get_api = reinterpret_cast<GetApiFunction>(
                    GetProcAddress(pdf_component, get_api_export));
                ComponentApi api;
                expect(
                    get_api != nullptr && get_api(abi_version, &api) != FALSE,
                    "PDF component ABI negotiation");
                if (get_api != nullptr && api.initialize != nullptr)
                {
                    std::vector<std::wstring> extensions;
                    ComponentRegistrar registrar{
                        .context = &extensions,
                        .register_extension = collect_extension,
                        .register_renderer = accept_renderer };
                    ComponentRegistration registration;
                    expect(
                        api.initialize(&registrar, &registration) != FALSE,
                        "PDF component registration");
                    expect(
                        std::wstring_view(registration.component_id) == L"pdf" &&
                            extensions == std::vector<std::wstring>{ L".pdf" },
                        "PDF component extension registration");

                    void* interface_pointer{};
                    expect(
                        api.query_interface(
                            &paged_document_renderer_api_id,
                            paged_document_renderer_api_version,
                            &interface_pointer) != FALSE &&
                            interface_pointer != nullptr,
                        "PDF component paged document interface");
                    if (interface_pointer != nullptr)
                    {
                        PagedDocumentHostDescriptor host;
                        const auto renderer =
                            static_cast<const PagedDocumentRendererApi*>(interface_pointer);
                        expect(
                            renderer->query_host(&host) != FALSE &&
                                std::wstring_view(host.host_executable) ==
                                    L"Glance.PdfHost.exe",
                            "PDF component host descriptor");
                    }

                    interface_pointer = nullptr;
                    expect(
                        api.query_interface(
                            &settings_contribution_api_id,
                            settings_contribution_api_version,
                            &interface_pointer) != FALSE &&
                            interface_pointer != nullptr,
                        "PDF component settings interface");
                    if (interface_pointer != nullptr)
                    {
                        const auto settings =
                            static_cast<const SettingsContributionApi*>(interface_pointer);
                        std::uint32_t setting_count{};
                        expect(
                            settings->enumerate_settings(
                                L"zh-CN", nullptr, 0, &setting_count) != FALSE &&
                                setting_count == 1,
                            "PDF component setting count");
                        ComponentSettingDescriptor setting;
                        expect(
                            settings->enumerate_settings(
                                L"zh-CN", &setting, 1, &setting_count) != FALSE &&
                                std::wstring_view(setting.setting_id) ==
                                    L"render-dimension" &&
                                std::wstring_view(setting.group_id) == L"pdf-preview" &&
                                std::wstring_view(setting.group_title) == L"PDF 预览" &&
                                std::wstring_view(setting.label) == L"PDF 预览分辨率" &&
                                setting.option_count == 4 &&
                                setting.default_value == 4096,
                            "PDF component render setting");
                    }

                    PreparedPreview preview;
                    expect(
                        api.can_preview(pdf_path.c_str()) != FALSE &&
                            api.prepare_preview(
                                pdf_path.c_str(), L"en-US", &preview) ==
                                PrepareStatus::success &&
                            preview.kind == PreviewContentKind::document &&
                            preview.format == PreviewContentFormat::pdf &&
                            std::filesystem::path(preview.path) == pdf_path,
                        "PDF component prepares standard document output");
                    api.shutdown();
                }
                FreeLibrary(pdf_component);
            }

            glance::app::PagedDocumentRenderClient client(
                pdf_host_path.wstring(),
                nullptr);
            const auto opened = client.open(pdf_path.wstring(), L"");
            expect(
                opened.status == glance::contracts::document::Status::success &&
                    opened.page_count == 1,
                "PDF Host opens document");
            if (opened.status == glance::contracts::document::Status::success)
            {
                const auto rendered = client.render(0, 256, 256);
                expect(
                    rendered.status == glance::contracts::document::Status::success &&
                        rendered.pixel_width > 0 &&
                        rendered.pixel_height > 0 &&
                        !rendered.pixels.empty(),
                    "PDF Host renders page");
            }
        }
        std::filesystem::remove_all(test_directory, cleanup_error);
    }

    {
        using namespace glance::contracts::components;
        const auto archive_directory =
            std::filesystem::path(executable_path).parent_path() /
            L"components" / L"archive";
        const auto archive_component_path =
            archive_directory / L"Glance.ArchiveComponent.dll";
        expect(
            std::filesystem::is_regular_file(archive_component_path),
            "Archive component DLL output");
        expect(
            std::filesystem::is_regular_file(
                archive_directory / L"Glance.ArchiveHost.exe") &&
                std::filesystem::is_regular_file(archive_directory / L"7z.dll") &&
                std::filesystem::is_regular_file(
                    archive_directory / L"7-Zip-LICENSE.txt") &&
                std::filesystem::is_regular_file(archive_directory / L"resources.pri"),
            "Archive component runtime output");

        const HMODULE archive_component = LoadLibraryExW(
            archive_component_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        expect(archive_component != nullptr, "load Archive component DLL");
        if (archive_component != nullptr)
        {
            const auto get_api = reinterpret_cast<GetApiFunction>(
                GetProcAddress(archive_component, get_api_export));
            ComponentApi api;
            expect(
                get_api != nullptr && get_api(abi_version, &api) != FALSE,
                "Archive component ABI negotiation");
            if (get_api != nullptr && api.initialize != nullptr)
            {
                std::vector<std::wstring> extensions;
                ComponentRegistrar registrar{
                    .context = &extensions,
                    .register_extension = collect_extension,
                    .register_renderer = accept_renderer };
                ComponentRegistration registration;
                expect(
                    api.initialize(&registrar, &registration) != FALSE &&
                        std::wstring_view(registration.component_id) == L"archive" &&
                        registration.preferred_kind == PreviewContentKind::directory &&
                        registration.preferred_format ==
                            PreviewContentFormat::file_directory &&
                        std::ranges::find(extensions, L".iso") != extensions.end() &&
                        std::ranges::find(extensions, L".zip") != extensions.end(),
                    "Archive component registration");

                void* interface_pointer{};
                expect(
                    api.query_interface(
                        &file_directory_preview_api_id,
                        file_directory_preview_api_version,
                        &interface_pointer) != FALSE &&
                        interface_pointer != nullptr,
                    "Archive component file directory interface");

                const auto test_directory =
                    std::filesystem::temp_directory_path() /
                    (L"GlanceArchiveTests-" +
                     std::to_wstring(GetCurrentProcessId()));
                std::error_code cleanup_error;
                std::filesystem::remove_all(test_directory, cleanup_error);
                std::filesystem::create_directories(test_directory, cleanup_error);
                const auto archive_path = test_directory / L"sample.zip";
                expect(
                    !cleanup_error && write_bytes(archive_path, make_zip_fixture()),
                    "Archive preview fixture");
                if (interface_pointer != nullptr &&
                    std::filesystem::is_regular_file(archive_path))
                {
                    PreparedPreview preview;
                    expect(
                        api.can_preview(archive_path.c_str()) != FALSE &&
                            api.prepare_preview(
                                archive_path.c_str(),
                                L"en-US",
                                &preview) == PrepareStatus::success &&
                            preview.kind == PreviewContentKind::directory &&
                            preview.format == PreviewContentFormat::file_directory &&
                            preview.lease_token != 0,
                        "Archive component prepares directory output");
                    const auto directory_api =
                        static_cast<const FileDirectoryPreviewApi*>(interface_pointer);
                    FileDirectoryDescriptor archive_descriptor;
                    const auto opened = directory_api->open(
                        preview.lease_token,
                        L"en-US",
                        L"",
                        &archive_descriptor);
                    expect(
                        opened == FileDirectoryOpenStatus::ready &&
                            archive_descriptor.column_count >= 3 &&
                            archive_descriptor.info_field_count >= 3,
                        "Archive Host reads ZIP metadata");
                    if (opened == FileDirectoryOpenStatus::ready)
                    {
                        std::vector<CollectedDirectoryEntry> root_entries;
                        FileDirectoryEntrySink sink{
                            .context = &root_entries,
                            .append = collect_directory_entry };
                        std::uint32_t returned{};
                        std::uint32_t total{};
                        expect(
                            directory_api->enumerate_children(
                                preview.lease_token,
                                0,
                                0,
                                16,
                                &sink,
                                &returned,
                                &total) != FALSE &&
                                returned == 1 && total == 1 &&
                                root_entries.size() == 1 &&
                                root_entries[0].folder &&
                                root_entries[0].has_children &&
                                root_entries[0].name == L"folder",
                            "Archive component enumerates root directory");
                        if (!root_entries.empty())
                        {
                            std::vector<CollectedDirectoryEntry> child_entries;
                            sink.context = &child_entries;
                            expect(
                                directory_api->enumerate_children(
                                    preview.lease_token,
                                    root_entries[0].id,
                                    0,
                                    16,
                                    &sink,
                                    &returned,
                                    &total) != FALSE &&
                                    returned == 1 && total == 1 &&
                                    child_entries.size() == 1 &&
                                    !child_entries[0].folder &&
                                    child_entries[0].name == L"hello.txt",
                                "Archive component enumerates child directory");
                        }
                    }
                    api.release_preview(preview.lease_token);
                }
                std::filesystem::remove_all(test_directory, cleanup_error);
                api.shutdown();
            }
            FreeLibrary(archive_component);
        }
    }

    if (failures == 0)
    {
        std::cout << "All regression tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
