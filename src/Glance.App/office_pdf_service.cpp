#include "pch.h"
#include "office_pdf_service.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr DWORD conversion_timeout_ms = 120000;
    constexpr DWORD cooperative_cancel_timeout_ms = 6000;
    constexpr auto shutdown_wait = std::chrono::seconds(7);
    constexpr std::size_t copy_buffer_size = 1024U * 1024U;

    std::atomic_uint64_t temporary_sequence{};

    class UniqueHandle final
    {
    public:
        UniqueHandle() = default;

        explicit UniqueHandle(HANDLE value) noexcept
            : value_(value)
        {
        }

        ~UniqueHandle()
        {
            reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : value_(std::exchange(other.value_, nullptr))
        {
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(std::exchange(other.value_, nullptr));
            }
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
        }

        HANDLE release() noexcept
        {
            return std::exchange(value_, nullptr);
        }

        void reset(HANDLE value = nullptr) noexcept
        {
            if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value_);
            }
            value_ = value;
        }

    private:
        HANDLE value_{};
    };

    enum class OfficeKind : std::uint32_t
    {
        word = 1,
        excel = 2,
        powerpoint = 3,
    };

    struct SourceFile
    {
        UniqueHandle handle;
        OfficeKind kind{};
        std::wstring kind_name;
        std::wstring extension;
        std::wstring cache_key;
        LONGLONG size{};
        LONGLONG last_write_time{};
    };

    template <typename T>
    void append_value(std::vector<std::byte>& output, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), begin, begin + sizeof(T));
    }

    void append_bytes(std::vector<std::byte>& output, const void* data, std::size_t size)
    {
        const auto* begin = static_cast<const std::byte*>(data);
        output.insert(output.end(), begin, begin + size);
    }

    std::wstring executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path().wstring();
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result = L"\"";
        std::size_t backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring hash_metadata(const std::vector<std::byte>& metadata)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        if (BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0)
        {
            return {};
        }

        DWORD object_size{};
        DWORD hash_size{};
        DWORD copied{};
        const bool properties_read =
            BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &copied,
                0) >= 0 &&
            BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size),
                sizeof(hash_size),
                &copied,
                0) >= 0;
        if (!properties_read || hash_size == 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }

        std::vector<UCHAR> object(object_size);
        std::vector<UCHAR> hash(hash_size);
        BCRYPT_HASH_HANDLE hash_handle{};
        const bool succeeded =
            BCryptCreateHash(
                algorithm,
                &hash_handle,
                object.data(),
                static_cast<ULONG>(object.size()),
                nullptr,
                0,
                0) >= 0 &&
            BCryptHashData(
                hash_handle,
                reinterpret_cast<PUCHAR>(const_cast<std::byte*>(metadata.data())),
                static_cast<ULONG>(metadata.size()),
                0) >= 0 &&
            BCryptFinishHash(
                hash_handle,
                hash.data(),
                static_cast<ULONG>(hash.size()),
                0) >= 0;
        if (hash_handle != nullptr)
        {
            BCryptDestroyHash(hash_handle);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!succeeded)
        {
            return {};
        }

        constexpr wchar_t hexadecimal[] = L"0123456789abcdef";
        std::wstring result(hash.size() * 2U, L'0');
        for (std::size_t index = 0; index < hash.size(); ++index)
        {
            result[index * 2U] = hexadecimal[hash[index] >> 4U];
            result[index * 2U + 1U] = hexadecimal[hash[index] & 0x0FU];
        }
        return result;
    }

    bool office_kind_for_path(
        const std::filesystem::path& path,
        OfficeKind& kind,
        std::wstring& kind_name)
    {
        const auto extension = path.extension().wstring();
        if (_wcsicmp(extension.c_str(), L".doc") == 0 ||
            _wcsicmp(extension.c_str(), L".docx") == 0)
        {
            kind = OfficeKind::word;
            kind_name = L"word";
            return true;
        }
        if (_wcsicmp(extension.c_str(), L".xls") == 0 ||
            _wcsicmp(extension.c_str(), L".xlsx") == 0)
        {
            kind = OfficeKind::excel;
            kind_name = L"excel";
            return true;
        }
        if (_wcsicmp(extension.c_str(), L".ppt") == 0 ||
            _wcsicmp(extension.c_str(), L".pptx") == 0)
        {
            kind = OfficeKind::powerpoint;
            kind_name = L"powerpoint";
            return true;
        }
        return false;
    }

    std::wstring normalized_path(const std::wstring& path)
    {
        std::error_code error;
        auto absolute = std::filesystem::absolute(path, error).lexically_normal().wstring();
        if (error)
        {
            absolute = path;
        }
        CharLowerBuffW(absolute.data(), static_cast<DWORD>(absolute.size()));
        return absolute;
    }

    SourceFile open_source_file(const std::wstring& path)
    {
        SourceFile result;
        const std::filesystem::path filesystem_path(path);
        if (!office_kind_for_path(filesystem_path, result.kind, result.kind_name))
        {
            return result;
        }
        result.extension = filesystem_path.extension().wstring();

        result.handle.reset(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!result.handle)
        {
            return {};
        }

        FILE_STANDARD_INFO standard{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(
                result.handle.get(),
                FileStandardInfo,
                &standard,
                sizeof(standard)) ||
            !GetFileInformationByHandleEx(
                result.handle.get(),
                FileBasicInfo,
                &basic,
                sizeof(basic)) ||
            standard.Directory)
        {
            return {};
        }

        std::vector<std::byte> metadata;
        constexpr std::uint32_t fingerprint_version = 1;
        append_value(metadata, fingerprint_version);
        append_value(metadata, result.kind);
        append_value(metadata, standard.EndOfFile.QuadPart);
        append_value(metadata, basic.LastWriteTime.QuadPart);
        result.size = standard.EndOfFile.QuadPart;
        result.last_write_time = basic.LastWriteTime.QuadPart;

        FILE_ID_INFO file_id{};
        const bool has_file_id = GetFileInformationByHandleEx(
            result.handle.get(),
            FileIdInfo,
            &file_id,
            sizeof(file_id));
        append_value(metadata, has_file_id);
        if (has_file_id)
        {
            append_value(metadata, file_id.VolumeSerialNumber);
            append_bytes(metadata, file_id.FileId.Identifier, sizeof(file_id.FileId.Identifier));
        }
        else
        {
            const auto fallback_path = normalized_path(path);
            append_bytes(
                metadata,
                fallback_path.data(),
                fallback_path.size() * sizeof(wchar_t));
        }

        result.cache_key = hash_metadata(metadata);
        if (result.cache_key.empty())
        {
            return {};
        }
        return result;
    }

    std::filesystem::path cache_root()
    {
        std::error_code error;
        const auto root =
            std::filesystem::temp_directory_path(error) / L"Glance" / L"OfficePdfCache" / L"v1";
        if (error)
        {
            return {};
        }
        std::filesystem::create_directories(root, error);
        return error ? std::filesystem::path{} : root;
    }

    bool valid_pdf(const std::filesystem::path& path)
    {
        UniqueHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file)
        {
            return false;
        }
        LARGE_INTEGER size{};
        std::array<char, 5> signature{};
        DWORD read{};
        return GetFileSizeEx(file.get(), &size) &&
            size.QuadPart > static_cast<LONGLONG>(signature.size()) &&
            ReadFile(
                file.get(),
                signature.data(),
                static_cast<DWORD>(signature.size()),
                &read,
                nullptr) &&
            read == signature.size() &&
            std::string_view(signature.data(), signature.size()) == "%PDF-";
    }

    std::wstring temporary_suffix()
    {
        return std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    }

    std::wstring handle_argument(HANDLE handle)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    }

    bool write_exact(HANDLE file, const std::byte* bytes, std::size_t size)
    {
        while (size != 0)
        {
            DWORD written{};
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(file, bytes, requested, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    struct ConversionJob final
    {
        glance::app::OfficePdfResult wait()
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [this] { return complete || cancelled; });
            return cancelled && !complete
                ? glance::app::OfficePdfResult{
                    .status = glance::app::OfficePdfStatus::cancelled }
                : result;
        }

        void finish(glance::app::OfficePdfResult value)
        {
            {
                std::scoped_lock lock(mutex);
                result = std::move(value);
                complete = true;
                process = nullptr;
                process_job = nullptr;
            }
            condition.notify_all();
        }

        void wait_until_complete()
        {
            std::unique_lock lock(mutex);
            static_cast<void>(condition.wait_for(
                lock,
                shutdown_wait,
                [this] { return complete; }));
        }

        void attach_process(HANDLE value, HANDLE job, HANDLE cancellation)
        {
            std::scoped_lock lock(mutex);
            process = value;
            process_job = job;
            cancellation_event = cancellation;
            if (cancelled)
            {
                SetEvent(cancellation_event);
            }
        }

        void detach_process(HANDLE value)
        {
            std::scoped_lock lock(mutex);
            if (process == value)
            {
                process = nullptr;
                process_job = nullptr;
                cancellation_event = nullptr;
            }
        }

        void cancel()
        {
            {
                std::scoped_lock lock(mutex);
                cancelled = true;
                if (cancellation_event != nullptr)
                {
                    SetEvent(cancellation_event);
                }
            }
            condition.notify_all();
        }

        [[nodiscard]] bool is_cancelled()
        {
            std::scoped_lock lock(mutex);
            return cancelled;
        }

        std::mutex mutex;
        std::condition_variable condition;
        glance::app::OfficePdfResult result;
        HANDLE process{};
        HANDLE process_job{};
        HANDLE cancellation_event{};
        bool complete{};
        bool cancelled{};
    };

    bool copy_source(
        const SourceFile& source,
        const std::filesystem::path& destination,
        const std::shared_ptr<ConversionJob>& job)
    {
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(source.handle.get(), beginning, nullptr, FILE_BEGIN))
        {
            return false;
        }
        UniqueHandle output(CreateFileW(
            destination.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!output)
        {
            return false;
        }

        std::vector<std::byte> buffer(copy_buffer_size);
        for (;;)
        {
            if (job->is_cancelled())
            {
                return false;
            }
            DWORD read{};
            if (!ReadFile(
                    source.handle.get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr))
            {
                return false;
            }
            if (read == 0)
            {
                FILE_STANDARD_INFO standard{};
                FILE_BASIC_INFO basic{};
                return FlushFileBuffers(output.get()) != FALSE &&
                    GetFileInformationByHandleEx(
                        source.handle.get(),
                        FileStandardInfo,
                        &standard,
                        sizeof(standard)) &&
                    GetFileInformationByHandleEx(
                        source.handle.get(),
                        FileBasicInfo,
                        &basic,
                        sizeof(basic)) &&
                    standard.EndOfFile.QuadPart == source.size &&
                    basic.LastWriteTime.QuadPart == source.last_write_time;
            }
            if (!write_exact(output.get(), buffer.data(), read))
            {
                return false;
            }
        }
    }

    bool run_office_host(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        const std::shared_ptr<ConversionJob>& job)
    {
        const auto host_path = std::filesystem::path(executable_directory()) / L"Glance.OfficeHost.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(host_path, error))
        {
            return false;
        }

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        UniqueHandle cancellation_event(CreateEventW(
            &security,
            TRUE,
            FALSE,
            nullptr));
        if (!cancellation_event)
        {
            return false;
        }

        std::wstring command_line = quote_argument(host_path.wstring()) + L" " +
            quote_argument(input.wstring()) + L" " +
            quote_argument(output.wstring()) + L" " +
            handle_argument(cancellation_event.get());
        SIZE_T attribute_bytes{};
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr,
            1,
            0,
            &attribute_bytes));
        std::vector<std::byte> attribute_storage(attribute_bytes);
        auto* attributes =
            reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
        {
            return false;
        }
        HANDLE inherited_handle = cancellation_event.get();
        if (!UpdateProcThreadAttribute(
                attributes,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &inherited_handle,
                sizeof(inherited_handle),
                nullptr,
                nullptr))
        {
            DeleteProcThreadAttributeList(attributes);
            return false;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
                host_path.c_str(),
                command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &process);
        DeleteProcThreadAttributeList(attributes);
        if (!created)
        {
            return false;
        }
        UniqueHandle process_handle(process.hProcess);
        UniqueHandle thread_handle(process.hThread);

        UniqueHandle process_job(CreateJobObjectW(nullptr, nullptr));
        if (process_job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(
                    process_job.get(),
                    JobObjectExtendedLimitInformation,
                    &limits,
                    sizeof(limits)) ||
                !AssignProcessToJobObject(process_job.get(), process_handle.get()))
            {
                process_job.reset();
            }
        }

        job->attach_process(
            process_handle.get(),
            process_job.get(),
            cancellation_event.get());
        if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1))
        {
            TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED);
        }
        thread_handle.reset();

        HANDLE wait_handles[]{ process_handle.get(), cancellation_event.get() };
        DWORD wait_result = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(wait_handles)),
            wait_handles,
            FALSE,
            conversion_timeout_ms);
        if (wait_result == WAIT_OBJECT_0 + 1 || wait_result == WAIT_TIMEOUT)
        {
            SetEvent(cancellation_event.get());
            wait_result = WaitForSingleObject(
                process_handle.get(),
                cooperative_cancel_timeout_ms);
        }
        if (wait_result != WAIT_OBJECT_0)
        {
            TerminateProcess(process_handle.get(), ERROR_TIMEOUT);
            WaitForSingleObject(process_handle.get(), 5000);
        }
        DWORD exit_code = ERROR_PROCESS_ABORTED;
        static_cast<void>(GetExitCodeProcess(process_handle.get(), &exit_code));
        job->detach_process(process_handle.get());
        process_job.reset();
        return wait_result == WAIT_OBJECT_0 && exit_code == 0 && !job->is_cancelled();
    }

    class OfficePdfService final
    {
    public:
        glance::app::OfficePdfResult prepare(const std::wstring& source_path)
        {
            auto source = open_source_file(source_path);
            if (!source.handle)
            {
                return { .status = glance::app::OfficePdfStatus::unavailable };
            }
            const auto root = cache_root();
            if (root.empty())
            {
                return { .status = glance::app::OfficePdfStatus::unavailable };
            }
            const auto final_path =
                root / (source.kind_name + L"-" + source.cache_key + L".pdf");
            if (valid_pdf(final_path))
            {
                return {
                    .status = glance::app::OfficePdfStatus::success,
                    .cache_key = std::move(source.cache_key),
                    .pdf_path = final_path.wstring(),
                };
            }

            std::shared_ptr<ConversionJob> job;
            bool owner{};
            {
                std::scoped_lock lock(mutex_);
                if (stopping_)
                {
                    return { .status = glance::app::OfficePdfStatus::cancelled };
                }
                const auto existing = jobs_.find(source.cache_key);
                if (existing != jobs_.end())
                {
                    job = existing->second;
                }
                else if (valid_pdf(final_path))
                {
                    return {
                        .status = glance::app::OfficePdfStatus::success,
                        .cache_key = std::move(source.cache_key),
                        .pdf_path = final_path.wstring(),
                    };
                }
                else
                {
                    job = std::make_shared<ConversionJob>();
                    jobs_.emplace(source.cache_key, job);
                    owner = true;
                }
            }
            if (!owner)
            {
                source.handle.reset();
                return job->wait();
            }

            const auto suffix = temporary_suffix();
            const auto staged_path =
                root / (L"staging-" + suffix + source.extension);
            const auto output_path =
                root / (L"output-" + suffix + L".pdf");
            glance::app::OfficePdfResult result{
                .status = glance::app::OfficePdfStatus::conversion_failed,
                .cache_key = source.cache_key,
            };

            const bool staged = copy_source(source, staged_path, job);
            source.handle.reset();
            if (staged && run_office_host(staged_path, output_path, job) && valid_pdf(output_path))
            {
                if (MoveFileExW(
                        output_path.c_str(),
                        final_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) &&
                    valid_pdf(final_path))
                {
                    result.status = glance::app::OfficePdfStatus::success;
                    result.pdf_path = final_path.wstring();
                }
            }
            if (job->is_cancelled())
            {
                result.status = glance::app::OfficePdfStatus::cancelled;
            }
            DeleteFileW(staged_path.c_str());
            DeleteFileW(output_path.c_str());

            job->finish(result);
            {
                std::scoped_lock lock(mutex_);
                const auto iterator = jobs_.find(source.cache_key);
                if (iterator != jobs_.end() && iterator->second == job)
                {
                    jobs_.erase(iterator);
                }
            }
            return result;
        }

        void shutdown() noexcept
        {
            std::vector<std::shared_ptr<ConversionJob>> jobs;
            {
                std::scoped_lock lock(mutex_);
                if (stopping_)
                {
                    return;
                }
                stopping_ = true;
                jobs.reserve(jobs_.size());
                for (const auto& [key, job] : jobs_)
                {
                    static_cast<void>(key);
                    jobs.push_back(job);
                }
                jobs_.clear();
            }
            for (const auto& job : jobs)
            {
                job->cancel();
            }
            for (const auto& job : jobs)
            {
                job->wait_until_complete();
            }
        }

    private:
        std::mutex mutex_;
        std::unordered_map<std::wstring, std::shared_ptr<ConversionJob>> jobs_;
        bool stopping_{};
    };

    OfficePdfService& service()
    {
        static OfficePdfService instance;
        return instance;
    }
}

namespace glance::app
{
    OfficePdfResult prepare_office_pdf(const std::wstring& source_path)
    {
        return service().prepare(source_path);
    }

    void shutdown_office_pdf_service() noexcept
    {
        service().shutdown();
    }
}
