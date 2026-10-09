#include "glance/contracts/storage.h"
#include <fstream>
#include <iostream>
#include <limits>

int run_storage_tests()
{
    namespace storage = glance::contracts::storage;
    const auto root =
        std::filesystem::temp_directory_path() / (L"Glance-StorageTests-" + std::to_wstring(GetCurrentProcessId()));
    const auto registry = L"Software\\Glance.StorageTests." + std::to_wstring(GetCurrentProcessId());
    struct Cleanup
    {
        std::filesystem::path path;
        std::wstring registry;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
        }
    } cleanup{root, registry};
    std::filesystem::create_directories(root);
    int failures{};
    const auto expect = [&](bool success, const char *label) {
        if (!success)
        {
            std::cerr << "FAIL " << label << '\n';
            ++failures;
        }
    };
    storage::SettingsStore installed(storage::Backend::registry, {}, registry);
    storage::SettingsStore portable(storage::Backend::json, root / L"settings.json");
    storage::SettingsStore concurrent(storage::Backend::json, root / L"settings.json");
    const DWORD dword = 0xffffffff;
    const std::uint64_t qword = (std::numeric_limits<std::uint64_t>::max)();
    const std::int64_t negative = (std::numeric_limits<std::int64_t>::min)();
    const wchar_t text[] = L"中文 Ελληνικά العربية 😀";
    const wchar_t malformed[] = {0xd800, 0};
    const unsigned char binary[] = {0, 0xff, 0x80, 0, 1};
    storage::Batch batch(L"Types", installed);
    batch.set(L"Dword", REG_DWORD, &dword, sizeof(dword));
    batch.set(L"Qword", REG_QWORD, &qword, sizeof(qword));
    batch.set(L"Negative", REG_QWORD, &negative, sizeof(negative));
    batch.set(L"Unicode", REG_SZ, text, sizeof(text));
    batch.set(L"MalformedUtf16", REG_SZ, malformed, sizeof(malformed));
    batch.set(L"Binary", REG_BINARY, binary, sizeof(binary));
    batch.set(L"UnknownType", 42, binary, sizeof(binary));
    batch.set(L"MalformedDword", REG_DWORD, binary, 3);
    expect(batch.commit() == ERROR_SUCCESS, "registry batch writes all value types");
    storage::Snapshot source, decoded, roundtrip;
    expect(installed.snapshot(source) == ERROR_SUCCESS, "complete registry export");
    expect(storage::write_snapshot(root / L"settings.json", source) == ERROR_SUCCESS &&
               portable.snapshot(decoded) == ERROR_SUCCESS && decoded == source,
           "JSON preserves types, unsigned boundaries, signed bits, Unicode, binary and unknown values");
    DWORD number{}, size = sizeof(number);
    expect(installed.read(L"Types", L"MalformedDword", REG_DWORD, &number, &size) == ERROR_INVALID_DATA,
           "registry rejects malformed numeric values");
    size = sizeof(number);
    expect(portable.read(L"Types", L"MalformedDword", REG_DWORD, &number, &size) == ERROR_INVALID_DATA,
           "JSON rejects malformed numeric values without dropping them");
    storage::Batch other(L"Other", concurrent);
    other.set(L"Value", REG_DWORD, &dword, sizeof(dword));
    expect(other.commit() == ERROR_SUCCESS, "second process merges existing data");
    expect(portable.snapshot(decoded) == ERROR_SUCCESS && decoded.contains(L"Types") && decoded.contains(L"Other"),
           "cache observes atomic replacement");
    const auto saved_time = std::filesystem::last_write_time(root / L"settings.json");
    expect(storage::write_snapshot(root / L"settings.json", source) == ERROR_SUCCESS,
           "replace with same timestamp fixture");
    std::filesystem::last_write_time(root / L"settings.json", saved_time);
    expect(portable.snapshot(decoded) == ERROR_SUCCESS && decoded == source,
           "file identity detects replacement with identical write time");
    expect(other.commit() == ERROR_SUCCESS, "empty commit preserves current snapshot");
    other.set(L"Value", REG_DWORD, &dword, sizeof(dword));
    expect(other.commit() == ERROR_SUCCESS, "restore unrelated group");
    expect(portable.clear(L"types") == ERROR_SUCCESS && portable.snapshot(decoded) == ERROR_SUCCESS &&
               !decoded.contains(L"Types") && decoded.contains(L"Other"),
           "case insensitive group clear preserves unrelated values");
    expect(storage::write_snapshot(root / L"roundtrip.json", source) == ERROR_SUCCESS &&
               storage::read_snapshot(root / L"roundtrip.json", roundtrip) == ERROR_SUCCESS && roundtrip == source,
           "snapshot roundtrip preserves unknown keys");
    storage::Batch deletion(L"Other", portable);
    deletion.erase(L"Value");
    expect(deletion.commit() == ERROR_SUCCESS && portable.snapshot(decoded) == ERROR_SUCCESS &&
               !decoded.contains(L"Other"),
           "value removal persists");
    expect(storage::write_snapshot(root / L"settings.json", source) == ERROR_SUCCESS, "restore fixture");
    const HANDLE locked = CreateFileW((root / L"settings.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(locked != INVALID_HANDLE_VALUE, "open replacement lock");
    storage::Batch blocked(L"Types", portable);
    blocked.set(L"Dword", REG_DWORD, binary, sizeof(binary));
    blocked.set(L"Binary", REG_BINARY, nullptr, 0);
    expect(blocked.commit() != ERROR_SUCCESS, "locked file rejects entire batch");
    if (locked != INVALID_HANDLE_VALUE)
        CloseHandle(locked);
    portable.invalidate();
    expect(portable.snapshot(decoded) == ERROR_SUCCESS && decoded == source,
           "failed batch preserves every original value");
    {
        std::ofstream corrupt(root / L"settings.json", std::ios::binary | std::ios::trunc);
        corrupt << "{broken";
    }
    portable.invalidate();
    storage::Batch corrupt_write(L"Types", portable);
    corrupt_write.set(L"Value", REG_DWORD, &dword, sizeof(dword));
    expect(portable.snapshot(decoded) == ERROR_INVALID_DATA && corrupt_write.commit() == ERROR_INVALID_DATA,
           "corrupt JSON rejects reads and writes");
    {
        std::ifstream file(root / L"settings.json");
        std::string contents((std::istreambuf_iterator<char>(file)), {});
        expect(contents == "{broken", "corrupt JSON remains unchanged");
    }
    std::cout << "Storage regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
