#include "pch.h"
#include "dependency_service.h"
#include <shlobj.h>
#include <wil/resource.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace glance::app::dependencies
{
    namespace
    {
        namespace api = contracts::dependencies;
        using EntryStates = std::map<std::wstring, api::Availability, std::less<>>;
        struct Registration { Definition definition; std::set<std::wstring> consumers; EntryStates states; };
        EntryStates resolve_states(const Definition& definition);
        void refresh_states(const Definition& definition);
        struct LoadedLibrary
        {
            Lease lease;
            api::Library library;
            explicit LoadedLibrary(Lease value) : lease(std::move(value)), library(lease.path) {}
        };
        std::mutex registry_mutex;
        std::map<std::wstring, Registration, std::less<>> registrations;
        std::map<std::uint64_t, std::shared_ptr<LoadedLibrary>> libraries;
        std::uint64_t next_library{};
        struct Transfers
        {
            std::mutex mutex;
            std::map<std::wstring, std::shared_ptr<Transfer>, std::less<>> items;
            Downloader download;
            std::thread migration;
            ~Transfers()
            {
                if (migration.joinable()) migration.join();
                for (const auto& [id, task] : items) task->cancel();
                for (const auto& [id, task] : items) if (task->worker.joinable()) task->worker.join();
            }
        } transfers;

        std::wstring sha256_file(const std::filesystem::path& path)
        {
            wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file) winrt::throw_last_error();
            BCRYPT_ALG_HANDLE algorithm{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                throw winrt::hresult_error(E_FAIL);
            const auto close_algorithm = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(algorithm, 0); });
            BCRYPT_HASH_HANDLE hash{};
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
                throw winrt::hresult_error(E_FAIL);
            const auto close_hash = wil::scope_exit([&] { BCryptDestroyHash(hash); });
            std::array<unsigned char, 65536> buffer{};
            for (;;)
            {
                DWORD read{};
                winrt::check_bool(ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr));
                if (!read) break;
                if (BCryptHashData(hash, buffer.data(), read, 0) < 0) throw winrt::hresult_error(E_FAIL);
            }
            std::array<unsigned char, 32> digest{};
            if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
                throw winrt::hresult_error(E_FAIL);
            constexpr wchar_t hex[] = L"0123456789abcdef";
            std::wstring result;
            for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
            return result;
        }

        Definition get_definition(std::wstring_view id)
        {
            std::scoped_lock lock(registry_mutex);
            const auto found = registrations.find(id);
            if (found == registrations.end()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            return found->second.definition;
        }

        wil::unique_hfile lock_dependency(const Definition& definition, bool exclusive)
        {
            const auto parent = storage_root() / definition.id;
            std::filesystem::create_directories(parent);
            const auto gate = parent / L".lease";
            wil::unique_hfile handle(CreateFileW(gate.c_str(), GENERIC_READ,
                exclusive ? 0 : FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
            if (!handle) winrt::throw_last_error();
            return handle;
        }

        bool safe_id(std::wstring_view value)
        {
            return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
            });
        }
        bool relative_path(std::wstring_view value)
        {
            const std::filesystem::path path(value);
            if (value.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
                value.find_first_of(L":*?\"<>|") != std::wstring_view::npos) return false;
            for (const auto& part : path) if (part == L".." || part == L".") return false;
            return true;
        }
        bool hash_string(std::wstring_view value)
        {
            return value.size() == 64 && std::ranges::all_of(value, [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
            });
        }
        std::wstring copy(const wchar_t* value) { return value ? value : L""; }

        Definition definition_from(const api::Declaration& value)
        {
            Definition result{copy(value.id), copy(value.version), copy(value.display_name), copy(value.url),
                copy(value.archive_name), copy(value.sha256), value.archive_size, {}, {}, copy(value.description_key)};
            if (!safe_id(result.id) || !relative_path(result.version) ||
                std::filesystem::path(result.version).has_parent_path() || result.display_name.empty() ||
                !result.url.starts_with(L"https://") || !relative_path(result.archive_name) ||
                std::filesystem::path(result.archive_name).has_parent_path() || !hash_string(result.sha256) ||
                !result.archive_size || !value.files || !value.entries || !value.file_count || !value.entry_count ||
                value.file_count > 128 || value.entry_count > 32) throw std::invalid_argument("Invalid dependency declaration");
            std::set<std::wstring> files, entries;
            for (std::uint32_t i = 0; i < value.file_count; ++i)
            {
                const auto& file = value.files[i];
                File owned{copy(file.archive_path), copy(file.installed_path), copy(file.sha256)};
                if (!relative_path(owned.archive_path) || !relative_path(owned.installed_path) ||
                    !hash_string(owned.sha256) || !files.insert(owned.installed_path).second)
                    throw std::invalid_argument("Invalid dependency file");
                result.files.push_back(std::move(owned));
            }
            for (std::uint32_t i = 0; i < value.entry_count; ++i)
            {
                const auto& entry = value.entries[i];
                Entry owned{copy(entry.id), copy(entry.installed_path), copy(entry.external_file_name), entry.kind,
                    copy(entry.legacy_relative_path)};
                if (!safe_id(owned.id) || !entries.insert(owned.id).second || !files.contains(owned.installed_path) ||
                    (!owned.legacy_relative_path.empty() && !relative_path(owned.legacy_relative_path)) ||
                    (owned.kind != api::EntryKind::library && owned.kind != api::EntryKind::executable) ||
                    (!owned.external_file_name.empty() && (owned.kind != api::EntryKind::executable ||
                        !relative_path(owned.external_file_name) || std::filesystem::path(owned.external_file_name).has_parent_path())))
                    throw std::invalid_argument("Invalid dependency entry");
                result.entries.push_back(std::move(owned));
            }
            return result;
        }
        HRESULT WINAPI register_dependency(const api::Declaration* value, const wchar_t* consumer) noexcept
        {
            try
            {
                if (!value || value->size < sizeof(*value) || !consumer || !safe_id(consumer)) return E_INVALIDARG;
                auto definition = definition_from(*value);
                std::unique_lock lock(registry_mutex);
                auto found = registrations.find(definition.id);
                EntryStates states;
                if (found == registrations.end())
                {
                    lock.unlock();
                    states = resolve_states(definition);
                    lock.lock();
                    found = registrations.find(definition.id);
                }
                if (found != registrations.end())
                {
                    if (found->second.definition != definition) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                    found->second.consumers.insert(consumer);
                }
                else
                {
                    const auto id = definition.id;
                    registrations.emplace(id, Registration{std::move(definition), {consumer}, std::move(states)});
                }
                return S_OK;
            }
            catch (const std::invalid_argument&) { return E_INVALIDARG; }
            catch (...) { return winrt::to_hresult(); }
        }
        std::pair<Definition, Entry> lookup(std::wstring_view dependency, std::wstring_view entry)
        {
            std::scoped_lock lock(registry_mutex);
            const auto found = registrations.find(dependency);
            if (found == registrations.end()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            const auto& definition = found->second.definition;
            const auto item = std::ranges::find(definition.entries, entry, &Entry::id);
            if (item == definition.entries.end()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            return {definition, *item};
        }
        std::filesystem::path managed_path(const Definition& definition, const Entry& entry)
        {
            return storage_root() / definition.id / definition.version / entry.installed_path;
        }
        bool managed_available(const Definition& definition)
        {
            const auto directory = storage_root() / definition.id / definition.version;
            return std::ranges::all_of(definition.entries, [&](const Entry& entry) {
                return std::filesystem::is_regular_file(directory / entry.installed_path);
            });
        }
        std::filesystem::path external_path(const Entry& entry)
        {
            if (entry.kind != api::EntryKind::executable || entry.external_file_name.empty()) return {};
            const auto size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (!size) return {};
            std::wstring directories(size, L'\0');
            if (!GetEnvironmentVariableW(L"PATH", directories.data(), size)) return {};
            std::wstring path(32768, L'\0');
            const auto length = SearchPathW(directories.c_str(), entry.external_file_name.c_str(),
                nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
            if (!length || length >= path.size()) return {};
            path.resize(length);
            return std::filesystem::is_regular_file(path) ? std::filesystem::path(path) : std::filesystem::path{};
        }
        EntryStates resolve_states(const Definition& definition)
        {
            EntryStates states;
            bool managed{};
            try { managed = managed_available(definition); } catch (...) {}
            for (const auto& entry : definition.entries)
            {
                auto availability = api::Availability::missing;
                try
                {
                    if (managed) availability = api::Availability::managed;
                    else if (!external_path(entry).empty()) availability = api::Availability::external;
                }
                catch (...) {}
                states.emplace(entry.id, availability);
            }
            return states;
        }
        void refresh_states(const Definition& definition)
        {
            auto states = resolve_states(definition);
            std::scoped_lock lock(registry_mutex);
            const auto found = registrations.find(definition.id);
            if (found != registrations.end() && found->second.definition == definition)
                found->second.states = std::move(states);
        }
        api::Availability WINAPI query(const wchar_t* dependency, const wchar_t* entry) noexcept
        {
            try
            {
                if (!dependency || !entry) return api::Availability::missing;
                std::scoped_lock lock(registry_mutex);
                const auto found = registrations.find(dependency);
                if (found == registrations.end()) return api::Availability::missing;
                const auto state = found->second.states.find(entry);
                return state == found->second.states.end() ? api::Availability::missing : state->second;
            }
            catch (...) { return api::Availability::missing; }
        }
        HRESULT WINAPI execute(const api::ProcessRequest* request, api::ProcessResult* result) noexcept
        {
            try
            {
                if (!request || request->size < sizeof(*request) || !result || result->size < sizeof(*result) ||
                    !request->dependency_id || !request->entry_id || request->argument_count > 256 ||
                    (request->argument_count && !request->arguments)) return E_INVALIDARG;
                *result = {};
                const auto [definition, entry] = lookup(request->dependency_id, request->entry_id);
                if (entry.kind != api::EntryKind::executable) return E_INVALIDARG;
                auto lease = acquire(request->dependency_id, request->entry_id);
                std::vector<std::wstring> arguments;
                for (std::uint32_t i = 0; i < request->argument_count; ++i)
                {
                    if (!request->arguments[i]) return E_INVALIDARG;
                    arguments.emplace_back(request->arguments[i]);
                }
                const auto output = api::execute(lease.path, arguments, {
                    request->timeout_ms, request->maximum_output_bytes, [request] {
                        const auto& cancel = request->cancellation;
                        return cancel.requested && cancel.requested(cancel.context);
                    }});
                result->exit_code = output.exit_code;
                switch (output.status)
                {
                case api::ExecutionStatus::completed: result->result = S_OK; break;
                case api::ExecutionStatus::cancelled: result->result = HRESULT_FROM_WIN32(ERROR_CANCELLED); break;
                case api::ExecutionStatus::timed_out: result->result = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break;
                case api::ExecutionStatus::output_limit: result->result = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); break;
                default: result->result = HRESULT_FROM_WIN32(output.error ? output.error : ERROR_GEN_FAILURE); break;
                }
                const auto& sink = request->output;
                if (sink.append &&
                    ((!output.output.empty() && !sink.append(sink.context, FALSE, output.output.data(), static_cast<std::uint32_t>(output.output.size()))) ||
                     (!output.errors.empty() && !sink.append(sink.context, TRUE, output.errors.data(), static_cast<std::uint32_t>(output.errors.size())))))
                    result->result = E_ABORT;
                return result->result;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        HRESULT WINAPI load_library(const wchar_t* dependency, const wchar_t* entry, std::uint64_t* token) noexcept
        {
            try
            {
                if (!dependency || !entry || !token) return E_INVALIDARG;
                *token = 0;
                if (lookup(dependency, entry).second.kind != api::EntryKind::library) return E_INVALIDARG;
                auto value = std::make_shared<LoadedLibrary>(acquire(dependency, entry));
                std::scoped_lock lock(registry_mutex);
                *token = ++next_library;
                libraries.emplace(*token, std::move(value));
                return S_OK;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        FARPROC WINAPI find_symbol(std::uint64_t token, const char* name) noexcept
        {
            std::scoped_lock lock(registry_mutex);
            const auto found = libraries.find(token);
            return found == libraries.end() ? nullptr : found->second->library.symbol(name);
        }
        void WINAPI release_library(std::uint64_t token) noexcept
        {
            std::shared_ptr<LoadedLibrary> released;
            {
                std::scoped_lock lock(registry_mutex);
                const auto found = libraries.find(token);
                if (found == libraries.end()) return;
                released = std::move(found->second);
                libraries.erase(found);
            }
        }
    }

    std::filesystem::path storage_root()
    {
        PWSTR path{};
        winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path));
        const wil::unique_cotaskmem_string owner(path);
        return std::filesystem::path(path) / L"Glance" / L"Dependencies";
    }

    void unregister_consumer(std::wstring_view consumer)
    {
        std::scoped_lock lock(registry_mutex);
        for (auto it = registrations.begin(); it != registrations.end();)
        {
            it->second.consumers.erase(std::wstring(consumer));
            if (it->second.consumers.empty()) it = registrations.erase(it);
            else ++it;
        }
    }

    Lease acquire(std::wstring_view dependency, std::wstring_view entry)
    {
        const auto [definition, item] = lookup(dependency, entry);
        const auto handle = lock_dependency(definition, false).release();
        std::shared_ptr<void> guard(handle, [](void* value) { CloseHandle(value); });
        refresh_states(definition);
        const auto path = managed_path(definition, item);
        if (!managed_available(definition))
        {
            if (const auto external = external_path(item); !external.empty()) return {external, {}};
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        }
        return {path, std::move(guard)};
    }

    void install_archive(std::wstring_view id, const std::filesystem::path& archive,
        const std::function<bool()>& cancelled)
    {
        const auto definition = get_definition(id);
        const auto lock = lock_dependency(definition, true);
        const auto check_cancelled = [&] {
            if (cancelled && cancelled()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        };
        check_cancelled();
        if (std::filesystem::file_size(archive) != definition.archive_size || sha256_file(archive) != definition.sha256)
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CRC));
        const auto parent = storage_root() / definition.id;
        const auto stage = parent / (L".staging-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(stage);
        const auto cleanup = wil::scope_exit([&] { std::error_code error; std::filesystem::remove_all(stage, error); });
        std::array<wchar_t, MAX_PATH> system{};
        if (!GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()))) winrt::throw_last_error();
        std::vector<std::wstring> arguments{L"-xf", std::filesystem::absolute(archive).wstring(), L"-C", stage.wstring(), L"--"};
        for (const auto& file : definition.files) arguments.push_back(file.archive_path);
        const auto extracted = api::execute(std::filesystem::path(system.data()) / L"tar.exe", arguments,
            {.timeout_ms = 120000, .maximum_output_bytes = 1024 * 1024, .cancelled = cancelled});
        check_cancelled();
        if (!extracted.succeeded()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        const auto prepared = stage / L".prepared";
        std::filesystem::create_directory(prepared);
        for (const auto& file : definition.files)
        {
            check_cancelled();
            const auto source = stage / file.archive_path;
            for (auto part = source; part != stage; part = part.parent_path())
            {
                const auto attributes = GetFileAttributesW(part.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            }
            if (sha256_file(source) != file.sha256) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CRC));
            const auto destination = prepared / file.installed_path;
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(source, destination);
        }
        check_cancelled();
        const auto destination = parent / definition.version;
        const auto previous = stage / L".previous";
        const bool exists = std::filesystem::exists(destination);
        if (exists) std::filesystem::rename(destination, previous);
        try { std::filesystem::rename(prepared, destination); }
        catch (...)
        {
            if (exists) std::filesystem::rename(previous, destination);
            throw;
        }
        refresh_states(definition);
    }

    void uninstall(std::wstring_view id)
    {
        const auto definition = get_definition(id);
        const auto lock = lock_dependency(definition, true);
        try { std::filesystem::remove_all(storage_root() / definition.id / definition.version); }
        catch (...) { refresh_states(definition); throw; }
        refresh_states(definition);
    }

    Transfer::~Transfer()
    {
        cancel();
        if (worker.joinable()) worker.join();
    }
    TransferState Transfer::state() const
    {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    void Transfer::cancel() noexcept { cancellation.store(true, std::memory_order_release); }
    void Transfer::subscribe(std::function<void()> changed)
    {
        if (!changed) return;
        std::scoped_lock lock(mutex_);
        observers_.push_back(std::move(changed));
    }
    void Transfer::publish(TransferState value)
    {
        std::vector<std::function<void()>> observers;
        {
            std::scoped_lock lock(mutex_);
            if (state_.complete) return;
            state_ = value;
            observers = observers_;
            if (value.complete) observers_.clear();
        }
        for (const auto& changed : observers)
            try { changed(); } catch (...) {}
    }
    std::shared_ptr<Transfer> transfer(std::wstring_view id)
    {
        std::scoped_lock lock(transfers.mutex);
        const auto found = transfers.items.find(id);
        return found == transfers.items.end() ? nullptr : found->second;
    }
    std::shared_ptr<Transfer> begin_install(std::wstring_view id, Downloader download)
    {
        if (!download) throw winrt::hresult_error(E_INVALIDARG);
        const auto definition = get_definition(id);
        std::shared_ptr<Transfer> previous;
        std::unique_lock lock(transfers.mutex);
        auto& slot = transfers.items[definition.id];
        if (slot && !slot->state().complete) return slot;
        auto task = std::make_shared<Transfer>();
        task->publish({api::Availability::downloading, 0, definition.archive_size});
        // The registry owns the task until completion; teardown cancels and joins
        // all workers before destroying the registry and its callbacks.
        task->worker = std::thread([task = task.get(), weak_task = std::weak_ptr<Transfer>(task), definition, download = std::move(download)] {
            TransferState state{api::Availability::downloading, 0, definition.archive_size};
            try
            {
                const contracts::NetworkDownloadRequest request{
                    definition.url, definition.archive_name, definition.sha256, definition.archive_size};
                const auto result = download(request, task->cancellation, [weak_task](std::uint64_t bytes, std::uint64_t total) {
                    if (const auto active = weak_task.lock())
                        active->publish({api::Availability::downloading, bytes, total});
                });
                if (result.status == contracts::NetworkDownloadStatus::cancelled || task->cancellation.load())
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                if (result.status != contracts::NetworkDownloadStatus::succeeded)
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(
                        result.status == contracts::NetworkDownloadStatus::integrity_error ? ERROR_CRC : ERROR_CONNECTION_ABORTED));
                state.availability = api::Availability::installing;
                state.downloaded = definition.archive_size;
                task->publish(state);
                install_archive(definition.id, result.path, [task] { return task->cancellation.load(); });
                state.availability = api::Availability::managed;
            }
            catch (...) { state.error = winrt::to_hresult(); state.availability = api::Availability::failed; }
            state.complete = true;
            task->publish(state);
        });
        previous = std::exchange(slot, task);
        lock.unlock();
        return task;
    }

    void configure_downloader(Downloader download)
    {
        std::scoped_lock lock(transfers.mutex);
        transfers.download = std::move(download);
    }

    void migrate_legacy_installations()
    {
        std::scoped_lock guard(transfers.mutex);
        if (transfers.migration.joinable()) return;
        transfers.migration = std::thread([] {
            try
            {
                for (const auto& item : snapshot())
                {
                    for (const auto& entry : item.definition.entries)
                    {
                        if (entry.legacy_relative_path.empty()) continue;
                        const auto source = storage_root().parent_path() / entry.legacy_relative_path;
                        const auto destination = managed_path(item.definition, entry);
                        if (std::filesystem::is_regular_file(destination) || !std::filesystem::is_regular_file(source)) continue;
                        try
                        {
                            const auto lock = lock_dependency(item.definition, true);
                            const auto file = std::ranges::find(item.definition.files, entry.installed_path, &File::installed_path);
                            if (file == item.definition.files.end() || sha256_file(source) != file->sha256) continue;
                            std::filesystem::create_directories(destination.parent_path());
                            const auto staging = destination.wstring() + L".migrating";
                            const auto cleanup = wil::scope_exit([&] { std::error_code error; std::filesystem::remove(staging, error); });
                            std::filesystem::copy_file(source, staging, std::filesystem::copy_options::overwrite_existing);
                            if (sha256_file(staging) != file->sha256) continue;
                            std::filesystem::rename(staging, destination);
                            refresh_states(item.definition);
                            std::error_code error;
                            std::filesystem::remove(source, error);
                        }
                        catch (...) {}
                    }
                }
            }
            catch (...) {}
        });
    }

    std::shared_ptr<Transfer> begin_install(std::wstring_view id)
    {
        Downloader download;
        {
            std::scoped_lock lock(transfers.mutex);
            download = transfers.download;
        }
        return begin_install(id, std::move(download));
    }

    void shutdown()
    {
        if (transfers.migration.joinable()) transfers.migration.join();
        decltype(transfers.items) tasks;
        {
            std::scoped_lock lock(transfers.mutex);
            transfers.download = {};
            tasks.swap(transfers.items);
        }
        for (const auto& [id, task] : tasks) task->cancel();
        for (const auto& [id, task] : tasks) if (task->worker.joinable()) task->worker.join();
    }

    std::vector<Snapshot> snapshot()
    {
        std::vector<Snapshot> result;
        {
            std::scoped_lock lock(registry_mutex);
            for (const auto& [id, registration] : registrations)
                result.push_back({registration.definition,
                    {registration.consumers.begin(), registration.consumers.end()}, api::Availability::missing});
        }
        for (auto& item : result)
        {
            if (std::ranges::all_of(item.definition.entries, [&](const Entry& entry) {
                return query(item.definition.id.c_str(), entry.id.c_str()) == api::Availability::managed;
            })) item.availability = api::Availability::managed;
            else if (std::ranges::all_of(item.definition.entries, [&](const Entry& entry) {
                const auto state = query(item.definition.id.c_str(), entry.id.c_str());
                return state == api::Availability::managed || state == api::Availability::external;
            })) item.availability = api::Availability::external;
            if (const auto task = transfer(item.definition.id); task && !task->state().complete)
                item.availability = task->state().availability;
        }
        return result;
    }

    const api::HostApi& host_api()
    {
        static const api::HostApi value{.register_dependency = register_dependency, .query = query,
            .execute = execute, .load_library = load_library, .find_symbol = find_symbol, .release_library = release_library};
        return value;
    }
}
