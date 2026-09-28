#include "pch.h"
#include "media_probe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace glance::contracts::components;

    constexpr std::size_t maximum_probe_output_bytes = 2 * 1024 * 1024;
    constexpr std::size_t maximum_tag_value_characters = 512;
    constexpr std::uint32_t maximum_stream_count = 64;
    constexpr std::size_t maximum_tag_count = 128;
    struct CancellationProbe
    {
        void* context{};
        BOOL(WINAPI* is_cancelled)(void*) noexcept{};
    };

    struct ProcessOutput
    {
        std::string text;
        bool succeeded{};
        bool cancelled{};
    };

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

    bool copy_panel_text(
        InformationPanelText& destination,
        ComponentTextKind kind,
        std::wstring_view value,
        std::initializer_list<std::wstring_view> arguments = {})
    {
        if (value.size() >= std::size(destination.value) ||
            arguments.size() > maximum_information_panel_arguments)
        {
            return false;
        }
        destination.kind = kind;
        std::copy(value.begin(), value.end(), destination.value);
        destination.value[value.size()] = L'\0';
        destination.argument_count = static_cast<std::uint32_t>(arguments.size());
        std::size_t index{};
        for (const auto argument : arguments)
        {
            if (argument.size() >= std::size(destination.arguments[index]))
            {
                return false;
            }
            std::copy(
                argument.begin(),
                argument.end(),
                destination.arguments[index]);
            destination.arguments[index][argument.size()] = L'\0';
            ++index;
        }
        return true;
    }

    bool append_panel_entry(
        const InformationPanelSink& sink,
        InformationPanelEntryKind kind,
        ComponentTextKind label_kind,
        std::wstring_view label,
        std::wstring_view value = {},
        ComponentTextKind value_kind = ComponentTextKind::literal,
        std::initializer_list<std::wstring_view> label_arguments = {})
    {
        InformationPanelEntry entry;
        entry.kind = kind;
        return copy_panel_text(entry.label, label_kind, label, label_arguments) &&
            copy_panel_text(entry.value, value_kind, value) &&
            sink.append(sink.context, &entry);
    }

    const wchar_t* stream_heading_key(std::wstring_view type) noexcept
    {
        if (type == L"video") return L"MediaInfo.VideoStream";
        if (type == L"audio") return L"MediaInfo.AudioStream";
        if (type == L"subtitle") return L"MediaInfo.SubtitleStream";
        if (type == L"attachment") return L"MediaInfo.AttachmentStream";
        return L"MediaInfo.DataStream";
    }

    bool format_probe_json(
        std::string_view json,
        const InformationPanelSink& sink)
    {
        using winrt::Windows::Data::Json::JsonObject;
        const auto root = JsonObject::Parse(winrt::to_hstring(json));
        bool emitted{};
        const auto append_field = [&](std::wstring_view label, std::wstring value) {
            if (value.empty())
            {
                return true;
            }
            emitted = true;
            return append_panel_entry(
                sink,
                InformationPanelEntryKind::field,
                ComponentTextKind::resource_key,
                label,
                value);
        };

        const auto format = root.GetNamedObject(L"format", nullptr);
        std::vector<std::pair<std::wstring, std::wstring>> tags;
        if (format != nullptr)
        {
            if (!append_panel_entry(
                    sink,
                    InformationPanelEntryKind::section,
                    ComponentTextKind::resource_key,
                    L"MediaInfo.General"))
            {
                return false;
            }
            auto container = json_string(format, L"format_long_name");
            if (container.empty())
            {
                container = json_string(format, L"format_name");
            }
            if (!append_field(L"MediaInfo.Container", std::move(container)) ||
                !append_field(
                    L"MediaInfo.Duration",
                    format_duration(json_string(format, L"duration"))) ||
                !append_field(
                    L"MediaInfo.OverallBitrate",
                    format_rate(unsigned_value(json_string(format, L"bit_rate")))))
            {
                return false;
            }
            append_tags(format, tags);
        }
        if (const auto chapters = root.GetNamedArray(L"chapters", nullptr);
            chapters != nullptr && chapters.Size() != 0 &&
            !append_field(L"MediaInfo.Chapters", std::to_wstring(chapters.Size())))
        {
            return false;
        }

        const auto streams = root.GetNamedArray(L"streams", nullptr);
        if (streams != nullptr)
        {
            const auto count = std::min(streams.Size(), maximum_stream_count);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const auto stream = streams.GetObjectAt(index);
                const auto type = json_string(stream, L"codec_type");
                const auto stream_number = std::to_wstring(index + 1);
                if (!append_panel_entry(
                        sink,
                        InformationPanelEntryKind::section,
                        ComponentTextKind::resource_key,
                        stream_heading_key(type),
                        {},
                        ComponentTextKind::literal,
                        { stream_number }))
                {
                    return false;
                }
                emitted = true;

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
                if (!append_field(L"MediaInfo.Codec", std::move(codec)))
                {
                    return false;
                }

                const auto width = json_string(stream, L"width");
                const auto height = json_string(stream, L"height");
                if (!width.empty() && !height.empty() &&
                    !append_field(L"MediaInfo.Resolution", width + L"x" + height))
                {
                    return false;
                }
                auto frame_rate = format_frame_rate(json_string(stream, L"avg_frame_rate"));
                if (frame_rate.empty())
                {
                    frame_rate = format_frame_rate(json_string(stream, L"r_frame_rate"));
                }
                auto bit_depth = json_string(stream, L"bits_per_raw_sample");
                if (bit_depth.empty() || bit_depth == L"0")
                {
                    bit_depth = json_string(stream, L"bits_per_sample");
                }
                if (!append_field(L"MediaInfo.FrameRate", std::move(frame_rate)) ||
                    !append_field(L"MediaInfo.PixelFormat", json_string(stream, L"pix_fmt")) ||
                    !append_field(L"MediaInfo.BitDepth", std::move(bit_depth)) ||
                    !append_field(
                        L"MediaInfo.Color",
                        join_values({
                            json_string(stream, L"color_primaries"),
                            json_string(stream, L"color_transfer"),
                            json_string(stream, L"color_space"),
                            json_string(stream, L"color_range") })) ||
                    !append_field(
                        L"MediaInfo.AspectRatio",
                        join_values({
                            json_string(stream, L"sample_aspect_ratio"),
                            json_string(stream, L"display_aspect_ratio") })))
                {
                    return false;
                }
                if (const auto side_data = stream.GetNamedArray(L"side_data_list", nullptr))
                {
                    for (const auto& value : side_data)
                    {
                        const auto rotation = json_string(value.GetObjectW(), L"rotation");
                        if (!rotation.empty())
                        {
                            if (!append_field(L"MediaInfo.Rotation", rotation + L"\u00b0"))
                            {
                                return false;
                            }
                            break;
                        }
                    }
                }

                const auto sample_rate = json_string(stream, L"sample_rate");
                if (!sample_rate.empty())
                {
                    const auto numeric_rate = unsigned_value(sample_rate);
                    if (!append_field(
                            L"MediaInfo.SampleRate",
                            numeric_rate == 0
                                ? sample_rate
                                : std::to_wstring(numeric_rate) + L"Hz"))
                    {
                        return false;
                    }
                }
                if (!append_field(L"MediaInfo.SampleFormat", json_string(stream, L"sample_fmt")) ||
                    !append_field(
                        L"MediaInfo.Channels",
                        join_values({
                            json_string(stream, L"channels"),
                            json_string(stream, L"channel_layout") })) ||
                    !append_field(
                        L"MediaInfo.Bitrate",
                        format_rate(unsigned_value(json_string(stream, L"bit_rate")))) ||
                    !append_field(
                        L"MediaInfo.Duration",
                        format_duration(json_string(stream, L"duration"))) ||
                    !append_field(L"MediaInfo.Frames", json_string(stream, L"nb_frames")))
                {
                    return false;
                }

                if (const auto stream_tags = stream.GetNamedObject(L"tags", nullptr))
                {
                    if (!append_field(
                            L"MediaInfo.Language",
                            json_string(stream_tags, L"language")) ||
                        !append_field(
                            L"MediaInfo.Title",
                            json_string(stream_tags, L"title")))
                    {
                        return false;
                    }
                }
                if (const auto disposition = stream.GetNamedObject(L"disposition", nullptr))
                {
                    const bool is_default =
                        disposition.GetNamedNumber(L"default", 0) != 0;
                    const bool is_forced =
                        disposition.GetNamedNumber(L"forced", 0) != 0;
                    const wchar_t* value_key = is_default && is_forced
                        ? L"MediaInfo.DefaultForced"
                        : is_default
                            ? L"MediaInfo.Default"
                            : is_forced ? L"MediaInfo.Forced" : nullptr;
                    if (value_key != nullptr)
                    {
                        emitted = true;
                        if (!append_panel_entry(
                                sink,
                                InformationPanelEntryKind::field,
                                ComponentTextKind::resource_key,
                                L"MediaInfo.Disposition",
                                value_key,
                                ComponentTextKind::resource_key))
                        {
                            return false;
                        }
                    }
                }
            }
        }

        if (!tags.empty())
        {
            if (!append_panel_entry(
                    sink,
                    InformationPanelEntryKind::section,
                    ComponentTextKind::resource_key,
                    L"MediaInfo.Metadata"))
            {
                return false;
            }
            emitted = true;
            for (const auto& [name, value] : tags)
            {
                if (!append_panel_entry(
                        sink,
                        InformationPanelEntryKind::field,
                        ComponentTextKind::literal,
                        name,
                        value))
                {
                    return false;
                }
            }
        }
        return emitted;
    }


    ProcessOutput run_media_probe(
        const glance::contracts::dependencies::HostApi& dependencies,
        std::wstring_view path,
        const CancellationProbe* sink)
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
        const std::wstring source(path);
        const wchar_t* arguments[]{L"-v", L"error", L"-show_entries", entries.data(), L"-of", L"json", source.c_str()};
        ProcessOutput output;
        const glance::contracts::dependencies::ProcessRequest request{
            .dependency_id = L"ffprobe", .entry_id = L"ffprobe",
            .arguments = arguments, .argument_count = static_cast<std::uint32_t>(std::size(arguments)),
            .maximum_output_bytes = maximum_probe_output_bytes,
            .cancellation = {sink ? sink->context : nullptr, sink ? sink->is_cancelled : nullptr},
            .output = {&output, [](void* context, BOOL error, const char* bytes, std::uint32_t size) noexcept -> BOOL {
                try { if (!error) static_cast<ProcessOutput*>(context)->text.append(bytes, size); return TRUE; }
                catch (...) { return FALSE; }
            }}};
        glance::contracts::dependencies::ProcessResult result;
        const auto status = dependencies.execute(&request, &result);
        output.succeeded = SUCCEEDED(status) && result.exit_code == 0;
        output.cancelled = status == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return output;
    }
}

