#include "../../src/Glance.Components/Executable/host/pe_reader.h"
#include "../../src/Glance.Components/Executable/host/resource_image.h"
#include <windows.h>
#include <objbase.h>
#include <gdiplus.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>

int run_executable_tests()
{
    using namespace glance::executable;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    int failures{};
    const auto expect = [&](bool value, const char* message) {
        std::cout << (value ? "PASS " : "FAIL ") << message << '\n';
        if (!value)
            ++failures;
    };
    try
    {
        const auto started = std::chrono::steady_clock::now();
        const auto summary = read_summary(executable, {});
        expect(summary.architecture == L"x64" && !summary.title.empty(), "native executable summary");
        for (const auto section : {Section::imports, Section::resources, Section::structure})
        {
            const auto table = read_section(executable, summary.identity, section, {});
            expect(table.state == L"Complete", "static section completes");
        }
        const auto imports = read_section(executable, summary.identity, Section::imports, {});
        expect(!imports.rows.empty(), "imports include actual dependencies");
        const auto system_file = std::wstring(L"C:\\Windows\\System32\\notepad.exe");
        const auto system_summary = read_summary(system_file, {});
        const auto system_resources =
            read_section(system_file, system_summary.identity, Section::resources, {});
        const auto icon = std::find_if(system_resources.rows.begin(), system_resources.rows.end(),
                                       [](const Row& row) { return row.type == 14; });
        if (icon != system_resources.rows.end())
        {
            const auto bytes = icon_file(system_file, system_summary.identity, *icon);
            expect(bytes.size() > 22 && bytes[2] == std::byte{1}, "reconstructed ICO directory");
            Gdiplus::GdiplusStartupInput startup;
            ULONG_PTR token{};
            Gdiplus::GdiplusStartup(&token, &startup, nullptr);
            {
                const auto image = resource_image(system_file, system_summary.identity, *icon);
                expect(image.bitmap && image.width > 0, "icon resource preview");
            }
            Gdiplus::GdiplusShutdown(token);
        }
        const auto managed_path =
            std::filesystem::path(L"C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\mscorlib.dll");
        if (std::filesystem::is_regular_file(managed_path))
        {
            const auto managed_summary = read_summary(managed_path.wstring(), {});
            const auto metadata =
                read_section(managed_path.wstring(), managed_summary.identity, Section::managed, {});
            expect(managed_summary.managed && metadata.state == L"Complete" && !metadata.rows.empty(),
                   "CLR metadata and assembly tables");
            for (const auto section : {Section::managed_types, Section::managed_methods, Section::managed_fields})
            {
                const auto members = read_section(managed_path.wstring(), managed_summary.identity, section, {});
                expect(members.state == L"Complete" && !members.rows.empty(), "CLR member table is readable");
                if (section == Section::managed_methods)
                    expect(std::any_of(members.rows.begin(), members.rows.end(), [](const Row& row) {
                        return !row.cells.empty() && row.cells[0] == L"System.Object::ToString";
                    }), "CLR methods retain declaring type and method name");
            }
        }
        const auto cancelled =
            read_section(executable, summary.identity, Section::imports, [] { return true; });
        expect(cancelled.state == L"Cancelled" && cancelled.rows.empty(),
               "cancelled parse publishes no rows");
        auto changed = summary.identity;
        ++changed.size;
        expect(read_section(executable, changed, Section::imports, {}).state == L"Changed",
               "changed identity rejected");
        const auto directory = std::filesystem::current_path() / L".tmp" /
                               (L"executable-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto fixture = directory / L"fixture.exe";
        auto write = [&](const std::vector<char>& bytes) {
            std::ofstream stream(fixture, std::ios::binary | std::ios::trunc);
            stream.write(bytes.data(), bytes.size());
        };
        write({'M', 'Z'});
        bool rejected{};
        try
        {
            static_cast<void>(read_summary(fixture.wstring(), {}));
        }
        catch (...)
        {
            rejected = true;
        }
        expect(rejected, "truncated PE rejected");
        std::vector<char> bytes(1024);
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 128;
        memcpy(bytes.data(), &dos, sizeof(dos));
        IMAGE_NT_HEADERS32 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        nt.OptionalHeader.SizeOfHeaders = 1024;
        nt.OptionalHeader.NumberOfRvaAndSizes = 16;
        memcpy(bytes.data() + 128, &nt, sizeof(nt));
        write(bytes);
        const auto simple = read_summary(fixture.wstring(), {});
        expect(simple.architecture == L"x86", "minimal x86 PE");
        Row raw;
        raw.length = static_cast<std::uint32_t>(bytes.size());
        const auto exported = directory / L"export.bin";
        export_resource(fixture.wstring(), simple.identity, raw, exported.wstring(), {});
        std::ifstream exported_stream(exported, std::ios::binary);
        const std::vector<char> exported_bytes{std::istreambuf_iterator<char>(exported_stream), {}};
        exported_stream.close();
        expect(exported_bytes == bytes, "resource export preserves exact bytes");
        std::filesystem::remove(exported);
        bool cancelled_export{};
        try
        {
            export_resource(fixture.wstring(), simple.identity, raw, exported.wstring(), [] { return true; });
        }
        catch (const std::exception&)
        {
            cancelled_export = true;
        }
        expect(cancelled_export && !std::filesystem::exists(exported), "cancelled export leaves no partial file");
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0xfffffff0, 100};
        memcpy(bytes.data() + 128, &nt, sizeof(nt));
        write(bytes);
        const auto malformed = read_summary(fixture.wstring(), {});
        expect(read_section(fixture.wstring(), malformed.identity, Section::imports, {}).state == L"Invalid",
               "out-of-bounds RVA rejected");
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {};
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE] = {512, 128};
        memcpy(bytes.data() + 128, &nt, sizeof(nt));
        IMAGE_RESOURCE_DIRECTORY resource{};
        resource.NumberOfIdEntries = 1;
        memcpy(bytes.data() + 512, &resource, sizeof(resource));
        IMAGE_RESOURCE_DIRECTORY_ENTRY entry{};
        entry.Name = 3;
        entry.OffsetToData = 0x80000000;
        memcpy(bytes.data() + 528, &entry, sizeof(entry));
        write(bytes);
        const auto cycle = read_summary(fixture.wstring(), {});
        expect(read_section(fixture.wstring(), cycle.identity, Section::resources, {}).state == L"Invalid",
               "cyclic resource directory rejected");
        std::filesystem::remove(fixture);
        std::filesystem::remove(directory);
        std::cout << "Elapsed ms: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                           started)
                         .count()
                  << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        ++failures;
    }
    return failures ? 1 : 0;
}
