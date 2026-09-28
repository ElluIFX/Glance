#include "pch.h"
#include "dependencies/dependency_service.h"
#include <bcrypt.h>
#include <fstream>
#include <iostream>
#include <array>
#include <wil/resource.h>

namespace
{
    std::wstring hash_file(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
        std::array<unsigned char, 32> hash{};
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(),
            static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size())) < 0)
            throw std::runtime_error("hash failed");
        std::wstring result;
        for (const auto byte : hash) { result += L"0123456789abcdef"[byte >> 4]; result += L"0123456789abcdef"[byte & 15]; }
        return result;
    }
}

int run_dependency_service_tests()
{
    namespace service = glance::app::dependencies;
    namespace api = glance::contracts::dependencies;
    int failures{};
    const auto expect = [&](bool value, const char* name) {
        std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
        if (!value) ++failures;
    };
    const auto id = L"test-dependency-" + std::to_wstring(GetCurrentProcessId());
    const auto root = service::storage_root() / id;
    std::filesystem::create_directories(root);
    const auto cleanup = wil::scope_exit([&] {
        service::shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    });
    try
    {
        const auto source = root / L"payload.txt";
        { std::ofstream output(source, std::ios::binary); output << "dependency fixture"; }
        const auto archive = root / L"fixture.tar";
        std::array<wchar_t, MAX_PATH> system{};
        GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        const auto tar = api::execute(std::filesystem::path(system.data()) / L"tar.exe",
            {L"-cf", archive.wstring(), L"-C", root.wstring(), L"payload.txt"});
        if (!tar.succeeded()) throw std::runtime_error("fixture archive failed");
        const auto file_hash = hash_file(source), archive_hash = hash_file(archive);
        const api::File file{L"payload.txt", L"bin/payload.txt", file_hash.c_str()};
        const auto legacy_path = L"Dependencies/" + id + L"/legacy.txt";
        const api::Entry entry{L"payload", api::EntryKind::executable, L"bin/payload.txt", nullptr, legacy_path.c_str()};
        api::Declaration declaration{
            .id = id.c_str(), .version = L"1", .display_name = L"Test dependency",
            .url = L"https://example.invalid/fixture.tar", .archive_name = L"fixture.tar",
            .sha256 = archive_hash.c_str(), .archive_size = std::filesystem::file_size(archive),
            .files = &file, .file_count = 1, .entries = &entry, .entry_count = 1};
        const auto& host = service::host_api();
        expect(SUCCEEDED(host.register_dependency(&declaration, L"first")) &&
            SUCCEEDED(host.register_dependency(&declaration, L"second")), "Dependency shared declaration accepted");
        declaration.version = L"2";
        expect(FAILED(host.register_dependency(&declaration, L"third")), "Dependency conflicting declaration rejected");
        declaration.version = L"1";
        bool cancelled{};
        try { service::install_archive(id, archive, [] { return true; }); }
        catch (const winrt::hresult_error& error) { cancelled = error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED); }
        expect(cancelled && host.query(id.c_str(), L"payload") == api::Availability::missing,
            "Dependency cancelled installation remains unavailable");
        service::install_archive(id, archive);
        expect(host.query(id.c_str(), L"payload") == api::Availability::managed &&
            hash_file(root / L"1/bin/payload.txt") == file_hash, "Dependency verified archive installed");
        const auto installed_file = root / L"1/bin/payload.txt";
        const auto moved_file = root / L"moved-payload.txt";
        std::filesystem::rename(installed_file, moved_file);
        expect(host.query(id.c_str(), L"payload") == api::Availability::managed,
            "Dependency status reads the registration cache");
        bool missing{};
        try { static_cast<void>(service::acquire(id, L"payload")); }
        catch (const winrt::hresult_error& error) { missing = error.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }
        expect(missing && host.query(id.c_str(), L"payload") == api::Availability::missing,
            "Dependency invocation revalidates a removed file");
        std::filesystem::rename(moved_file, installed_file);
        {
            const auto lease = service::acquire(id, L"payload");
            bool blocked{};
            try { service::uninstall(id); }
            catch (const winrt::hresult_error& error) { blocked = error.code() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION); }
            expect(blocked && std::filesystem::is_regular_file(lease.path), "Dependency lease blocks uninstall");
        }
        service::uninstall(id);
        expect(host.query(id.c_str(), L"payload") == api::Availability::missing && std::filesystem::is_regular_file(source),
            "Dependency uninstall preserves unrelated files");
        std::filesystem::copy_file(source, root / L"legacy.txt");
        service::migrate_legacy_installations();
        service::shutdown();
        expect(host.query(id.c_str(), L"payload") == api::Availability::managed &&
            !std::filesystem::exists(root / L"legacy.txt"), "Dependency legacy file moves only after hash validation");
        service::uninstall(id);
        std::atomic_uint calls{};
        std::function<void(std::uint64_t, std::uint64_t)> late_progress;
        auto download = [&](const auto&, const std::atomic_bool& stop, const auto& progress) {
            late_progress = progress;
            ++calls;
            while (!stop.load()) Sleep(5);
            glance::contracts::NetworkDownloadResult result;
            result.status = glance::contracts::NetworkDownloadStatus::cancelled;
            return result;
        };
        auto first = service::begin_install(id, download);
        auto second = service::begin_install(id, download);
        expect(first == second, "Dependency concurrent requests share transfer");
        first->cancel();
        service::shutdown();
        expect(calls == 1 && first->state().complete && first->state().error == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "Dependency cancellation joins transfer worker");
        late_progress(1, 2);
        expect(first->state().complete && first->state().error == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "Dependency late progress preserves completion");
        const std::weak_ptr<service::Transfer> released = first;
        first.reset();
        second.reset();
        late_progress(2, 2);
        expect(released.expired(), "Dependency delayed progress does not retain or access a released transfer");
        { std::ofstream output(archive, std::ios::binary | std::ios::app); output << 'x'; }
        bool corrupted{};
        try { service::install_archive(id, archive); }
        catch (const winrt::hresult_error& error) { corrupted = error.code() == HRESULT_FROM_WIN32(ERROR_CRC); }
        expect(corrupted && host.query(id.c_str(), L"payload") == api::Availability::missing,
            "Dependency corrupt archive is never published");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    catch (...) { std::cerr << "Dependency service exception\n"; ++failures; }
    return failures ? 1 : 0;
}
