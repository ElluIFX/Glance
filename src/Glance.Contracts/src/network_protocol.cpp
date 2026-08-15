#include "glance/contracts/network_protocol.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace
{
    class PayloadWriter
    {
    public:
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        void write(T value)
        {
            const auto offset = payload_.size();
            payload_.resize(offset + sizeof(value));
            std::memcpy(payload_.data() + offset, &value, sizeof(value));
        }

        void write(std::wstring_view value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::length_error("IPC string exceeds the protocol limit");
            }
            write(static_cast<std::uint32_t>(value.size()));
            const auto bytes = value.size() * sizeof(wchar_t);
            const auto offset = payload_.size();
            payload_.resize(offset + bytes);
            std::memcpy(payload_.data() + offset, value.data(), bytes);
        }

        [[nodiscard]] std::string finish() &&
        {
            return std::move(payload_);
        }

    private:
        std::string payload_;
    };

    class PayloadReader
    {
    public:
        explicit PayloadReader(std::string_view payload) noexcept : payload_(payload)
        {
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        [[nodiscard]] bool read(T& value) noexcept
        {
            if (remaining() < sizeof(value))
            {
                return false;
            }
            std::memcpy(&value, payload_.data() + offset_, sizeof(value));
            offset_ += sizeof(value);
            return true;
        }

        [[nodiscard]] bool read(std::wstring& value) noexcept
        {
            std::uint32_t length{};
            if (!read(length) ||
                static_cast<std::uint64_t>(length) * sizeof(wchar_t) > remaining())
            {
                return false;
            }
            const auto bytes = static_cast<std::size_t>(length) * sizeof(wchar_t);
            value.resize(length);
            std::memcpy(value.data(), payload_.data() + offset_, bytes);
            offset_ += bytes;
            return true;
        }

        [[nodiscard]] bool finished() const noexcept
        {
            return offset_ == payload_.size();
        }

    private:
        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return payload_.size() - offset_;
        }

        std::string_view payload_;
        std::size_t offset_{};
    };

    void write_installer(
        PayloadWriter& writer,
        const glance::contracts::UpdateInstallerAsset& value)
    {
        writer.write(value.version);
        writer.write(value.file_name);
        writer.write(value.download_url);
        writer.write(value.sha256);
        writer.write(value.size);
    }

    bool read_installer(
        PayloadReader& reader,
        glance::contracts::UpdateInstallerAsset& value) noexcept
    {
        return reader.read(value.version) && reader.read(value.file_name) &&
            reader.read(value.download_url) && reader.read(value.sha256) &&
            reader.read(value.size);
    }
}

namespace glance::contracts
{
    std::string encode_update_check_request(const UpdateCheckRequest& value)
    {
        PayloadWriter writer;
        writer.write(value.request_id);
        writer.write(value.last_successful_check);
        writer.write(static_cast<std::uint8_t>(value.automatic));
        writer.write(value.current_version);
        return std::move(writer).finish();
    }

    std::optional<UpdateCheckRequest> decode_update_check_request(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        UpdateCheckRequest result;
        std::uint8_t automatic{};
        if (!reader.read(result.request_id) || !reader.read(result.last_successful_check) ||
            !reader.read(automatic) || automatic > 1 || !reader.read(result.current_version) ||
            !reader.finished())
        {
            return std::nullopt;
        }
        result.automatic = automatic != 0;
        return result;
    }

    std::string encode_update_check_response(const UpdateCheckResponse& value)
    {
        PayloadWriter writer;
        writer.write(value.request_id);
        writer.write(static_cast<std::uint32_t>(value.result.status));
        writer.write(value.result.latest_version);
        writer.write(value.result.release_url);
        write_installer(writer, value.result.installer);
        writer.write(value.result.checked_at);
        return std::move(writer).finish();
    }

    std::optional<UpdateCheckResponse> decode_update_check_response(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        UpdateCheckResponse result;
        std::uint32_t status{};
        if (!reader.read(result.request_id) || !reader.read(status) ||
            status > static_cast<std::uint32_t>(UpdateCheckStatus::unavailable) ||
            !reader.read(result.result.latest_version) || !reader.read(result.result.release_url) ||
            !read_installer(reader, result.result.installer) ||
            !reader.read(result.result.checked_at) || !reader.finished())
        {
            return std::nullopt;
        }
        result.result.status = static_cast<UpdateCheckStatus>(status);
        return result;
    }

    std::string encode_update_check_deferred(std::uint64_t request_id)
    {
        PayloadWriter writer;
        writer.write(request_id);
        return std::move(writer).finish();
    }

    std::optional<std::uint64_t> decode_update_check_deferred(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        std::uint64_t result{};
        return reader.read(result) && reader.finished()
            ? std::optional<std::uint64_t>(result)
            : std::nullopt;
    }

    std::string encode_network_download_request(const NetworkDownloadMessage& value)
    {
        PayloadWriter writer;
        writer.write(value.request_id);
        writer.write(value.request.url);
        writer.write(value.request.file_name);
        writer.write(value.request.sha256);
        writer.write(value.request.expected_size);
        return std::move(writer).finish();
    }

    std::optional<NetworkDownloadMessage> decode_network_download_request(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        NetworkDownloadMessage result;
        if (!reader.read(result.request_id) || !reader.read(result.request.url) ||
            !reader.read(result.request.file_name) || !reader.read(result.request.sha256) ||
            !reader.read(result.request.expected_size) || !reader.finished())
        {
            return std::nullopt;
        }
        return result;
    }

    std::string encode_network_download_progress(const NetworkDownloadProgress& value)
    {
        PayloadWriter writer;
        writer.write(value.request_id);
        writer.write(value.downloaded);
        writer.write(value.total);
        return std::move(writer).finish();
    }

    std::optional<NetworkDownloadProgress> decode_network_download_progress(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        NetworkDownloadProgress result;
        if (!reader.read(result.request_id) || !reader.read(result.downloaded) ||
            !reader.read(result.total) || !reader.finished())
        {
            return std::nullopt;
        }
        return result;
    }

    std::string encode_network_download_response(const NetworkDownloadResponse& value)
    {
        PayloadWriter writer;
        writer.write(value.request_id);
        writer.write(static_cast<std::uint32_t>(value.result.status));
        writer.write(value.result.path);
        return std::move(writer).finish();
    }

    std::optional<NetworkDownloadResponse> decode_network_download_response(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        NetworkDownloadResponse result;
        std::uint32_t status{};
        if (!reader.read(result.request_id) || !reader.read(status) ||
            status > static_cast<std::uint32_t>(NetworkDownloadStatus::integrity_error) ||
            !reader.read(result.result.path) || !reader.finished())
        {
            return std::nullopt;
        }
        result.result.status = static_cast<NetworkDownloadStatus>(status);
        return result;
    }

    std::string encode_network_download_cancel(std::uint64_t request_id)
    {
        PayloadWriter writer;
        writer.write(request_id);
        return std::move(writer).finish();
    }

    std::optional<std::uint64_t> decode_network_download_cancel(
        std::string_view payload) noexcept
    {
        PayloadReader reader(payload);
        std::uint64_t result{};
        return reader.read(result) && reader.finished()
            ? std::optional<std::uint64_t>(result)
            : std::nullopt;
    }
}