namespace glance::components::media_info
{

    PrepareStatus query_media_info(
        const glance::contracts::dependencies::HostApi& dependencies,
        std::wstring_view path,
        const InformationPanelSink& sink) noexcept
    {
        try
        {
            const CancellationProbe cancellation{
                .context = sink.context,
                .is_cancelled = sink.is_cancelled };
            const auto output = run_media_probe(dependencies, path, &cancellation);
            if (output.cancelled)
            {
                return PrepareStatus::cancelled;
            }
            if (!output.succeeded || output.text.empty())
            {
                return PrepareStatus::failed;
            }
            return format_probe_json(output.text, sink)
                ? PrepareStatus::success
                : sink.is_cancelled != nullptr && sink.is_cancelled(sink.context)
                    ? PrepareStatus::cancelled
                    : PrepareStatus::failed;
        }
        catch (...)
        {
            return PrepareStatus::failed;
        }
    }

    std::wstring query_media_json(
        const glance::contracts::dependencies::HostApi& dependencies,
        std::wstring_view path,
        const glance::contracts::components::HoverInfoTextSink& sink) noexcept
    {
        try
        {
            const CancellationProbe cancellation{
                .context = sink.context,
                .is_cancelled = sink.is_cancelled };
            const auto output = run_media_probe(dependencies, path, &cancellation);
            if (output.cancelled || !output.succeeded || output.text.empty())
            {
                return {};
            }
            const auto json = winrt::to_hstring(output.text);
            static_cast<void>(winrt::Windows::Data::Json::JsonObject::Parse(json));
            return std::wstring(json);
        }
        catch (...)
        {
            return {};
        }
    }

}
