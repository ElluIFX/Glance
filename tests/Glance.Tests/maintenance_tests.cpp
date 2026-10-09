#include "glance/contracts/maintenance.h"
#include "glance/contracts/cli_protocol.h"
#include <fstream>
#include <iostream>

int run_maintenance_command(int count, wchar_t *arguments[])
{
    try
    {
        if (count == 5 && std::wstring_view(arguments[1]) == L"--prepare-portable-update")
            glance::contracts::maintenance::prepare_update(arguments[2], arguments[3], arguments[4]);
        else if (count == 3 && std::wstring_view(arguments[1]) == L"--prepare-data-migration")
            glance::contracts::maintenance::prepare_migration(arguments[2]);
        else
            return 2;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

namespace
{
    namespace storage = glance::contracts::storage;
    namespace maintenance = glance::contracts::maintenance;
    void put(storage::Snapshot &state, const wchar_t *group, const wchar_t *name, const wchar_t *text)
    {
        const auto bytes = reinterpret_cast<const std::uint8_t *>(text);
        state[group][name] = {REG_SZ, {bytes, bytes + (wcslen(text) + 1) * sizeof(wchar_t)}};
    }
    std::string text(const std::filesystem::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {(std::istreambuf_iterator<char>(stream)), {}};
    }
} // namespace

int run_maintenance_tests()
{
    const auto root =
        std::filesystem::temp_directory_path() / (L"Glance-MaintenanceTests-" + std::to_wstring(GetCurrentProcessId()));
    const auto registry = L"Software\\Glance.MaintenanceTests." + std::to_wstring(GetCurrentProcessId());
    wchar_t original_appdata[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", original_appdata, ARRAYSIZE(original_appdata));
    HKEY isolated{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolated,
                        nullptr) != ERROR_SUCCESS)
        return 1;
    struct Cleanup
    {
        std::filesystem::path root;
        std::wstring registry, appdata;
        HKEY key;
        ~Cleanup()
        {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            RegCloseKey(key);
            RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
            SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str());
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root, registry, original_appdata, isolated};
    if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated) != ERROR_SUCCESS)
        return 1;
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"installed").c_str());
    int failures{};
    const auto expect = [&](bool success, const char *label) {
        std::cout << (success ? "PASS " : "FAIL ") << label << '\n';
        if (!success)
            ++failures;
    };
    const auto application = root / L"portable";
    const auto work = application / L".glance-maintenance";
    std::filesystem::create_directories(application / L"data");
    {
        std::ofstream stream(application / L"data/old.txt");
        stream << "original portable data";
    }
    expect(!maintenance::installed_data_available(), "absent migration source is unavailable");
    bool missing{};
    try
    {
        maintenance::prepare_migration(application);
    }
    catch (...)
    {
        missing = true;
    }
    expect(missing && text(application / L"data/old.txt") == "original portable data",
           "missing source preserves portable data");
    const auto source = storage::installed_data_directory();
    std::filesystem::create_directories(source / L"Dependencies/example");
    {
        std::ofstream stream(source / L"Dependencies/example/file.bin");
        stream << "installed payload";
    }
    storage::SettingsStore installed(storage::Backend::registry);
    storage::Batch batch(L"Components/example", installed);
    const std::int64_t number = -123;
    batch.set(L"Unknown", REG_QWORD, &number, sizeof(number));
    expect(batch.commit() == ERROR_SUCCESS && maintenance::installed_data_available(),
           "source settings and data detected");
    const HANDLE locked_source = CreateFileW((source / L"Dependencies/example/file.bin").c_str(), GENERIC_WRITE, 0,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool occupied{};
    try
    {
        maintenance::prepare_migration(application);
    }
    catch (...)
    {
        occupied = true;
    }
    if (locked_source != INVALID_HANDLE_VALUE)
        CloseHandle(locked_source);
    expect(occupied && text(application / L"data/old.txt") == "original portable data",
           "occupied source stops migration and preserves target");
    maintenance::prepare_migration(application);
    expect(text(work / L"stage/data/Dependencies/example/file.bin") == "installed payload" &&
               std::filesystem::exists(application / L"data/old.txt"),
           "preparation verifies payload without overwriting portable data");
    storage::Snapshot journal;
    if (storage::read_snapshot(work / L"journal.json", journal) != ERROR_SUCCESS)
        return 1;
    put(journal, L"Job", L"state", L"committing");
    if (storage::write_snapshot(work / L"journal.json", journal) != ERROR_SUCCESS)
        return 1;
    std::filesystem::create_directories(work / L"backup");
    std::filesystem::rename(application / L"data", work / L"backup/data");
    std::filesystem::rename(work / L"stage/data", application / L"data");
    maintenance::recover(application);
    expect(text(application / L"data/old.txt") == "original portable data" &&
               std::filesystem::exists(source / L"Dependencies/example/file.bin"),
           "interrupted commit restores original portable data and source");
    expect(maintenance::failure(application) == ERROR_PROCESS_ABORTED, "interrupted transaction reports recovery");
    maintenance::finish_migration(application);
    maintenance::prepare_migration(application);
    std::filesystem::create_directories(work / L"backup");
    std::filesystem::rename(application / L"data", work / L"backup/data");
    std::filesystem::rename(work / L"stage/data", application / L"data");
    if (storage::read_snapshot(work / L"journal.json", journal) != ERROR_SUCCESS)
        return 1;
    put(journal, L"Job", L"state", L"complete");
    if (storage::write_snapshot(work / L"journal.json", journal) != ERROR_SUCCESS)
        return 1;
    storage::Snapshot portable_values, installed_values;
    expect(storage::read_snapshot(application / L"data/settings.json", portable_values) == ERROR_SUCCESS &&
               installed.snapshot(installed_values) == ERROR_SUCCESS && portable_values == installed_values,
           "component and unknown settings migrate exactly");
    {
        std::ofstream stream(source / L"changed.txt");
        stream << "new data";
    }
    bool changed{};
    try
    {
        maintenance::delete_migration_source(application);
    }
    catch (...)
    {
        changed = true;
    }
    expect(changed && std::filesystem::exists(source / L"changed.txt"), "modified migration source is retained");
    std::filesystem::remove(source / L"changed.txt");
    maintenance::delete_migration_source(application);
    expect(!std::filesystem::exists(source) && installed.snapshot(installed_values) == ERROR_SUCCESS &&
               installed_values.empty(),
           "verified unchanged source cleanup removes files and registry");
    maintenance::finish_migration(application);
    expect(text(application / L"data/Dependencies/example/file.bin") == "installed payload",
           "cleanup preserves migrated data");
    std::cout << "Maintenance regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
