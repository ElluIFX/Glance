#include "office_preview_benchmark.h"

#include "glance/contracts/component_api.h"
#include "native_preview_surface.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace std::chrono;
    using namespace glance::contracts::components;

    struct BenchmarkOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::uint32_t iterations{ 1 };
    };

    struct BenchmarkResult
    {
        PrepareStatus prepare_status{ PrepareStatus::failed };
        glance::contracts::native_preview::Status open_status{
            glance::contracts::native_preview::Status::open_failed };
        std::int64_t component_prepare_ms{ -1 };
        std::int64_t host_create_ms{ -1 };
        std::int64_t do_preview_return_ms{ -1 };
        std::int64_t first_surface_ready_ms{ -1 };
        std::int64_t unload_ms{ -1 };
        bool native_output_valid{};
        bool host_started{};
    };

    bool parse_number(
        const wchar_t* text,
        std::uint32_t minimum,
        std::uint32_t maximum,
        std::uint32_t& value)
    {
        wchar_t* end{};
        const auto parsed = text == nullptr ? 0 : wcstoul(text, &end, 10);
        if (text == nullptr || end == text || *end != L'\0' ||
            parsed < minimum || parsed > maximum)
        {
            return false;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    }

    void print_usage()
    {
        std::wcout
            << L"Usage:\n"
            << L"  Glance.Tests.exe --office-benchmark <file-or-directory> "
               L"[--iterations <1-20>] [--output <csv>]\n";
    }

    std::optional<BenchmarkOptions> parse_options(
        int argument_count,
        wchar_t* arguments[])
    {
        if (argument_count < 3 || std::wstring_view(arguments[2]) == L"--help")
        {
            print_usage();
            return std::nullopt;
        }
        BenchmarkOptions options{ .input = arguments[2] };
        for (int index = 3; index < argument_count; ++index)
        {
            const std::wstring_view argument{ arguments[index] };
            if (argument == L"--iterations" && index + 1 < argument_count &&
                parse_number(arguments[++index], 1, 20, options.iterations))
            {
                continue;
            }
            if (argument == L"--output" && index + 1 < argument_count)
            {
                options.output = arguments[++index];
                continue;
            }
            std::wcerr << L"Invalid office benchmark argument: " << argument << L'\n';
            return std::nullopt;
        }
        return options;
    }

    bool supported_office_path(const std::filesystem::path& path)
    {
        static constexpr std::wstring_view extensions[]{
            L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx" };
        const auto extension = path.extension().wstring();
        return std::ranges::any_of(extensions, [&](std::wstring_view candidate) {
            return _wcsicmp(extension.c_str(), candidate.data()) == 0;
        });
    }

    std::vector<std::filesystem::path> collect_inputs(
        const std::filesystem::path& input)
    {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        const auto absolute = std::filesystem::absolute(input, error);
        if (!error && std::filesystem::is_regular_file(absolute, error))
        {
            if (supported_office_path(absolute))
            {
                files.push_back(absolute);
            }
            return files;
        }
        if (error || !std::filesystem::is_directory(absolute, error))
        {
            return files;
        }
        for (std::filesystem::directory_iterator iterator(
                 absolute,
                 std::filesystem::directory_options::skip_permission_denied,
                 error),
             end;
             iterator != end;
             iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }
            if (iterator->is_regular_file(error) &&
                supported_office_path(iterator->path()))
            {
                files.push_back(iterator->path());
            }
        }
        std::ranges::sort(files);
        return files;
    }

    BOOL WINAPI collect_extension(void*, const wchar_t*) noexcept
    {
        return TRUE;
    }

    BOOL WINAPI accept_renderer(
        void*,
        PreviewContentKind kind,
        PreviewContentFormat format,
        const GUID* interface_id,
        std::uint32_t interface_version) noexcept
    {
        return kind == PreviewContentKind::document &&
            format == PreviewContentFormat::native_surface &&
            interface_id != nullptr &&
            IsEqualGUID(*interface_id, native_preview_renderer_api_id) &&
            interface_version == native_preview_renderer_api_version;
    }

    template <typename Function>
    auto run_with_message_pump(Function&& function)
    {
        auto future = std::async(std::launch::async, std::forward<Function>(function));
        while (future.wait_for(milliseconds(5)) != std::future_status::ready)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return future.get();
    }

    BenchmarkResult measure(
        const std::filesystem::path& source,
        const ComponentApi& api,
        const std::filesystem::path& host_path,
        HWND parent)
    {
        BenchmarkResult result;
        PreparedPreview preview;
        const auto total_start = steady_clock::now();
        const auto prepare_start = steady_clock::now();
        result.prepare_status = api.prepare_preview(source.c_str(), L"en-US", &preview);
        result.component_prepare_ms = duration_cast<milliseconds>(
            steady_clock::now() - prepare_start).count();
        result.native_output_valid = result.prepare_status == PrepareStatus::success &&
            preview.kind == PreviewContentKind::document &&
            preview.format == PreviewContentFormat::native_surface &&
            std::filesystem::path(preview.path) == source;
        if (!result.native_output_valid)
        {
            return result;
        }

        auto surface = std::make_shared<glance::app::NativePreviewSurface>(
            parent,
            host_path.wstring(),
            nullptr,
            [] {});
        surface->set_bounds(0, 0, 960, 720);
        const auto host_start = steady_clock::now();
        result.host_started = run_with_message_pump([surface] {
            return surface->start_host();
        });
        result.host_create_ms = duration_cast<milliseconds>(
            steady_clock::now() - host_start).count();
        if (result.host_started)
        {
            const auto preview_start = steady_clock::now();
            result.open_status = run_with_message_pump([surface, path = source.wstring()] {
                return surface->open(
                    path,
                    {
                        .background_color = RGB(32, 32, 32),
                        .text_color = RGB(255, 255, 255),
                        .color_scheme = 1 },
                    96);
            });
            result.do_preview_return_ms = duration_cast<milliseconds>(
                steady_clock::now() - preview_start).count();
            if (result.open_status ==
                glance::contracts::native_preview::Status::success)
            {
                result.first_surface_ready_ms = duration_cast<milliseconds>(
                    steady_clock::now() - total_start).count();
            }
        }

        const auto unload_start = steady_clock::now();
        run_with_message_pump([surface] {
            surface->shutdown();
            return true;
        });
        result.unload_ms = duration_cast<milliseconds>(
            steady_clock::now() - unload_start).count();
        surface->destroy_surface();
        if (preview.lease_token != 0)
        {
            api.release_preview(preview.lease_token);
        }
        return result;
    }

    std::string to_utf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        std::string result(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
        if (size > 0)
        {
            WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                text.data(),
                static_cast<int>(text.size()),
                result.data(),
                size,
                nullptr,
                nullptr);
        }
        return result;
    }

    std::string csv_text(std::string_view value)
    {
        std::string result{ "\"" };
        for (const char character : value)
        {
            result.push_back(character);
            if (character == '"')
            {
                result.push_back('"');
            }
        }
        return result + '"';
    }

    void write_header(std::ofstream& output)
    {
        output.write("\xEF\xBB\xBF", 3);
        output
            << "schema_version,source,extension,source_bytes,iteration,"
               "component_prepare_ms,host_create_ms,do_preview_return_ms,"
               "first_surface_ready_ms,full_content_ready,unload_ms,prepare_status,"
               "open_status,native_output_valid,host_started\n";
    }

    void write_result(
        std::ofstream& output,
        const std::filesystem::path& source,
        std::uintmax_t source_bytes,
        std::uint32_t iteration,
        const BenchmarkResult& result)
    {
        output << "2," << csv_text(to_utf8(source.wstring())) << ','
               << csv_text(to_utf8(source.extension().wstring())) << ','
               << source_bytes << ',' << iteration << ','
               << result.component_prepare_ms << ',' << result.host_create_ms << ','
               << result.do_preview_return_ms << ',' << result.first_surface_ready_ms << ','
               << "not_applicable," << result.unload_ms << ','
               << static_cast<std::uint32_t>(result.prepare_status) << ','
               << static_cast<std::uint32_t>(result.open_status) << ','
               << result.native_output_valid << ',' << result.host_started << '\n';
    }

    bool successful(const BenchmarkResult& result)
    {
        return result.prepare_status == PrepareStatus::success &&
            result.open_status == glance::contracts::native_preview::Status::success &&
            result.native_output_valid && result.host_started;
    }
}

