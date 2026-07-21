#include "pch.h"
#include "media_metadata_provider.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

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

    std::vector<std::filesystem::path> ffprobe_candidates()
    {
        std::vector<std::filesystem::path> result;
        const auto bundled = executable_directory() / L"ffprobe.exe";
        std::error_code error;
        if (!bundled.empty() && std::filesystem::is_regular_file(bundled, error))
        {
            result.push_back(bundled);
        }

        wchar_t executable[32768]{};
        const DWORD length = SearchPathW(
            nullptr,
            L"ffprobe.exe",
            nullptr,
            static_cast<DWORD>(std::size(executable)),
            executable,
            nullptr);
        if (length > 0 && length < std::size(executable))
        {
            const std::filesystem::path fallback(executable);
            if (result.empty() || _wcsicmp(result.front().c_str(), fallback.c_str()) != 0)
            {
                result.push_back(fallback);
            }
        }
        return result;
    }

    std::string run_ffprobe_executable(
        const std::filesystem::path& executable,
        std::wstring_view path,
        bool audio)
    {

        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE read_pipe{};
        HANDLE write_pipe{};
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
        {
            return {};
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring command = quote_argument(executable.wstring())
            + L" -v error -select_streams " + (audio ? std::wstring(L"a:0") : std::wstring(L"v:0"))
            + L" -show_entries stream=codec_name,avg_frame_rate,bit_rate,sample_rate,bits_per_sample,bits_per_raw_sample"
            + L" -of json " + quote_argument(path);
        STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write_pipe;
        startup.hStdError = write_pipe;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process);
        CloseHandle(write_pipe);
        if (!created)
        {
            CloseHandle(read_pipe);
            return {};
        }

        if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 1000);
        }

        std::string output;
        std::array<char, 4096> buffer{};
        DWORD read{};
        while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        {
            output.append(buffer.data(), read);
        }
        CloseHandle(read_pipe);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return output;
    }

    std::string run_ffprobe(std::wstring_view path, bool audio)
    {
        for (const auto& executable : ffprobe_candidates())
        {
            auto output = run_ffprobe_executable(executable, path, audio);
            if (!output.empty())
            {
                return output;
            }
        }
        return {};
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

    std::uint64_t unsigned_value(const winrt::Windows::Data::Json::JsonObject& object, wchar_t const* name)
    {
        try
        {
            return std::stoull(object.GetNamedString(name, L"0").c_str());
        }
        catch (...)
        {
            return 0;
        }
    }

    double frame_rate_value(std::wstring_view value)
    {
        const auto separator = value.find(L'/');
        try
        {
            if (separator == std::wstring_view::npos)
            {
                return std::stod(std::wstring(value));
            }
            const double numerator = std::stod(std::wstring(value.substr(0, separator)));
            const double denominator = std::stod(std::wstring(value.substr(separator + 1)));
            return denominator == 0.0 ? 0.0 : numerator / denominator;
        }
        catch (...)
        {
            return 0.0;
        }
    }

    std::wstring format_rate(std::uint64_t bitrate)
    {
        std::wostringstream output;
        if (bitrate >= 1000000)
        {
            output << std::fixed << std::setprecision(1) << bitrate / 1000000.0 << L" Mbps";
        }
        else
        {
            output << (bitrate + 500) / 1000 << L" kbps";
        }
        return output.str();
    }
}

namespace glance::app
{
    MediaTechnicalMetadata probe_media_metadata(std::wstring_view path, bool audio) noexcept
    {
        MediaTechnicalMetadata result;
        try
        {
            const std::string output = run_ffprobe(path, audio);
            if (output.empty())
            {
                return result;
            }
            const auto root = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(output));
            const auto streams = root.GetNamedArray(L"streams", nullptr);
            if (streams == nullptr || streams.Size() == 0)
            {
                return result;
            }
            const auto stream = streams.GetObjectAt(0);
            result.codec = friendly_codec(stream.GetNamedString(L"codec_name", L"").c_str());
            result.bitrate = unsigned_value(stream, L"bit_rate");
            if (audio)
            {
                result.sample_rate = static_cast<std::uint32_t>(unsigned_value(stream, L"sample_rate"));
                result.bit_depth = static_cast<std::uint32_t>(unsigned_value(stream, L"bits_per_raw_sample"));
                if (result.bit_depth == 0)
                {
                    result.bit_depth = static_cast<std::uint32_t>(unsigned_value(stream, L"bits_per_sample"));
                }
            }
            else
            {
                result.frame_rate = frame_rate_value(stream.GetNamedString(L"avg_frame_rate", L"0/0").c_str());
            }
        }
        catch (...)
        {
        }
        return result;
    }

    std::wstring format_media_metadata(const MediaTechnicalMetadata& metadata, bool audio)
    {
        std::vector<std::wstring> parts;
        if (audio)
        {
            if (metadata.sample_rate > 0)
            {
                std::wostringstream rate;
                rate << std::fixed << std::setprecision(metadata.sample_rate % 1000 == 0 ? 0 : 1)
                     << metadata.sample_rate / 1000.0 << L" kHz";
                parts.push_back(rate.str());
            }
            if (metadata.bit_depth > 0)
            {
                parts.push_back(std::to_wstring(metadata.bit_depth) + L"-bit");
            }
            if (!metadata.codec.empty())
            {
                parts.push_back(metadata.codec);
            }
        }
        else
        {
            if (metadata.frame_rate > 0.0)
            {
                std::wostringstream fps;
                fps << std::fixed << std::setprecision(
                    std::abs(metadata.frame_rate - std::round(metadata.frame_rate)) < 0.01 ? 0 : 2)
                    << metadata.frame_rate << L" fps";
                parts.push_back(fps.str());
            }
            if (!metadata.codec.empty())
            {
                parts.push_back(metadata.codec);
            }
        }
        if (metadata.bitrate > 0)
        {
            parts.push_back(format_rate(metadata.bitrate));
        }

        std::wstring result;
        for (const auto& part : parts)
        {
            result += result.empty() ? part : L"  |  " + part;
        }
        return result;
    }
}
