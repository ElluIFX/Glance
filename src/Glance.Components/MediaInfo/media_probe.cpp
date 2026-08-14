#include "pch.h"
#include "media_probe.h"

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr std::size_t maximum_probe_output_bytes = 2 * 1024 * 1024;
    constexpr std::size_t maximum_tag_value_characters = 512;
    constexpr std::uint32_t maximum_stream_count = 64;
    constexpr std::size_t maximum_tag_count = 128;
    constexpr wchar_t ffprobe_sha256[] =
        L"b49ccc7c6547b141ad5a2f6ec69cc04323d7133d7704d70b331b904c63eecb07";
    constexpr wchar_t ffprobe_archive_member[] =
        L"ffmpeg-8.1.2-essentials_build/bin/ffprobe.exe";

    class Handle
    {
    public:
        explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value)
        {
        }

        ~Handle()
        {
            reset();
        }

        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        Handle(Handle&& other) noexcept : value_(other.release())
        {
        }

        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        {
            if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr)
            {
                CloseHandle(value_);
            }
            value_ = value;
        }

        [[nodiscard]] HANDLE release() noexcept
        {
            const HANDLE value = value_;
            value_ = INVALID_HANDLE_VALUE;
            return value;
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
        }

    private:
        HANDLE value_{ INVALID_HANDLE_VALUE };
    };

    struct BCryptState
    {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};

        ~BCryptState()
        {
            if (hash != nullptr)
            {
                BCryptDestroyHash(hash);
            }
            if (algorithm != nullptr)
            {
                BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        }
    };

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result{ L'"' };
        std::size_t backslashes{};
        for (const wchar_t character : value)
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

    bool sink_cancelled(
        const glance::contracts::components::HoverInfoTextSink* sink) noexcept
    {
        return sink != nullptr && sink->is_cancelled != nullptr &&
            sink->is_cancelled(sink->context) != FALSE;
    }

    struct ProcessOutput
    {
        std::string text;
        bool succeeded{};
        bool cancelled{};
    };

    ProcessOutput run_process_capture(
        const std::filesystem::path& executable,
        std::wstring arguments,
        DWORD timeout_ms,
        const glance::contracts::components::HoverInfoTextSink* sink)
    {
        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        HANDLE raw_read{};
        HANDLE raw_write{};
        if (!CreatePipe(&raw_read, &raw_write, &security, 0))
        {
            return {};
        }
        Handle read_pipe(raw_read);
        Handle write_pipe(raw_write);
        if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0))
        {
            return {};
        }

        std::wstring command_line = quote_argument(executable.wstring());
        if (!arguments.empty())
        {
            command_line.push_back(L' ');
            command_line.append(arguments);
        }
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write_pipe.get();
        startup.hStdError = write_pipe.get();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process))
        {
            return {};
        }
        Handle process_handle(process.hProcess);
        Handle thread_handle(process.hThread);
        write_pipe.reset();

        ProcessOutput result;
        std::thread reader([handle = read_pipe.release(), &result] {
            Handle pipe(handle);
            std::array<char, 8192> buffer{};
            DWORD read{};
            while (ReadFile(
                       pipe.get(),
                       buffer.data(),
                       static_cast<DWORD>(buffer.size()),
                       &read,
                       nullptr) &&
                   read != 0)
            {
                const std::size_t remaining =
                    maximum_probe_output_bytes - result.text.size();
                result.text.append(buffer.data(), std::min<std::size_t>(read, remaining));
            }
        });

        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (true)
        {
            const DWORD wait = WaitForSingleObject(process_handle.get(), 50);
            if (wait == WAIT_OBJECT_0)
            {
                break;
            }
            if (wait == WAIT_FAILED || sink_cancelled(sink) || GetTickCount64() >= deadline)
            {
                result.cancelled = sink_cancelled(sink);
                static_cast<void>(TerminateProcess(
                    process_handle.get(),
                    result.cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT));
                static_cast<void>(WaitForSingleObject(process_handle.get(), 1000));
                break;
            }
        }
        reader.join();
        DWORD exit_code = ERROR_GEN_FAILURE;
        result.succeeded = !result.cancelled &&
            GetExitCodeProcess(process_handle.get(), &exit_code) && exit_code == 0;
        return result;
    }

    std::filesystem::path local_ffprobe_path()
    {
        PWSTR raw_path{};
        if (FAILED(SHGetKnownFolderPath(
                FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path)))
        {
            return {};
        }
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(
            raw_path,
            CoTaskMemFree);
        return std::filesystem::path(path.get()) / L"Glance" / L"Components" /
            L"media-info" / L"bin" / L"ffprobe.exe";
    }

    std::filesystem::path path_ffprobe()
    {
        const DWORD path_length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (path_length == 0)
        {
            return {};
        }
        std::wstring search_path(path_length, L'\0');
        if (GetEnvironmentVariableW(L"PATH", search_path.data(), path_length) == 0)
        {
            return {};
        }
        while (!search_path.empty() && search_path.back() == L'\0')
        {
            search_path.pop_back();
        }
        std::wstring executable(32768, L'\0');
        const DWORD length = SearchPathW(
            search_path.c_str(),
            L"ffprobe.exe",
            nullptr,
            static_cast<DWORD>(executable.size()),
            executable.data(),
            nullptr);
        if (length == 0 || length >= executable.size())
        {
            return {};
        }
        executable.resize(length);
        return executable;
    }

    std::wstring json_string(
        const winrt::Windows::Data::Json::JsonObject& object,
        wchar_t const* name)
    {
        if (!object.HasKey(name))
        {
            return {};
        }
        const auto value = object.GetNamedValue(name);
        if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
        {
            return value.GetString().c_str();
        }
        if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
        {
            std::wostringstream output;
            output << std::setprecision(15) << value.GetNumber();
            return output.str();
        }
        return {};
    }

    std::uint64_t unsigned_value(std::wstring_view value)
    {
        try
        {
            return std::stoull(std::wstring(value));
        }
        catch (...)
        {
            return 0;
        }
    }

    double decimal_value(std::wstring_view value)
    {
        try
        {
            return std::stod(std::wstring(value));
        }
        catch (...)
        {
            return 0.0;
        }
    }

    std::wstring format_duration(std::wstring_view raw)
    {
        const double seconds = decimal_value(raw);
        if (seconds <= 0.0)
        {
            return {};
        }
        const auto milliseconds =
            static_cast<std::uint64_t>(std::round(seconds * 1000.0));
        const auto hours = milliseconds / 3600000;
        const auto minutes = milliseconds / 60000 % 60;
        const auto whole_seconds = milliseconds / 1000 % 60;
        const auto remainder = milliseconds % 1000;
        std::wostringstream output;
        if (hours != 0)
        {
            output << hours << L':' << std::setfill(L'0') << std::setw(2) << minutes;
        }
        else
        {
            output << minutes;
        }
        output << L':' << std::setfill(L'0') << std::setw(2) << whole_seconds;
        if (remainder != 0)
        {
            output << L'.' << std::setw(3) << remainder;
        }
        return output.str();
    }

    std::wstring format_rate(std::uint64_t bitrate)
    {
        if (bitrate == 0)
        {
            return {};
        }
        std::wostringstream output;
        if (bitrate >= 1000000)
        {
            output << std::fixed << std::setprecision(1)
                   << bitrate / 1000000.0 << L"Mbps";
        }
        else
        {
            output << (bitrate + 500) / 1000 << L"kbps";
        }
        return output.str();
    }

    std::wstring format_frame_rate(std::wstring_view raw)
    {
        const auto separator = raw.find(L'/');
        const double numerator = decimal_value(raw.substr(0, separator));
        const double denominator = separator == std::wstring_view::npos
            ? 1.0
            : decimal_value(raw.substr(separator + 1));
        const double rate = denominator == 0.0 ? 0.0 : numerator / denominator;
        if (rate <= 0.0)
        {
            return {};
        }
        std::wostringstream output;
        output << std::fixed
               << std::setprecision(std::abs(rate - std::round(rate)) < 0.01 ? 0 : 3)
               << rate << L"fps";
        return output.str();
    }

    std::wstring friendly_codec(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (value == L"h264") return L"H.264";
        if (value == L"hevc" || value == L"h265") return L"HEVC";
        if (value == L"av1") return L"AV1";
        if (value == L"vp9") return L"VP9";
        if (value == L"aac") return L"AAC";
        if (value == L"mp3") return L"MP3";
        if (value == L"flac") return L"FLAC";
        if (value == L"opus") return L"Opus";
        if (value.starts_with(L"pcm_")) return L"PCM";
        std::ranges::transform(value, value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towupper(character));
        });
        return value;
    }

    std::wstring join_values(
        std::initializer_list<std::wstring_view> values,
        std::wstring_view separator = L" / ")
    {
        std::wstring result;
        for (const auto value : values)
        {
            if (value.empty() || value == L"unknown" || value == L"N/A")
            {
                continue;
            }
            if (!result.empty())
            {
                result.append(separator);
            }
            result.append(value);
        }
        return result;
    }

    std::wstring format_pattern(
        std::wstring pattern,
        std::initializer_list<std::wstring_view> values)
    {
        std::size_t index{};
        for (const auto value : values)
        {
            const std::wstring token = L"{" + std::to_wstring(index++) + L"}";
            if (const auto position = pattern.find(token); position != std::wstring::npos)
            {
                pattern.replace(position, token.size(), value);
            }
        }
        return pattern;
    }

    void append_line(
        std::wstring& output,
        std::wstring_view label,
        std::wstring_view value)
    {
        if (value.empty())
        {
            return;
        }
        if (!output.empty())
        {
            output.push_back(L'\n');
        }
        output.append(label).append(L": ").append(value);
    }

    std::wstring stream_type_label(
        std::wstring_view type,
        const wchar_t* language_tag)
    {
        const wchar_t* key = L"MediaType.Data";
        if (type == L"video") key = L"MediaType.Video";
        else if (type == L"audio") key = L"MediaType.Audio";
        else if (type == L"subtitle") key = L"MediaType.Subtitle";
        else if (type == L"attachment") key = L"MediaType.Attachment";
        return glance::components::media_info::localize_text(key, language_tag);
    }

    std::wstring compact_tag_value(std::wstring value)
    {
        for (auto& character : value)
        {
            if (character == L'\r' || character == L'\n' || character == L'\t')
            {
                character = L' ';
            }
        }
        if (value.size() > maximum_tag_value_characters)
        {
            value.resize(maximum_tag_value_characters);
            value.append(L"...");
        }
        return value;
    }

    std::wstring display_tag_name(std::wstring value)
    {
        bool capitalize = true;
        for (auto& character : value)
        {
            if (character == L'_' || character == L'-')
            {
                character = L' ';
                capitalize = true;
            }
            else if (capitalize)
            {
                character = static_cast<wchar_t>(std::towupper(character));
                capitalize = false;
            }
            else
            {
                character = static_cast<wchar_t>(std::towlower(character));
            }
        }
        return value;
    }

    void append_tags(
        const winrt::Windows::Data::Json::JsonObject& object,
        std::vector<std::pair<std::wstring, std::wstring>>& destination)
    {
        const auto tags = object.GetNamedObject(L"tags", nullptr);
        if (tags == nullptr)
        {
            return;
        }
        std::unordered_set<std::wstring> seen;
        for (const auto& entry : tags)
        {
            if (destination.size() >= maximum_tag_count)
            {
                break;
            }
            if (entry.Value().ValueType() !=
                winrt::Windows::Data::Json::JsonValueType::String)
            {
                continue;
            }
            std::wstring name(entry.Key());
            std::wstring normalized(name);
            std::ranges::transform(normalized, normalized.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (normalized == L"major_brand" || normalized == L"minor_version" ||
                normalized == L"compatible_brands" || normalized == L"duration" ||
                normalized.starts_with(L"number_of_") ||
                normalized.starts_with(L"_statistics_"))
            {
                continue;
            }
            auto value = compact_tag_value(entry.Value().GetString().c_str());
            if (!value.empty() && seen.insert(normalized + L"\n" + value).second)
            {
                destination.emplace_back(display_tag_name(std::move(name)), std::move(value));
            }
        }
        std::ranges::sort(destination, {}, &std::pair<std::wstring, std::wstring>::first);
    }

    std::wstring format_probe_json(
        std::string_view json,
        const wchar_t* language_tag)
    {
        using winrt::Windows::Data::Json::JsonObject;
        const auto localize = [language_tag](std::wstring_view key) {
            return glance::components::media_info::localize_text(key, language_tag);
        };
        const auto root = JsonObject::Parse(winrt::to_hstring(json));
        std::wstring result;
        const auto format = root.GetNamedObject(L"format", nullptr);
        std::vector<std::pair<std::wstring, std::wstring>> tags;
        if (format != nullptr)
        {
            auto container = json_string(format, L"format_long_name");
            if (container.empty())
            {
                container = json_string(format, L"format_name");
            }
            append_line(result, localize(L"MediaInfo.Container"), container);
            append_line(
                result,
                localize(L"MediaInfo.Duration"),
                format_duration(json_string(format, L"duration")));
            append_line(
                result,
                localize(L"MediaInfo.OverallBitrate"),
                format_rate(unsigned_value(json_string(format, L"bit_rate"))));
            append_tags(format, tags);
        }
        if (const auto chapters = root.GetNamedArray(L"chapters", nullptr);
            chapters != nullptr && chapters.Size() != 0)
        {
            append_line(
                result,
                localize(L"MediaInfo.Chapters"),
                std::to_wstring(chapters.Size()));
        }
        if (!result.empty())
        {
            result.insert(0, localize(L"MediaInfo.General") + L"\n");
        }

        const auto streams = root.GetNamedArray(L"streams", nullptr);
        if (streams != nullptr)
        {
            const auto count = std::min(streams.Size(), maximum_stream_count);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const auto stream = streams.GetObjectAt(index);
                if (!result.empty())
                {
                    result.append(L"\n\n");
                }
                const auto type = json_string(stream, L"codec_type");
                result.append(format_pattern(
                    localize(L"MediaInfo.StreamHeading"),
                    { stream_type_label(type, language_tag), std::to_wstring(index + 1) }));

                auto codec = friendly_codec(json_string(stream, L"codec_name"));
                const auto profile = json_string(stream, L"profile");
                const auto description = json_string(stream, L"codec_long_name");
                if (!profile.empty())
                {
                    codec += codec.empty() ? profile : L" / " + profile;
                }
                if (!description.empty() && _wcsicmp(description.c_str(), codec.c_str()) != 0)
                {
                    codec += codec.empty() ? description : L" (" + description + L")";
                }
                append_line(result, localize(L"MediaInfo.Codec"), codec);

                const auto width = json_string(stream, L"width");
                const auto height = json_string(stream, L"height");
                if (!width.empty() && !height.empty())
                {
                    append_line(
                        result,
                        localize(L"MediaInfo.Resolution"),
                        width + L"x" + height);
                }
                auto frame_rate = format_frame_rate(json_string(stream, L"avg_frame_rate"));
                if (frame_rate.empty())
                {
                    frame_rate = format_frame_rate(json_string(stream, L"r_frame_rate"));
                }
                append_line(result, localize(L"MediaInfo.FrameRate"), frame_rate);
                append_line(
                    result,
                    localize(L"MediaInfo.PixelFormat"),
                    json_string(stream, L"pix_fmt"));
                auto bit_depth = json_string(stream, L"bits_per_raw_sample");
                if (bit_depth.empty() || bit_depth == L"0")
                {
                    bit_depth = json_string(stream, L"bits_per_sample");
                }
                append_line(result, localize(L"MediaInfo.BitDepth"), bit_depth);
                append_line(
                    result,
                    localize(L"MediaInfo.Color"),
                    join_values({
                        json_string(stream, L"color_primaries"),
                        json_string(stream, L"color_transfer"),
                        json_string(stream, L"color_space"),
                        json_string(stream, L"color_range") }));
                append_line(
                    result,
                    localize(L"MediaInfo.AspectRatio"),
                    join_values({
                        json_string(stream, L"sample_aspect_ratio"),
                        json_string(stream, L"display_aspect_ratio") }));
                if (const auto side_data = stream.GetNamedArray(L"side_data_list", nullptr))
                {
                    for (const auto& value : side_data)
                    {
                        const auto rotation = json_string(value.GetObjectW(), L"rotation");
                        if (!rotation.empty())
                        {
                            append_line(
                                result,
                                localize(L"MediaInfo.Rotation"),
                                rotation + L"\u00b0");
                            break;
                        }
                    }
                }

                const auto sample_rate = json_string(stream, L"sample_rate");
                if (!sample_rate.empty())
                {
                    const auto numeric_rate = unsigned_value(sample_rate);
                    append_line(
                        result,
                        localize(L"MediaInfo.SampleRate"),
                        numeric_rate == 0 ? sample_rate : std::to_wstring(numeric_rate) + L"Hz");
                }
                append_line(
                    result,
                    localize(L"MediaInfo.SampleFormat"),
                    json_string(stream, L"sample_fmt"));
                append_line(
                    result,
                    localize(L"MediaInfo.Channels"),
                    join_values({
                        json_string(stream, L"channels"),
                        json_string(stream, L"channel_layout") }));
                append_line(
                    result,
                    localize(L"MediaInfo.Bitrate"),
                    format_rate(unsigned_value(json_string(stream, L"bit_rate"))));
                append_line(
                    result,
                    localize(L"MediaInfo.Duration"),
                    format_duration(json_string(stream, L"duration")));
                append_line(
                    result,
                    localize(L"MediaInfo.Frames"),
                    json_string(stream, L"nb_frames"));

                if (const auto stream_tags = stream.GetNamedObject(L"tags", nullptr))
                {
                    append_line(
                        result,
                        localize(L"MediaInfo.Language"),
                        json_string(stream_tags, L"language"));
                    append_line(
                        result,
                        localize(L"MediaInfo.Title"),
                        json_string(stream_tags, L"title"));
                }
                if (const auto disposition = stream.GetNamedObject(L"disposition", nullptr))
                {
                    std::wstring disposition_text;
                    if (disposition.GetNamedNumber(L"default", 0) != 0)
                    {
                        disposition_text = localize(L"MediaInfo.Default");
                    }
                    if (disposition.GetNamedNumber(L"forced", 0) != 0)
                    {
                        if (!disposition_text.empty()) disposition_text.append(L", ");
                        disposition_text.append(localize(L"MediaInfo.Forced"));
                    }
                    append_line(
                        result,
                        localize(L"MediaInfo.Disposition"),
                        disposition_text);
                }
            }
        }

        if (!tags.empty())
        {
            if (!result.empty())
            {
                result.append(L"\n\n");
            }
            result.append(localize(L"MediaInfo.Metadata"));
            for (const auto& [name, value] : tags)
            {
                append_line(result, name, value);
            }
        }
        return result;
    }

    std::optional<std::wstring> sha256_file(const std::filesystem::path& path)
    {
        Handle file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!file)
        {
            return std::nullopt;
        }

        BCryptState state;
        std::vector<std::uint8_t> object;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            return std::nullopt;
        }
        DWORD object_size{};
        DWORD result_size{};
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                state.algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &result_size,
                0)))
        {
            return std::nullopt;
        }
        object.resize(object_size);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                state.algorithm,
                &state.hash,
                object.data(),
                static_cast<ULONG>(object.size()),
                nullptr,
                0,
                0)))
        {
            return std::nullopt;
        }
        std::array<std::uint8_t, 256 * 1024> buffer{};
        while (true)
        {
            DWORD read{};
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                return std::nullopt;
            }
            if (read == 0)
            {
                break;
            }
            if (!BCRYPT_SUCCESS(BCryptHashData(state.hash, buffer.data(), read, 0)))
            {
                return std::nullopt;
            }
        }
        std::array<std::uint8_t, 32> digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
        {
            return std::nullopt;
        }
        BCryptDestroyHash(state.hash);
        state.hash = nullptr;
        constexpr wchar_t hexadecimal[] = L"0123456789abcdef";
        std::wstring value;
        value.reserve(64);
        for (const auto byte : digest)
        {
            value.push_back(hexadecimal[byte >> 4]);
            value.push_back(hexadecimal[byte & 0x0f]);
        }
        return value;
    }

    bool run_tar(
        const std::filesystem::path& tar,
        const std::filesystem::path& archive,
        const std::filesystem::path& staging)
    {
        std::wstring command_line = quote_argument(tar.wstring()) +
            L" -xf " + quote_argument(archive.wstring()) +
            L" -C " + quote_argument(staging.wstring()) +
            L" " + quote_argument(ffprobe_archive_member);
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                tar.c_str(),
                command_line.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process))
        {
            return false;
        }
        Handle process_handle(process.hProcess);
        Handle thread_handle(process.hThread);
        if (WaitForSingleObject(process_handle.get(), 30000) != WAIT_OBJECT_0)
        {
            static_cast<void>(TerminateProcess(process_handle.get(), ERROR_TIMEOUT));
            return false;
        }
        DWORD exit_code = ERROR_GEN_FAILURE;
        return GetExitCodeProcess(process_handle.get(), &exit_code) && exit_code == 0;
    }
}