namespace glance::tests
{
    int run_office_preview_benchmark(int argument_count, wchar_t* arguments[])
    {
        const auto parsed = parse_options(argument_count, arguments);
        if (!parsed)
        {
            return argument_count == 3 && std::wstring_view(arguments[2]) == L"--help"
                ? 0
                : 2;
        }
        auto options = *parsed;
        const auto inputs = collect_inputs(options.input);
        if (inputs.empty())
        {
            std::wcerr << L"No supported Office documents were found\n";
            return 2;
        }

        wchar_t executable_buffer[32768]{};
        const DWORD executable_length = GetModuleFileNameW(
            nullptr,
            executable_buffer,
            static_cast<DWORD>(std::size(executable_buffer)));
        if (executable_length == 0 || executable_length == std::size(executable_buffer))
        {
            return 3;
        }
        const auto output_root = std::filesystem::path(executable_buffer).parent_path();
        const auto component_root = output_root / L"components" / L"office";
        const auto component_path = component_root / L"Glance.OfficeComponent.dll";
        const auto host_path = component_root / L"Glance.OfficeHost.exe";
        const HMODULE module = LoadLibraryExW(
            component_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr || !std::filesystem::is_regular_file(host_path))
        {
            if (module != nullptr) FreeLibrary(module);
            std::wcerr << L"Build the Office component before benchmarking\n";
            return 3;
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(module, get_api_export));
        ComponentApi api;
        ComponentRegistrar registrar{
            .register_extension = collect_extension,
            .register_renderer = accept_renderer };
        ComponentRegistration registration;
        void* renderer_pointer{};
        if (get_api == nullptr || get_api(abi_version, &api) == FALSE ||
            api.initialize == nullptr || api.can_preview == nullptr ||
            api.prepare_preview == nullptr || api.release_preview == nullptr ||
            api.query_interface == nullptr || api.shutdown == nullptr ||
            api.initialize(&registrar, &registration) == FALSE ||
            api.query_interface(
                &native_preview_renderer_api_id,
                native_preview_renderer_api_version,
                &renderer_pointer) == FALSE || renderer_pointer == nullptr)
        {
            FreeLibrary(module);
            std::wcerr << L"Office component initialization failed\n";
            return 3;
        }

        const auto repository_root = output_root.parent_path().parent_path().parent_path();
        options.output = options.output.empty()
            ? repository_root / L"artifacts" / L"office-preview-benchmark.csv"
            : std::filesystem::absolute(options.output);
        std::filesystem::create_directories(options.output.parent_path());
        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            api.shutdown();
            FreeLibrary(module);
            return 3;
        }
        write_header(output);

        const HWND parent = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"STATIC",
            L"Glance Office benchmark",
            WS_POPUP,
            0,
            0,
            960,
            720,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        bool all_successful = parent != nullptr;
        std::error_code error;
        for (const auto& input : inputs)
        {
            const auto source_bytes = std::filesystem::file_size(input, error);
            if (error || api.can_preview(input.c_str()) == FALSE)
            {
                all_successful = false;
                error.clear();
                continue;
            }
            for (std::uint32_t iteration = 1; iteration <= options.iterations; ++iteration)
            {
                const auto result = measure(input, api, host_path, parent);
                write_result(output, input, source_bytes, iteration, result);
                all_successful = all_successful && successful(result);
                std::wcout << input.filename() << L" iteration " << iteration
                           << L": prepare " << result.component_prepare_ms
                           << L" ms, host " << result.host_create_ms
                           << L" ms, first surface " << result.first_surface_ready_ms
                           << L" ms, unload " << result.unload_ms << L" ms\n";
            }
        }

        if (parent != nullptr)
        {
            DestroyWindow(parent);
        }
        output.close();
        api.shutdown();
        FreeLibrary(module);
        std::wcout << L"Office preview benchmark: " << options.output << L'\n';
        return all_successful ? 0 : 1;
    }
}
