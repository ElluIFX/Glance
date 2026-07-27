#include "pch.h"

#include "adobe_preview_service.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cwctype>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    using glance::components::adobe::PreviewResult;
    using namespace glance::contracts::components;

    constexpr std::uint16_t photoshop_thumbnail_4 = 1033;
    constexpr std::uint16_t photoshop_thumbnail_5 = 1036;
    constexpr std::uint64_t maximum_thumbnail_size = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t maximum_cache_size = 1ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr auto maximum_cache_age = std::chrono::hours(24 * 30);

    struct RefinementContext
    {
        std::filesystem::path source;
        std::wstring fingerprint;
    };

    struct LeaseRecord
    {
        std::filesystem::path directory;
        std::optional<RefinementContext> refinement;
        bool low_resolution_only{};
    };

    struct RefinementJob
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool complete{};
        bool success{};

        bool wait()
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return complete; });
            return success;
        }

        void finish(bool succeeded)
        {
            {
                std::scoped_lock lock(mutex);
                complete = true;
                success = succeeded;
            }
            changed.notify_all();
        }
    };

    std::atomic_bool shutting_down{};
    std::mutex lease_mutex;
    std::unordered_map<std::uint64_t, LeaseRecord> leases;
    std::atomic_uint64_t next_lease{ 1 };
    std::mutex process_mutex;
    std::unordered_set<HANDLE> active_processes;
    std::mutex jobs_mutex;
    std::unordered_map<std::wstring, std::shared_ptr<RefinementJob>> jobs;
    std::atomic_uint64_t temporary_sequence{};

    std::uint16_t read_be16(const std::array<unsigned char, 2>& bytes) noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
    }

    std::uint32_t read_be32(const std::array<unsigned char, 4>& bytes) noexcept
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24) |
            (static_cast<std::uint32_t>(bytes[1]) << 16) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) |
            static_cast<std::uint32_t>(bytes[3]);
    }

    bool read_exact(
        std::ifstream& input,
        void* destination,
        std::size_t size) noexcept
    {
        if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        {
            return false;
        }
        return static_cast<bool>(input.read(
            static_cast<char*>(destination),
            static_cast<std::streamsize>(size)));
    }

    bool skip(std::ifstream& input, std::uint64_t count, std::uint64_t limit) noexcept
    {
        const auto position = input.tellg();
        if (position < 0)
        {
            return false;
        }
        const auto offset = static_cast<std::uint64_t>(position);
        if (count > limit || offset > limit - count ||
            offset + count > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()))
        {
            return false;
        }
        input.seekg(static_cast<std::streamoff>(offset + count), std::ios::beg);
        return static_cast<bool>(input);
    }

    bool read_be32(std::ifstream& input, std::uint32_t& value) noexcept
    {
        std::array<unsigned char, 4> bytes{};
        if (!read_exact(input, bytes.data(), bytes.size()))
        {
            return false;
        }
        value = read_be32(bytes);
        return true;
    }

    template <typename T>
    void append_value(std::vector<std::byte>& output, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), begin, begin + sizeof(T));
    }

    void append_bytes(
        std::vector<std::byte>& output,
        const void* data,
        std::size_t size)
    {
        const auto* begin = static_cast<const std::byte*>(data);
        output.insert(output.end(), begin, begin + size);
    }

    std::wstring hash_metadata(const std::vector<std::byte>& metadata)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash_handle{};
        DWORD object_size{};
        DWORD hash_size{};
        DWORD copied{};
        if (BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0 ||
            BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &copied,
                0) < 0 ||
            BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size),
                sizeof(hash_size),
                &copied,
                0) < 0)
        {
            if (algorithm != nullptr)
            {
                BCryptCloseAlgorithmProvider(algorithm, 0);
            }
            return {};
        }

        std::vector<UCHAR> object(object_size);
        std::vector<UCHAR> hash(hash_size);
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
                reinterpret_cast<PUCHAR>(
                    const_cast<std::byte*>(metadata.data())),
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

        constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring result(hash.size() * 2, L'0');
        for (std::size_t index = 0; index < hash.size(); ++index)
        {
            result[index * 2] = digits[hash[index] >> 4];
            result[index * 2 + 1] = digits[hash[index] & 0x0F];
        }
        return result;
    }

    std::wstring source_fingerprint(const std::filesystem::path& path)
    {
        HANDLE file = CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        FILE_STANDARD_INFO standard{};
        FILE_BASIC_INFO basic{};
        const bool metadata_read =
            GetFileInformationByHandleEx(
                file,
                FileStandardInfo,
                &standard,
                sizeof(standard)) &&
            GetFileInformationByHandleEx(
                file,
                FileBasicInfo,
                &basic,
                sizeof(basic)) &&
            !standard.Directory;
        std::vector<std::byte> metadata;
        if (metadata_read)
        {
            constexpr std::uint32_t fingerprint_version = 1;
            append_value(metadata, fingerprint_version);
            append_value(metadata, standard.EndOfFile.QuadPart);
            append_value(metadata, basic.LastWriteTime.QuadPart);

            FILE_ID_INFO file_id{};
            const bool has_file_id = GetFileInformationByHandleEx(
                file,
                FileIdInfo,
                &file_id,
                sizeof(file_id));
            append_value(metadata, has_file_id);
            if (has_file_id)
            {
                append_value(metadata, file_id.VolumeSerialNumber);
                append_bytes(
                    metadata,
                    file_id.FileId.Identifier,
                    sizeof(file_id.FileId.Identifier));
            }
            else
            {
                auto normalized = std::filesystem::absolute(path).wstring();
                std::ranges::transform(
                    normalized,
                    normalized.begin(),
                    [](wchar_t value) {
                        return static_cast<wchar_t>(std::towlower(value));
                    });
                append_bytes(
                    metadata,
                    normalized.data(),
                    normalized.size() * sizeof(wchar_t));
            }
        }
        CloseHandle(file);
        return metadata_read ? hash_metadata(metadata) : std::wstring{};
    }

    bool locate_photoshop_thumbnail(
        const std::filesystem::path& path,
        std::uint64_t& data_offset,
        std::uint32_t& data_size) noexcept
    {
        try
        {
            const auto file_size = std::filesystem::file_size(path);
            if (file_size < 34)
            {
                return false;
            }

            std::ifstream input(path, std::ios::binary);
            std::array<unsigned char, 26> header{};
            if (!read_exact(input, header.data(), header.size()) ||
                !std::equal(header.begin(), header.begin() + 4, "8BPS") ||
                (header[4] != 0 || (header[5] != 1 && header[5] != 2)))
            {
                return false;
            }

            std::uint32_t color_data_size{};
            if (!read_be32(input, color_data_size) ||
                !skip(input, color_data_size, file_size))
            {
                return false;
            }

            std::uint32_t resources_size{};
            if (!read_be32(input, resources_size))
            {
                return false;
            }
            const auto resources_start = static_cast<std::uint64_t>(input.tellg());
            if (resources_start > file_size ||
                resources_size > file_size - resources_start)
            {
                return false;
            }
            const auto resources_end = resources_start + resources_size;

            while (static_cast<std::uint64_t>(input.tellg()) < resources_end)
            {
                std::array<unsigned char, 4> signature{};
                std::array<unsigned char, 2> identifier_bytes{};
                unsigned char name_size{};
                if (!read_exact(input, signature.data(), signature.size()) ||
                    !read_exact(input, identifier_bytes.data(), identifier_bytes.size()) ||
                    !read_exact(input, &name_size, 1) ||
                    !std::equal(signature.begin(), signature.end(), "8BIM"))
                {
                    return false;
                }

                const auto padded_name_size =
                    static_cast<std::uint64_t>((1U + name_size + 1U) & ~1U) - 1U;
                if (!skip(input, padded_name_size, resources_end))
                {
                    return false;
                }

                std::uint32_t resource_size{};
                if (!read_be32(input, resource_size))
                {
                    return false;
                }
                const auto resource_offset = static_cast<std::uint64_t>(input.tellg());
                if (resource_offset > resources_end ||
                    resource_size > resources_end - resource_offset)
                {
                    return false;
                }

                const auto identifier = read_be16(identifier_bytes);
                if ((identifier == photoshop_thumbnail_4 ||
                     identifier == photoshop_thumbnail_5) &&
                    resource_size >= 32)
                {
                    std::array<unsigned char, 28> thumbnail_header{};
                    if (!read_exact(
                            input,
                            thumbnail_header.data(),
                            thumbnail_header.size()))
                    {
                        return false;
                    }
                    const std::array<unsigned char, 4> format_bytes{
                        thumbnail_header[0],
                        thumbnail_header[1],
                        thumbnail_header[2],
                        thumbnail_header[3] };
                    const std::array<unsigned char, 4> compressed_size_bytes{
                        thumbnail_header[20],
                        thumbnail_header[21],
                        thumbnail_header[22],
                        thumbnail_header[23] };
                    const auto format = read_be32(format_bytes);
                    const auto compressed_size = read_be32(compressed_size_bytes);
                    if (format == 1 && compressed_size >= 4 &&
                        compressed_size <= resource_size - thumbnail_header.size() &&
                        compressed_size <= maximum_thumbnail_size)
                    {
                        std::array<unsigned char, 2> jpeg_signature{};
                        if (!read_exact(
                                input,
                                jpeg_signature.data(),
                                jpeg_signature.size()))
                        {
                            return false;
                        }
                        if (jpeg_signature[0] == 0xFF && jpeg_signature[1] == 0xD8)
                        {
                            data_offset = resource_offset + thumbnail_header.size();
                            data_size = compressed_size;
                            return true;
                        }
                    }
                }

                const auto padded_resource_size =
                    static_cast<std::uint64_t>(resource_size) + (resource_size & 1U);
                if (padded_resource_size > resources_end - resource_offset)
                {
                    return false;
                }
                input.seekg(
                    static_cast<std::streamoff>(resource_offset + padded_resource_size),
                    std::ios::beg);
                if (!input)
                {
                    return false;
                }
            }
        }
        catch (...)
        {
        }
        return false;
    }

    bool is_pdf_compatible_ai(const std::filesystem::path& path) noexcept
    {
        std::ifstream input(path, std::ios::binary);
        std::array<char, 5> signature{};
        return read_exact(input, signature.data(), signature.size()) &&
            std::equal(signature.begin(), signature.end(), "%PDF-");
    }

    bool has_photoshop_signature(
        const std::filesystem::path& path,
        std::uint16_t expected_version) noexcept
    {
        std::ifstream input(path, std::ios::binary);
        std::array<unsigned char, 6> signature{};
        return read_exact(input, signature.data(), signature.size()) &&
            std::equal(signature.begin(), signature.begin() + 4, "8BPS") &&
            signature[4] == 0 &&
            signature[5] == expected_version;
    }

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(
            reinterpret_cast<HMODULE>(&__ImageBase),
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::filesystem::path create_lease_directory(std::uint64_t token)
    {
        auto root = std::filesystem::temp_directory_path() /
            L"Glance" / L"AdobePreview";
        std::filesystem::create_directories(root);
        const auto directory = root / std::to_wstring(GetCurrentProcessId()) /
            std::to_wstring(token);
        std::filesystem::create_directories(directory);
        return directory;
    }

    std::filesystem::path cache_root()
    {
        std::error_code error;
        auto root = std::filesystem::temp_directory_path(error) /
            L"Glance" / L"AdobePreviewCache" / L"v1";
        if (error)
        {
            return {};
        }
        std::filesystem::create_directories(root, error);
        return error ? std::filesystem::path{} : root;
    }

    bool valid_png(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        constexpr std::array<unsigned char, 8> expected{
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        std::array<unsigned char, expected.size()> signature{};
        return read_exact(input, signature.data(), signature.size()) &&
            signature == expected;
    }

    void touch_cache_file(const std::filesystem::path& path) noexcept
    {
        std::error_code error;
        std::filesystem::last_write_time(
            path,
            std::filesystem::file_time_type::clock::now(),
            error);
    }

    void prune_cache() noexcept
    {
        try
        {
            const auto root = cache_root();
            if (root.empty())
            {
                return;
            }
            struct Entry
            {
                std::filesystem::path path;
                std::uint64_t size{};
                std::filesystem::file_time_type write_time;
            };
            std::vector<Entry> entries;
            std::uint64_t total_size{};
            const auto expired_before =
                std::filesystem::file_time_type::clock::now() - maximum_cache_age;
            std::error_code error;
            for (std::filesystem::directory_iterator iterator(root, error), end;
                 !error && iterator != end;
                 iterator.increment(error))
            {
                if (!iterator->is_regular_file(error) ||
                    _wcsicmp(iterator->path().extension().c_str(), L".png") != 0)
                {
                    continue;
                }
                const auto write_time = iterator->last_write_time(error);
                const auto size = iterator->file_size(error);
                if (error)
                {
                    error.clear();
                    continue;
                }
                if (write_time < expired_before)
                {
                    std::filesystem::remove(iterator->path(), error);
                    error.clear();
                    continue;
                }
                entries.push_back({ iterator->path(), size, write_time });
                total_size += size;
            }
            std::ranges::sort(entries, {}, &Entry::write_time);
            for (const auto& entry : entries)
            {
                if (total_size <= maximum_cache_size)
                {
                    break;
                }
                if (std::filesystem::remove(entry.path, error))
                {
                    total_size -= std::min(total_size, entry.size);
                }
                error.clear();
            }
        }
        catch (...)
        {
        }
    }

    std::filesystem::path cached_preview_path(
        const std::wstring& fingerprint,
        std::uint32_t maximum_dimension)
    {
        const auto root = cache_root();
        return root.empty()
            ? std::filesystem::path{}
            : root / (fingerprint + L"-" +
                std::to_wstring(maximum_dimension) + L".png");
    }

    bool copy_range(
        const std::filesystem::path& source,
        std::uint64_t offset,
        std::uint32_t size,
        const std::filesystem::path& destination)
    {
        std::ifstream input(source, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        std::array<char, 64 * 1024> buffer{};
        std::uint32_t remaining = size;
        while (remaining != 0)
        {
            const auto chunk = std::min<std::uint32_t>(
                remaining,
                static_cast<std::uint32_t>(buffer.size()));
            if (!read_exact(input, buffer.data(), chunk))
            {
                return false;
            }
            output.write(buffer.data(), chunk);
            if (!output)
            {
                return false;
            }
            remaining -= chunk;
        }
        output.flush();
        return static_cast<bool>(output);
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result{ L'"' };
        std::size_t backslashes{};
        for (const auto character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(character);
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    bool run_host(
        std::wstring_view kind,
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::uint32_t maximum_dimension) noexcept
    {
        try
        {
            const auto host = executable_directory() / L"Glance.AdobeHost.exe";
            if (!std::filesystem::is_regular_file(host))
            {
                return false;
            }
            auto command = quote_argument(host.wstring()) +
                L" --kind " + quote_argument(kind) +
                L" --input " + quote_argument(input.wstring()) +
                L" --output " + quote_argument(output.wstring()) +
                L" --maximum-dimension " +
                std::to_wstring(maximum_dimension);

            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(
                    host.c_str(),
                    command.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    host.parent_path().c_str(),
                    &startup,
                    &process))
            {
                return false;
            }
            CloseHandle(process.hThread);
            {
                std::scoped_lock lock(process_mutex);
                active_processes.insert(process.hProcess);
            }

            DWORD wait_result{};
            do
            {
                wait_result = WaitForSingleObject(process.hProcess, 100);
            } while (wait_result == WAIT_TIMEOUT && !shutting_down.load());
            if (wait_result == WAIT_TIMEOUT)
            {
                TerminateProcess(process.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
            }

            DWORD exit_code = ERROR_GEN_FAILURE;
            const auto success =
                wait_result == WAIT_OBJECT_0 &&
                GetExitCodeProcess(process.hProcess, &exit_code) &&
                exit_code == ERROR_SUCCESS &&
                std::filesystem::is_regular_file(output);
            {
                std::scoped_lock lock(process_mutex);
                active_processes.erase(process.hProcess);
            }
            CloseHandle(process.hProcess);
            return success;
        }
        catch (...)
        {
            return false;
        }
    }

    PreviewResult create_embedded_preview(
        const std::filesystem::path& source,
        std::uint64_t offset,
        std::uint32_t size,
        std::wstring fingerprint,
        bool low_resolution_only)
    {
        const auto token = next_lease.fetch_add(1);
        const auto directory = create_lease_directory(token);
        const auto output = directory / L"preview.jpg";
        if (!copy_range(source, offset, size, output))
        {
            std::filesystem::remove_all(directory);
            return {};
        }
        {
            std::scoped_lock lock(lease_mutex);
            leases.emplace(
                token,
                LeaseRecord{
                    .directory = directory,
                    .refinement = low_resolution_only
                        ? std::nullopt
                        : std::optional<RefinementContext>{
                            RefinementContext{
                                .source = source,
                                .fingerprint = std::move(fingerprint) } },
                    .low_resolution_only = low_resolution_only });
        }
        return {
            .status = PrepareStatus::success,
            .kind = PreviewContentKind::image,
            .format = PreviewContentFormat::image_file,
            .path = output,
            .lease_token = token };
    }

    std::uint32_t normalize_dimension(std::uint32_t value) noexcept
    {
        switch (value)
        {
        case 1024:
        case 2048:
        case 4096:
        case 8192:
            return value;
        default:
            return 4096;
        }
    }

    PreviewResult cached_preview_result(const std::filesystem::path& path)
    {
        touch_cache_file(path);
        return {
            .status = PrepareStatus::success,
            .kind = PreviewContentKind::image,
            .format = PreviewContentFormat::image_file,
            .path = path };
    }

    PreviewResult generate_cached_preview(
        const std::filesystem::path& source,
        const std::wstring& fingerprint,
        std::uint32_t maximum_dimension)
    {
        const auto final_path =
            cached_preview_path(fingerprint, maximum_dimension);
        if (final_path.empty())
        {
            return { .status = PrepareStatus::failed };
        }
        if (valid_png(final_path))
        {
            return cached_preview_result(final_path);
        }

        const auto job_key = final_path.filename().wstring();
        std::shared_ptr<RefinementJob> job;
        bool owner{};
        {
            std::scoped_lock lock(jobs_mutex);
            const auto existing = jobs.find(job_key);
            if (existing != jobs.end())
            {
                job = existing->second;
            }
            else if (valid_png(final_path))
            {
                return cached_preview_result(final_path);
            }
            else
            {
                job = std::make_shared<RefinementJob>();
                jobs.emplace(job_key, job);
                owner = true;
            }
        }
        if (!owner)
        {
            return job->wait() && valid_png(final_path)
                ? cached_preview_result(final_path)
                : PreviewResult{ .status = PrepareStatus::failed };
        }

        const auto sequence = temporary_sequence.fetch_add(1);
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(sequence);
        const auto root = final_path.parent_path();
        const auto staged_path =
            root / (L"staging-" + suffix + source.extension().wstring());
        const auto output_path = root / (L"output-" + suffix + L".png");
        bool success{};
        try
        {
            std::filesystem::copy_file(
                source,
                staged_path,
                std::filesystem::copy_options::overwrite_existing);
            const bool source_unchanged =
                source_fingerprint(source) == fingerprint;
            const auto extension = source.extension().wstring();
            if (source_unchanged &&
                _wcsicmp(extension.c_str(), L".psd") == 0)
            {
                success = run_host(
                    L"psd",
                    staged_path,
                    output_path,
                    maximum_dimension);
            }
            if (success && valid_png(output_path))
            {
                success = MoveFileExW(
                    output_path.c_str(),
                    final_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
            else
            {
                success = false;
            }
        }
        catch (...)
        {
            success = false;
        }

        std::error_code cleanup_error;
        std::filesystem::remove(staged_path, cleanup_error);
        std::filesystem::remove(output_path, cleanup_error);
        job->finish(success);
        {
            std::scoped_lock lock(jobs_mutex);
            const auto iterator = jobs.find(job_key);
            if (iterator != jobs.end() && iterator->second == job)
            {
                jobs.erase(iterator);
            }
        }
        return success && valid_png(final_path)
            ? cached_preview_result(final_path)
            : PreviewResult{
                .status = shutting_down.load()
                    ? PrepareStatus::cancelled
                    : PrepareStatus::failed };
    }
}

namespace glance::components::adobe
{
    void initialize() noexcept
    {
        shutting_down.store(false);
        prune_cache();
    }

    bool can_preview(const std::filesystem::path& path) noexcept
    {
        try
        {
            const auto extension = path.extension().wstring();
            if (_wcsicmp(extension.c_str(), L".ai") == 0)
            {
                return is_pdf_compatible_ai(path);
            }
            if (_wcsicmp(extension.c_str(), L".psd") != 0 &&
                _wcsicmp(extension.c_str(), L".psb") != 0)
            {
                return false;
            }
            if (_wcsicmp(extension.c_str(), L".psd") == 0)
            {
                return has_photoshop_signature(path, 1);
            }
            std::uint64_t offset{};
            std::uint32_t size{};
            return has_photoshop_signature(path, 2) &&
                locate_photoshop_thumbnail(path, offset, size);
        }
        catch (...)
        {
            return false;
        }
    }

    PreviewResult prepare_preview(
        const std::filesystem::path& path,
        std::uint32_t maximum_dimension) noexcept
    {
        try
        {
            maximum_dimension = normalize_dimension(maximum_dimension);
            const auto extension = path.extension().wstring();
            if (_wcsicmp(extension.c_str(), L".ai") == 0)
            {
                if (is_pdf_compatible_ai(path))
                {
                    return {
                        .status = PrepareStatus::success,
                        .kind = PreviewContentKind::document,
                        .format = PreviewContentFormat::pdf,
                        .path = path };
                }
                return { .status = PrepareStatus::unavailable };
            }
            if (_wcsicmp(extension.c_str(), L".psb") == 0)
            {
                std::uint64_t offset{};
                std::uint32_t size{};
                if (!has_photoshop_signature(path, 2) ||
                    !locate_photoshop_thumbnail(path, offset, size))
                {
                    return { .status = PrepareStatus::unavailable };
                }
                return create_embedded_preview(
                    path,
                    offset,
                    size,
                    {},
                    true);
            }
            if (_wcsicmp(extension.c_str(), L".psd") != 0)
            {
                return { .status = PrepareStatus::unavailable };
            }

            const auto fingerprint = source_fingerprint(path);
            if (fingerprint.empty())
            {
                return { .status = PrepareStatus::failed };
            }
            const auto cached =
                cached_preview_path(fingerprint, maximum_dimension);
            if (!cached.empty() && valid_png(cached))
            {
                return cached_preview_result(cached);
            }

            std::uint64_t offset{};
            std::uint32_t size{};
            if (locate_photoshop_thumbnail(path, offset, size))
            {
                return create_embedded_preview(
                    path,
                    offset,
                    size,
                    fingerprint,
                    false);
            }
            return generate_cached_preview(
                path,
                fingerprint,
                maximum_dimension);
        }
        catch (...)
        {
            return { .status = PrepareStatus::failed };
        }
    }

    bool can_refine(std::uint64_t lease_token) noexcept
    {
        if (lease_token == 0)
        {
            return false;
        }
        std::scoped_lock lock(lease_mutex);
        const auto iterator = leases.find(lease_token);
        return iterator != leases.end() &&
            iterator->second.refinement.has_value();
    }

    bool is_low_resolution_only(std::uint64_t lease_token) noexcept
    {
        if (lease_token == 0)
        {
            return false;
        }
        std::scoped_lock lock(lease_mutex);
        const auto iterator = leases.find(lease_token);
        return iterator != leases.end() &&
            iterator->second.low_resolution_only;
    }

    PreviewResult prepare_refined_preview(
        std::uint64_t lease_token,
        std::uint32_t maximum_dimension) noexcept
    {
        try
        {
            RefinementContext context;
            {
                std::scoped_lock lock(lease_mutex);
                const auto iterator = leases.find(lease_token);
                if (iterator == leases.end() ||
                    !iterator->second.refinement.has_value())
                {
                    return { .status = PrepareStatus::failed };
                }
                context = *iterator->second.refinement;
            }
            if (source_fingerprint(context.source) != context.fingerprint)
            {
                return { .status = PrepareStatus::failed };
            }
            return generate_cached_preview(
                context.source,
                context.fingerprint,
                normalize_dimension(maximum_dimension));
        }
        catch (...)
        {
            return { .status = PrepareStatus::failed };
        }
    }

    void release_preview(std::uint64_t lease_token) noexcept
    {
        if (lease_token == 0)
        {
            return;
        }
        try
        {
            LeaseRecord record;
            {
                std::scoped_lock lock(lease_mutex);
                const auto iterator = leases.find(lease_token);
                if (iterator == leases.end())
                {
                    return;
                }
                record = std::move(iterator->second);
                leases.erase(iterator);
            }
            std::filesystem::remove_all(record.directory);
        }
        catch (...)
        {
        }
    }

    void shutdown() noexcept
    {
        shutting_down.store(true);
        {
            std::scoped_lock lock(process_mutex);
            for (const auto process : active_processes)
            {
                TerminateProcess(process, ERROR_CANCELLED);
            }
        }
        std::vector<std::filesystem::path> directories;
        {
            std::scoped_lock lock(lease_mutex);
            for (auto& [token, record] : leases)
            {
                static_cast<void>(token);
                directories.push_back(std::move(record.directory));
            }
            leases.clear();
        }
        for (const auto& directory : directories)
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
}
