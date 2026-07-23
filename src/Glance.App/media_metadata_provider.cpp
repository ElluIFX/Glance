#include "pch.h"
#include "media_metadata_provider.h"

#include "localization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace
{
    constexpr std::size_t max_probe_output_bytes = 2 * 1024 * 1024;
    constexpr std::size_t max_tag_value_characters = 512;
    constexpr std::uint32_t max_stream_count = 64;
    constexpr std::size_t max_tag_count = 128;

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
        std::wstring_view path)
    {
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE read_pipe{};
        HANDLE write_pipe{};
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
        {
            return {};
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

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
        std::wstring command = quote_argument(executable.wstring())
            + L" -v error -show_entries " + std::wstring(entries)
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

        std::string output;
        std::thread reader([read_pipe, &output]() {
            std::array<char, 8192> buffer{};
            DWORD read{};
            while (ReadFile(
                       read_pipe,
                       buffer.data(),
                       static_cast<DWORD>(buffer.size()),
                       &read,
                       nullptr) &&
                   read > 0)
            {
                const std::size_t remaining = max_probe_output_bytes - output.size();
                output.append(buffer.data(), std::min<std::size_t>(read, remaining));
            }
            CloseHandle(read_pipe);
        });

        if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 1000);
        }
        reader.join();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return output;
    }

    std::string run_ffprobe(std::wstring_view path)
    {
        for (const auto& executable : ffprobe_candidates())
        {
            auto output = run_ffprobe_executable(executable, path);
            if (!output.empty())
            {
                return output;
            }
        }
        return {};
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
        switch (value.ValueType())
        {
        case winrt::Windows::Data::Json::JsonValueType::String:
            return value.GetString().c_str();
        case winrt::Windows::Data::Json::JsonValueType::Number:
        {
            std::wostringstream output;
            output << std::setprecision(15) << value.GetNumber();
            return output.str();
        }
        default:
            return {};
        }
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

    double frame_rate_value(std::wstring_view value)
    {
        const auto separator = value.find(L'/');
        if (separator == std::wstring_view::npos)
        {
            return decimal_value(value);
        }
        const double numerator = decimal_value(value.substr(0, separator));
        const double denominator = decimal_value(value.substr(separator + 1));
        return denominator == 0.0 ? 0.0 : numerator / denominator;
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

    std::wstring format_rate(std::uint64_t bitrate)
    {
        if (bitrate == 0)
        {
            return {};
        }
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

    std::wstring format_duration(std::wstring_view raw)
    {
        const double seconds = decimal_value(raw);
        if (seconds <= 0.0)
        {
            return {};
        }
        const auto total_milliseconds = static_cast<std::uint64_t>(std::round(seconds * 1000.0));
        const auto hours = total_milliseconds / 3600000;
        const auto minutes = total_milliseconds / 60000 % 60;
        const auto whole_seconds = total_milliseconds / 1000 % 60;
        const auto milliseconds = total_milliseconds % 1000;
        std::wostringstream output;
        if (hours > 0)
        {
            output << hours << L':' << std::setfill(L'0') << std::setw(2) << minutes;
        }
        else
        {
            output << minutes;
        }
        output << L':' << std::setfill(L'0') << std::setw(2) << whole_seconds;
        if (milliseconds != 0)
        {
            output << L'.' << std::setw(3) << milliseconds;
        }
        return output.str();
    }

    std::wstring format_frame_rate(std::wstring_view raw)
    {
        const double rate = frame_rate_value(raw);
        if (rate <= 0.0)
        {
            return {};
        }
        std::wostringstream output;
        output << std::fixed << std::setprecision(
            std::abs(rate - std::round(rate)) < 0.01 ? 0 : 3)
               << rate << L" fps";
        return output.str();
    }

    std::wstring join_values(
        std::initializer_list<std::wstring_view> values,
        std::wstring_view separator = L" / ")
    {
        std::wstring result;
        for (const auto value : values)
        {
            if (!value.empty() && value != L"unknown" && value != L"N/A")
            {
                if (!result.empty())
                {
                    result.append(separator);
                }
                result.append(value);
            }
        }
        return result;
    }

    std::wstring normalized_tag_name(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    bool skip_tag(std::wstring_view normalized)
    {
        return normalized == L"major_brand" ||
               normalized == L"minor_version" ||
               normalized == L"compatible_brands" ||
               normalized == L"duration" ||
               normalized == L"bps" ||
               normalized == L"filesize" ||
               normalized.starts_with(L"number_of_") ||
               normalized.starts_with(L"_statistics_");
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

    std::wstring compact_tag_value(std::wstring value)
    {
        for (auto& character : value)
        {
            if (character == L'\r' || character == L'\n' || character == L'\t')
            {
                character = L' ';
            }
        }
        if (value.size() > max_tag_value_characters)
        {
            value.resize(max_tag_value_characters);
            value.append(L"...");
        }
        return value;
    }

    void append_tags(
        const winrt::Windows::Data::Json::JsonObject& object,
        std::vector<std::pair<std::wstring, std::wstring>>& destination,
        std::unordered_set<std::wstring>& seen)
    {
        const auto tags = object.GetNamedObject(L"tags", nullptr);
        if (tags == nullptr)
        {
            return;
        }
        for (const auto& entry : tags)
        {
            if (destination.size() >= max_tag_count)
            {
                break;
            }
            std::wstring name(entry.Key());
            const std::wstring normalized = normalized_tag_name(name);
            if (skip_tag(normalized))
            {
                continue;
            }
            std::wstring value;
            if (entry.Value().ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
            {
                value = compact_tag_value(entry.Value().GetString().c_str());
            }
            if (value.empty())
            {
                continue;
            }
            const std::wstring identity = normalized + L"\n" + normalized_tag_name(value);
            if (seen.insert(identity).second)
            {
                destination.emplace_back(display_tag_name(std::move(name)), std::move(value));
            }
        }
    }

    bool disposition_value(
        const winrt::Windows::Data::Json::JsonObject& stream,
        wchar_t const* name)
    {
        const auto disposition = stream.GetNamedObject(L"disposition", nullptr);
        return disposition != nullptr && disposition.GetNamedNumber(name, 0) != 0;
    }

    std::wstring stream_type_label(std::wstring_view type)
    {
        if (type == L"video") return glance::app::localize(L"MediaTypeVideo");
        if (type == L"audio") return glance::app::localize(L"MediaTypeAudio");
        if (type == L"subtitle") return glance::app::localize(L"MediaTypeSubtitle");
        if (type == L"attachment") return glance::app::localize(L"MediaTypeAttachment");
        return glance::app::localize(L"MediaTypeData");
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
}

namespace glance::app
{
    bool media_probe_available() noexcept
    {
        try
        {
            return !ffprobe_candidates().empty();
        }
        catch (...)
        {
            return false;
        }
    }

    MediaTechnicalMetadata probe_media_metadata(std::wstring_view path) noexcept
    {
        MediaTechnicalMetadata result;
        try
        {
            const std::string output = run_ffprobe(path);
            if (output.empty())
            {
                return result;
            }
            const auto root = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(output));
            const auto format = root.GetNamedObject(L"format", nullptr);
            std::unordered_set<std::wstring> seen_tags;
            if (format != nullptr)
            {
                result.container = json_string(format, L"format_long_name");
                if (result.container.empty())
                {
                    result.container = json_string(format, L"format_name");
                }
                result.duration = format_duration(json_string(format, L"duration"));
                result.overall_bitrate = format_rate(unsigned_value(json_string(format, L"bit_rate")));
                append_tags(format, result.tags, seen_tags);
            }

            const auto chapters = root.GetNamedArray(L"chapters", nullptr);
            result.chapter_count = chapters == nullptr ? 0 : chapters.Size();

            const auto streams = root.GetNamedArray(L"streams", nullptr);
            if (streams == nullptr)
            {
                return result;
            }
            const auto stream_count = std::min(streams.Size(), max_stream_count);
            result.streams.reserve(stream_count);
            for (std::uint32_t index = 0; index < stream_count; ++index)
            {
                const auto stream = streams.GetObjectAt(index);
                MediaStreamMetadata item;
                item.type = json_string(stream, L"codec_type");
                item.codec = friendly_codec(json_string(stream, L"codec_name"));
                item.codec_description = json_string(stream, L"codec_long_name");
                item.profile = json_string(stream, L"profile");
                item.level = json_string(stream, L"level");
                item.width = json_string(stream, L"width");
                item.height = json_string(stream, L"height");
                item.frame_rate = format_frame_rate(json_string(stream, L"avg_frame_rate"));
                if (item.frame_rate.empty())
                {
                    item.frame_rate = format_frame_rate(json_string(stream, L"r_frame_rate"));
                }
                item.pixel_format = json_string(stream, L"pix_fmt");
                item.bit_depth = json_string(stream, L"bits_per_raw_sample");
                if (item.bit_depth.empty() || item.bit_depth == L"0")
                {
                    item.bit_depth = json_string(stream, L"bits_per_sample");
                }
                item.color = join_values({
                    json_string(stream, L"color_primaries"),
                    json_string(stream, L"color_transfer"),
                    json_string(stream, L"color_space"),
                    json_string(stream, L"color_range") });
                item.aspect_ratio = join_values({
                    json_string(stream, L"sample_aspect_ratio"),
                    json_string(stream, L"display_aspect_ratio") });
                item.bitrate = format_rate(unsigned_value(json_string(stream, L"bit_rate")));
                item.sample_rate = json_string(stream, L"sample_rate");
                item.sample_format = json_string(stream, L"sample_fmt");
                item.channels = json_string(stream, L"channels");
                item.channel_layout = json_string(stream, L"channel_layout");
                item.duration = format_duration(json_string(stream, L"duration"));
                item.frame_count = json_string(stream, L"nb_frames");
                item.is_default = disposition_value(stream, L"default");
                item.is_forced = disposition_value(stream, L"forced");

                const auto tags = stream.GetNamedObject(L"tags", nullptr);
                if (tags != nullptr)
                {
                    item.language = json_string(tags, L"language");
                    item.title = json_string(tags, L"title");
                }
                const auto side_data = stream.GetNamedArray(L"side_data_list", nullptr);
                if (side_data != nullptr)
                {
                    for (std::uint32_t side_index = 0; side_index < side_data.Size(); ++side_index)
                    {
                        item.rotation = json_string(side_data.GetObjectAt(side_index), L"rotation");
                        if (!item.rotation.empty())
                        {
                            item.rotation += L"\u00b0";
                            break;
                        }
                    }
                }

                result.streams.push_back(std::move(item));
            }
            std::ranges::sort(result.tags, {}, &std::pair<std::wstring, std::wstring>::first);
        }
        catch (...)
        {
        }
        return result;
    }

    std::wstring format_media_advanced_metadata(const MediaTechnicalMetadata& metadata)
    {
        std::wstring result;
        append_line(result, localize(L"MediaInfoContainer"), metadata.container);
        append_line(result, localize(L"MediaInfoDuration"), metadata.duration);
        append_line(result, localize(L"MediaInfoOverallBitrate"), metadata.overall_bitrate);
        if (metadata.chapter_count > 0)
        {
            append_line(
                result,
                localize(L"MediaInfoChapters"),
                std::to_wstring(metadata.chapter_count));
        }

        std::uint32_t stream_index{};
        for (const auto& stream : metadata.streams)
        {
            ++stream_index;
            if (!result.empty())
            {
                result.append(L"\n\n");
            }
            result.append(localize_format(
                L"MediaInfoStreamHeading",
                { stream_type_label(stream.type), std::to_wstring(stream_index) }));

            std::wstring codec = stream.codec;
            if (!stream.profile.empty())
            {
                codec += codec.empty() ? stream.profile : L" / " + stream.profile;
            }
            if (!stream.codec_description.empty() &&
                _wcsicmp(stream.codec_description.c_str(), stream.codec.c_str()) != 0)
            {
                codec += codec.empty()
                    ? stream.codec_description
                    : L" (" + stream.codec_description + L")";
            }
            append_line(result, localize(L"MediaInfoCodec"), codec);
            if (!stream.width.empty() && !stream.height.empty())
            {
                append_line(
                    result,
                    localize(L"MediaInfoResolution"),
                    stream.width + L" x " + stream.height);
            }
            append_line(result, localize(L"MediaInfoFrameRate"), stream.frame_rate);
            append_line(result, localize(L"MediaInfoPixelFormat"), stream.pixel_format);
            append_line(result, localize(L"MediaInfoBitDepth"), stream.bit_depth);
            append_line(result, localize(L"MediaInfoColor"), stream.color);
            append_line(result, localize(L"MediaInfoAspectRatio"), stream.aspect_ratio);
            if (!stream.rotation.empty())
            {
                append_line(result, localize(L"MediaInfoRotation"), stream.rotation);
            }
            if (!stream.sample_rate.empty())
            {
                const auto rate = unsigned_value(stream.sample_rate);
                append_line(
                    result,
                    localize(L"MediaInfoSampleRate"),
                    rate == 0 ? stream.sample_rate : std::to_wstring(rate) + L" Hz");
            }
            append_line(result, localize(L"MediaInfoSampleFormat"), stream.sample_format);
            if (!stream.channels.empty())
            {
                append_line(
                    result,
                    localize(L"MediaInfoChannels"),
                    join_values({ stream.channels, stream.channel_layout }));
            }
            append_line(result, localize(L"MediaInfoBitrate"), stream.bitrate);
            append_line(result, localize(L"MediaInfoDuration"), stream.duration);
            append_line(result, localize(L"MediaInfoFrames"), stream.frame_count);
            append_line(result, localize(L"MediaInfoLanguage"), stream.language);
            append_line(result, localize(L"MediaInfoTitle"), stream.title);

            std::vector<std::wstring> dispositions;
            if (stream.is_default)
            {
                dispositions.push_back(localize(L"MediaInfoDefault"));
            }
            if (stream.is_forced)
            {
                dispositions.push_back(localize(L"MediaInfoForced"));
            }
            std::wstring disposition;
            for (const auto& value : dispositions)
            {
                disposition += disposition.empty() ? value : L", " + value;
            }
            append_line(result, localize(L"MediaInfoDisposition"), disposition);
        }

        if (!metadata.tags.empty())
        {
            if (!result.empty())
            {
                result.append(L"\n\n");
            }
            result.append(localize(L"MediaInfoMetadata"));
            for (const auto& [name, value] : metadata.tags)
            {
                append_line(result, name, value);
            }
        }
        return result;
    }
}