namespace glance::components::media_info
{
    std::filesystem::path find_ffprobe() noexcept
    {
        try
        {
            for (const auto& candidate : { path_ffprobe(), local_ffprobe_path() })
            {
                if (!candidate.empty() && validate_ffprobe(candidate))
                {
                    return candidate;
                }
            }
        }
        catch (...)
        {
        }
        return {};
    }

    bool validate_ffprobe(const std::filesystem::path& path) noexcept
    {
        try
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error) &&
                run_process_capture(path, L"-version", 1500, nullptr).succeeded;
        }
        catch (...)
        {
            return false;
        }
    }

    std::wstring query_media_info(
        const std::filesystem::path& ffprobe,
        std::wstring_view path,
        const wchar_t* language_tag,
        const glance::contracts::components::HoverInfoTextSink& sink) noexcept
    {
        try
        {
            constexpr std::wstring_view entries =
                L"format=format_name,format_long_name,duration,bit_rate:"
                L"format_tags:"
                L"stream=index,codec_name,codec_long_name,codec_type,profile,level,width,height,"
                L"sample_aspect_ratio,display_aspect_ratio,pix_fmt,color_range,color_space,"
                L"color_transfer,color_primaries,chroma_location,field_order,avg_frame_rate,"
                L"r_frame_rate,bit_rate,bits_per_sample,bits_per_raw_sample,sample_fmt,"
                L"sample_rate,channels,channel_layout,duration,nb_frames:"
                L"stream_tags=language,title:"
                L"stream_disposition=default,forced:"
                L"stream_side_data=rotation:"
                L"chapter=id,start_time,end_time:"
                L"chapter_tags=title";
            const auto output = run_process_capture(
                ffprobe,
                L"-v error -show_entries " + std::wstring(entries) +
                    L" -of json " + quote_argument(path),
                10000,
                &sink);
            if (output.cancelled)
            {
                return {};
            }
            if (!output.succeeded || output.text.empty())
            {
                return localize_text(L"Preview.Failed", language_tag);
            }
            auto result = format_probe_json(output.text, language_tag);
            return result.empty()
                ? localize_text(L"Preview.Failed", language_tag)
                : result;
        }
        catch (...)
        {
            return localize_text(L"Preview.Failed", language_tag);
        }
    }

    bool install_ffprobe(
        const std::filesystem::path& archive,
        const std::filesystem::path& storage,
        std::wstring& error_key) noexcept
    {
        std::filesystem::path staging;
        try
        {
            wchar_t system_directory[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(system_directory, std::size(system_directory));
            if (length == 0 || length >= std::size(system_directory))
            {
                error_key = L"Action.ExtractFailed";
                return false;
            }
            const auto tar = std::filesystem::path(system_directory) / L"tar.exe";
            std::error_code error;
            if (!std::filesystem::is_regular_file(tar, error))
            {
                error_key = L"Action.ExtractFailed";
                return false;
            }
            staging = storage / (L"staging-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(staging);
            if (!run_tar(tar, archive, staging))
            {
                error_key = L"Action.ExtractFailed";
                std::filesystem::remove_all(staging, error);
                return false;
            }
            const auto extracted = staging / std::filesystem::path(ffprobe_archive_member);
            const auto hash = sha256_file(extracted);
            if (!hash || _wcsicmp(hash->c_str(), ffprobe_sha256) != 0 ||
                !validate_ffprobe(extracted))
            {
                error_key = L"Action.IntegrityFailed";
                std::filesystem::remove_all(staging, error);
                return false;
            }
            const auto bin = storage / L"bin";
            const auto destination = bin / L"ffprobe.exe";
            std::filesystem::create_directories(bin);
            if (!MoveFileExW(
                    extracted.c_str(),
                    destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error_key = L"Action.InstallFailed";
                std::filesystem::remove_all(staging, error);
                return false;
            }
            std::filesystem::remove_all(staging, error);
            return true;
        }
        catch (...)
        {
            std::error_code error;
            if (!staging.empty())
            {
                std::filesystem::remove_all(staging, error);
            }
            error_key = L"Action.InstallFailed";
            return false;
        }
    }
}
