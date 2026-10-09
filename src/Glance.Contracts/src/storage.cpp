#include "glance/contracts/storage.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <memory>
#include <system_error>
#pragma comment(lib, "Version.lib")

namespace
{
    using namespace glance::contracts::storage;
    struct HandleCloser
    {
        void operator()(void *value) const noexcept
        {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    struct KeyCloser
    {
        void operator()(HKEY value) const noexcept
        {
            if (value)
                RegCloseKey(value);
        }
    };
    using Handle = std::unique_ptr<void, HandleCloser>;
    using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

    std::wstring relative_group(std::wstring_view input)
    {
        constexpr std::wstring_view root = L"Software\\Glance";
        if (input == root)
            return {};
        if (input.starts_with(root) && input.size() > root.size() && input[root.size()] == L'\\')
            input.remove_prefix(root.size() + 1);
        return std::wstring(input);
    }
    std::string utf8(std::wstring_view value)
    {
        if (value.empty())
            return {};
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                                static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (!length)
            throw std::system_error(GetLastError(), std::system_category());
        std::string result(length, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr);
        return result;
    }
    std::wstring wide(std::string_view value)
    {
        if (value.empty())
            return {};
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                static_cast<int>(value.size()), nullptr, 0);
        if (!length)
            throw std::system_error(GetLastError(), std::system_category());
        std::wstring result(length, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            length);
        return result;
    }
    LSTATUS caught_error() noexcept
    {
        try
        {
            throw;
        }
        catch (const std::system_error &error)
        {
            return static_cast<LSTATUS>(error.code().value());
        }
        catch (const std::bad_alloc &)
        {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        catch (...)
        {
            return ERROR_INVALID_DATA;
        }
    }
    std::string hex(std::span<const std::uint8_t> bytes)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(bytes.size() * 2, '0');
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            result[i * 2] = digits[bytes[i] >> 4];
            result[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return result;
    }
    std::optional<std::vector<std::uint8_t>> unhex(std::string_view text)
    {
        if (text.size() % 2)
            return std::nullopt;
        std::vector<std::uint8_t> result(text.size() / 2);
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            const int a = digit(text[i * 2]), b = digit(text[i * 2 + 1]);
            if (a < 0 || b < 0)
                return std::nullopt;
            result[i] = static_cast<std::uint8_t>((a << 4) | b);
        }
        return result;
    }
    std::string encode(const Snapshot &values)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("version");
        writer.Uint(1);
        writer.Key("keys");
        writer.StartObject();
        for (const auto &[group, entries] : values)
        {
            const auto group_text = utf8(group);
            writer.Key(group_text.data(), static_cast<rapidjson::SizeType>(group_text.size()));
            writer.StartObject();
            for (const auto &[name, item] : entries)
            {
                const auto name_text = utf8(name);
                writer.Key(name_text.data(), static_cast<rapidjson::SizeType>(name_text.size()));
                writer.StartObject();
                writer.Key("type");
                writer.Uint(item.type);
                if (item.type == REG_DWORD && item.bytes.size() == sizeof(DWORD))
                {
                    DWORD value{};
                    std::memcpy(&value, item.bytes.data(), sizeof(value));
                    writer.Key("value");
                    writer.Uint(value);
                }
                else if (item.type == REG_QWORD && item.bytes.size() == sizeof(std::uint64_t))
                {
                    std::uint64_t value{};
                    std::memcpy(&value, item.bytes.data(), sizeof(value));
                    const auto text = std::to_string(value);
                    writer.Key("value");
                    writer.String(text.c_str());
                }
                else if ((item.type == REG_SZ || item.type == REG_EXPAND_SZ) && item.bytes.size() >= sizeof(wchar_t) &&
                         item.bytes.size() % sizeof(wchar_t) == 0)
                {
                    std::wstring value(item.bytes.size() / sizeof(wchar_t), L'\0');
                    std::memcpy(value.data(), item.bytes.data(), item.bytes.size());
                    const bool terminated = value.back() == L'\0';
                    if (terminated)
                        value.pop_back();
                    if (!value.empty() &&
                        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr) == 0)
                    {
                        const auto text = hex(item.bytes);
                        writer.Key("bytes");
                        writer.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
                    }
                    else
                    {
                        const auto text = utf8(value);
                        writer.Key("value");
                        writer.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
                        writer.Key("terminated");
                        writer.Bool(terminated);
                    }
                }
                else
                {
                    const auto text = hex(item.bytes);
                    writer.Key("bytes");
                    writer.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
                }
                writer.EndObject();
            }
            writer.EndObject();
        }
        writer.EndObject();
        writer.EndObject();
        return {buffer.GetString(), buffer.GetSize()};
    }
    LSTATUS decode(std::string_view text, Snapshot &result)
    {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
        if (document.HasParseError() || !document.IsObject() || !document.HasMember("version") ||
            !document["version"].IsUint() || document["version"].GetUint() != 1 || !document.HasMember("keys") ||
            !document["keys"].IsObject())
            return ERROR_INVALID_DATA;
        Snapshot decoded;
        for (auto group_iterator = document["keys"].MemberBegin(); group_iterator != document["keys"].MemberEnd();
             ++group_iterator)
        {
            const auto &group = *group_iterator;
            if (!group.value.IsObject())
                return ERROR_INVALID_DATA;
            auto [target, inserted] =
                decoded.emplace(wide({group.name.GetString(), group.name.GetStringLength()}), Values{});
            if (!inserted)
                return ERROR_INVALID_DATA;
            for (auto entry_iterator = group.value.MemberBegin(); entry_iterator != group.value.MemberEnd();
                 ++entry_iterator)
            {
                const auto &entry = *entry_iterator;
                const auto &item = entry.value;
                if (!item.IsObject() || !item.HasMember("type") || !item["type"].IsUint())
                    return ERROR_INVALID_DATA;
                Value value{item["type"].GetUint(), {}};
                if (item.HasMember("bytes") && item["bytes"].IsString() && !item.HasMember("value"))
                {
                    const auto bytes = unhex({item["bytes"].GetString(), item["bytes"].GetStringLength()});
                    if (!bytes)
                        return ERROR_INVALID_DATA;
                    value.bytes = *bytes;
                }
                else if (value.type == REG_DWORD && item.HasMember("value") && item["value"].IsUint())
                {
                    const DWORD number = item["value"].GetUint();
                    value.bytes.resize(sizeof(number));
                    std::memcpy(value.bytes.data(), &number, sizeof(number));
                }
                else if (value.type == REG_QWORD && item.HasMember("value") && item["value"].IsString())
                {
                    std::uint64_t number{};
                    const std::string_view number_text{item["value"].GetString(), item["value"].GetStringLength()};
                    const auto [end, error] =
                        std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
                    if (error != std::errc{} || end != number_text.data() + number_text.size())
                        return ERROR_INVALID_DATA;
                    value.bytes.resize(sizeof(number));
                    std::memcpy(value.bytes.data(), &number, sizeof(number));
                }
                else if ((value.type == REG_SZ || value.type == REG_EXPAND_SZ) && item.HasMember("value") &&
                         item["value"].IsString() && item.HasMember("terminated") && item["terminated"].IsBool())
                {
                    auto text_value = wide({item["value"].GetString(), item["value"].GetStringLength()});
                    if (item["terminated"].GetBool())
                        text_value.push_back(L'\0');
                    value.bytes.resize(text_value.size() * sizeof(wchar_t));
                    if (!value.bytes.empty())
                        std::memcpy(value.bytes.data(), text_value.data(), value.bytes.size());
                }
                else
                    return ERROR_INVALID_DATA;
                if (!target->second
                         .emplace(wide({entry.name.GetString(), entry.name.GetStringLength()}), std::move(value))
                         .second)
                    return ERROR_INVALID_DATA;
            }
        }
        result = std::move(decoded);
        return ERROR_SUCCESS;
    }
    LSTATUS read_handle(HANDLE handle, Snapshot &values)
    {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size))
            return GetLastError();
        if (size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
            return ERROR_INVALID_DATA;
        std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read{};
        if (!ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
            return GetLastError();
        if (read != text.size())
            return ERROR_HANDLE_EOF;
        return decode(text, values);
    }
    LSTATUS registry_snapshot(HKEY key, const std::wstring &group, Snapshot &values)
    {
        values.try_emplace(group);
        DWORD maximum_name{}, maximum_data{}, maximum_subkey{};
        auto status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, &maximum_subkey, nullptr, nullptr,
                                       &maximum_name, &maximum_data, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
            return status;
        std::vector<wchar_t> name(static_cast<std::size_t>((std::max)(maximum_name, maximum_subkey)) + 2);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(maximum_data) + 1);
        for (DWORD index = 0;; ++index)
        {
            DWORD length = static_cast<DWORD>(name.size()), size = static_cast<DWORD>(bytes.size()), type{};
            status = RegEnumValueW(key, index, name.data(), &length, nullptr, &type, bytes.data(), &size);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                return status;
            values[group].emplace(std::wstring(name.data(), length),
                                  Value{type, {bytes.begin(), bytes.begin() + size}});
        }
        for (DWORD index = 0;; ++index)
        {
            DWORD length = static_cast<DWORD>(name.size());
            status = RegEnumKeyExW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                return status;
            HKEY raw{};
            status = RegOpenKeyExW(key, name.data(), 0, KEY_READ, &raw);
            Key child(raw);
            if (status != ERROR_SUCCESS)
                return status;
            status = registry_snapshot(child.get(),
                                       group.empty() ? std::wstring(name.data(), length)
                                                     : group + L"\\" + std::wstring(name.data(), length),
                                       values);
            if (status != ERROR_SUCCESS)
                return status;
        }
        return ERROR_SUCCESS;
    }
    class FileLock
    {
    public:
        explicit FileLock(const std::filesystem::path &file)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto c : std::filesystem::absolute(file).lexically_normal().wstring())
            {
                hash ^= static_cast<std::uint16_t>(towlower(c));
                hash *= 1099511628211ULL;
            }
            mutex_.reset(CreateMutexW(nullptr, FALSE, (L"Local\\Glance.Settings." + std::to_wstring(hash)).c_str()));
            if (!mutex_)
                throw std::system_error(GetLastError(), std::system_category());
            const auto result = WaitForSingleObject(mutex_.get(), 5000);
            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                throw std::system_error(ERROR_TIMEOUT, std::system_category());
        }
        ~FileLock()
        {
            ReleaseMutex(mutex_.get());
        }

    private:
        Handle mutex_;
    };
} // namespace

namespace glance::contracts::storage
{
    std::filesystem::path application_directory()
    {
        std::wstring path(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size())
            throw std::system_error(GetLastError(), std::system_category());
        path.resize(size);
        return std::filesystem::path(path).parent_path();
    }
    std::filesystem::path installed_data_directory()
    {
        std::wstring path(32768, L'\0');
        const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size())
            throw std::system_error(ERROR_PATH_NOT_FOUND, std::system_category());
        path.resize(size);
        return std::filesystem::path(path) / L"Glance";
    }
    std::filesystem::path data_directory()
    {
        if constexpr (portable)
            return application_directory() / L"data";
        else
            return installed_data_directory();
    }
    bool binary_matches(const std::filesystem::path &file, bool expected_portable, std::wstring_view version) noexcept
    {
        try
        {
            DWORD unused{};
            const auto size = GetFileVersionInfoSizeW(file.c_str(), &unused);
            if (!size)
                return false;
            std::vector<std::uint8_t> bytes(size);
            if (!GetFileVersionInfoW(file.c_str(), 0, size, bytes.data()))
                return false;
            const auto matches = [&](const wchar_t *key, std::wstring_view expected) {
                wchar_t *text{};
                UINT length{};
                const auto name = std::wstring(L"\\StringFileInfo\\040904b0\\") + key;
                return VerQueryValueW(bytes.data(), name.c_str(), reinterpret_cast<void **>(&text), &length) &&
                       length > 0 && std::wstring_view(text, length - 1) == expected;
            };
            return matches(L"Distribution", expected_portable ? L"Portable" : L"Installed") &&
                   matches(L"ProductVersion", version);
        }
        catch (...)
        {
            return false;
        }
    }
    bool OrdinalLess::operator()(const std::wstring &left, const std::wstring &right) const noexcept
    {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                    static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
    SettingsStore::SettingsStore(Backend backend, std::filesystem::path file, std::wstring registry_root)
        : backend_(backend), file_(std::move(file)), registry_root_(std::move(registry_root))
    {
    }
    LSTATUS read_snapshot(const std::filesystem::path &file, Snapshot &values) noexcept
    {
        try
        {
            Handle handle(CreateFileW(file.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
            if (handle.get() == INVALID_HANDLE_VALUE)
                return GetLastError();
            return read_handle(handle.get(), values);
        }
        catch (...)
        {
            return caught_error();
        }
    }
    LSTATUS write_snapshot(const std::filesystem::path &file, const Snapshot &values) noexcept
    {
        std::filesystem::path temporary;
        try
        {
            const auto text = encode(values);
            if (text.size() > 64 * 1024 * 1024)
                return ERROR_FILE_TOO_LARGE;
            std::filesystem::create_directories(file.parent_path());
            static std::atomic_uint64_t sequence{};
            temporary =
                file.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++);
            Handle handle(
                CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (handle.get() == INVALID_HANDLE_VALUE)
                return GetLastError();
            DWORD written{};
            LSTATUS status = ERROR_SUCCESS;
            if (!WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
                written != text.size())
                status = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            else if (!FlushFileBuffers(handle.get()))
                status = GetLastError();
            handle.reset();
            if (status == ERROR_SUCCESS &&
                !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                status = GetLastError();
            if (status != ERROR_SUCCESS)
                DeleteFileW(temporary.c_str());
            return status;
        }
        catch (...)
        {
            if (!temporary.empty())
                DeleteFileW(temporary.c_str());
            return caught_error();
        }
    }
    LSTATUS SettingsStore::load_json()
    {
        Handle handle(CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return error;
            cache_.clear();
            stamp_.reset();
            loaded_ = true;
            return ERROR_SUCCESS;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(), &info))
            return GetLastError();
        const auto combined = [](DWORD high, DWORD low) { return (static_cast<std::uint64_t>(high) << 32) | low; };
        const FileStamp stamp{combined(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
                              combined(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime),
                              combined(info.nFileSizeHigh, info.nFileSizeLow),
                              combined(info.nFileIndexHigh, info.nFileIndexLow), info.dwVolumeSerialNumber};
        if (loaded_ && stamp_ == stamp)
            return ERROR_SUCCESS;
        Snapshot next;
        const auto status = read_handle(handle.get(), next);
        if (status == ERROR_SUCCESS)
        {
            cache_ = std::move(next);
            stamp_ = stamp;
            loaded_ = true;
        }
        return status;
    }
    LSTATUS SettingsStore::save_json(const Snapshot &values)
    {
        const auto status = write_snapshot(file_, values);
        if (status != ERROR_SUCCESS)
            return status;
        loaded_ = false;
        return load_json();
    }
    LSTATUS SettingsStore::read(std::wstring_view group, std::wstring_view name, DWORD expected_type, void *data,
                                DWORD *bytes) noexcept
    {
        std::scoped_lock lock(mutex_);
        try
        {
            if (!bytes)
                return ERROR_INVALID_PARAMETER;
            const auto relative = relative_group(group);
            if (backend_ == Backend::registry)
            {
                HKEY raw{};
                auto status = RegOpenKeyExW(HKEY_CURRENT_USER,
                                            (registry_root_ + (relative.empty() ? L"" : L"\\" + relative)).c_str(), 0,
                                            KEY_QUERY_VALUE, &raw);
                Key key(raw);
                if (status != ERROR_SUCCESS)
                    return status;
                DWORD type{};
                status = RegQueryValueExW(key.get(), std::wstring(name).c_str(), nullptr, &type,
                                          static_cast<BYTE *>(data), bytes);
                if (status == ERROR_SUCCESS)
                {
                    if (type != expected_type)
                        return ERROR_UNSUPPORTED_TYPE;
                    if ((type == REG_DWORD && *bytes != sizeof(DWORD)) ||
                        (type == REG_QWORD && *bytes != sizeof(std::uint64_t)))
                        return ERROR_INVALID_DATA;
                }
                return status;
            }
            error_ = load_json();
            if (error_ != ERROR_SUCCESS)
                return error_;
            const auto found_group = cache_.find(relative);
            if (found_group == cache_.end())
                return ERROR_FILE_NOT_FOUND;
            const auto entry = found_group->second.find(std::wstring(name));
            if (entry == found_group->second.end())
                return ERROR_FILE_NOT_FOUND;
            const auto &value = entry->second;
            if (value.type != expected_type)
                return ERROR_UNSUPPORTED_TYPE;
            if ((value.type == REG_DWORD && value.bytes.size() != sizeof(DWORD)) ||
                (value.type == REG_QWORD && value.bytes.size() != sizeof(std::uint64_t)))
                return ERROR_INVALID_DATA;
            const auto capacity = *bytes;
            *bytes = static_cast<DWORD>(value.bytes.size());
            if (!data)
                return ERROR_SUCCESS;
            if (capacity < value.bytes.size())
                return ERROR_MORE_DATA;
            if (!value.bytes.empty())
                std::memcpy(data, value.bytes.data(), value.bytes.size());
            return ERROR_SUCCESS;
        }
        catch (...)
        {
            return error_ = caught_error();
        }
    }
    LSTATUS SettingsStore::apply(std::span<const Change> changes) noexcept
    {
        std::scoped_lock lock(mutex_);
        try
        {
            if (backend_ == Backend::registry)
            {
                for (const auto &change : changes)
                {
                    const auto group = relative_group(change.group);
                    HKEY raw{};
                    auto status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                                  (registry_root_ + (group.empty() ? L"" : L"\\" + group)).c_str(), 0,
                                                  nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr);
                    Key key(raw);
                    if (status != ERROR_SUCCESS)
                        return error_ = status;
                    status = change.value ? RegSetValueExW(key.get(), change.name.c_str(), 0, change.value->type,
                                                           change.value->bytes.data(),
                                                           static_cast<DWORD>(change.value->bytes.size()))
                                          : RegDeleteValueW(key.get(), change.name.c_str());
                    if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !change.value))
                        return error_ = status;
                }
                return error_ = ERROR_SUCCESS;
            }
            FileLock guard(file_);
            loaded_ = false;
            error_ = load_json();
            if (error_ != ERROR_SUCCESS)
                return error_;
            auto next = cache_;
            for (const auto &change : changes)
            {
                const auto group = relative_group(change.group);
                if (change.value)
                    next[group][change.name] = *change.value;
                else if (const auto found = next.find(group); found != next.end())
                {
                    found->second.erase(change.name);
                    if (found->second.empty())
                        next.erase(found);
                }
            }
            return error_ = save_json(next);
        }
        catch (...)
        {
            return error_ = caught_error();
        }
    }
    LSTATUS SettingsStore::clear(std::wstring_view group) noexcept
    {
        std::scoped_lock lock(mutex_);
        try
        {
            const auto relative = relative_group(group);
            if (backend_ == Backend::registry)
            {
                const auto status = RegDeleteTreeW(
                    HKEY_CURRENT_USER, (registry_root_ + (relative.empty() ? L"" : L"\\" + relative)).c_str());
                return error_ =
                           status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : status;
            }
            FileLock guard(file_);
            loaded_ = false;
            error_ = load_json();
            if (error_ != ERROR_SUCCESS)
                return error_;
            auto next = cache_;
            for (auto it = next.begin(); it != next.end();)
            {
                const auto prefix = it->first.substr(0, relative.size());
                const bool matches = !OrdinalLess{}(prefix, relative) && !OrdinalLess{}(relative, prefix);
                const bool descendant = it->first.size() > relative.size() && it->first[relative.size()] == L'\\';
                if (relative.empty() || (matches && (it->first.size() == relative.size() || descendant)))
                    it = next.erase(it);
                else
                    ++it;
            }
            return error_ = save_json(next);
        }
        catch (...)
        {
            return error_ = caught_error();
        }
    }
    LSTATUS SettingsStore::snapshot(Snapshot &result) noexcept
    {
        std::scoped_lock lock(mutex_);
        try
        {
            if (backend_ == Backend::json)
            {
                error_ = load_json();
                if (!error_)
                    result = cache_;
                return error_;
            }
            HKEY raw{};
            const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, registry_root_.c_str(), 0, KEY_READ, &raw);
            Key key(raw);
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
            {
                result.clear();
                return error_ = ERROR_SUCCESS;
            }
            if (status != ERROR_SUCCESS)
                return error_ = status;
            Snapshot next;
            error_ = registry_snapshot(key.get(), L"", next);
            if (!error_)
                result = std::move(next);
            return error_;
        }
        catch (...)
        {
            return error_ = caught_error();
        }
    }
    LSTATUS SettingsStore::error() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return error_;
    }
    void SettingsStore::invalidate() noexcept
    {
        std::scoped_lock lock(mutex_);
        loaded_ = false;
        stamp_.reset();
    }
    SettingsStore &settings()
    {
        static SettingsStore value(portable ? Backend::json : Backend::registry,
                                   portable ? data_directory() / L"settings.json" : std::filesystem::path{});
        return value;
    }
    LSTATUS read_value(std::wstring_view group, std::wstring_view name, DWORD type, void *data, DWORD *bytes) noexcept
    {
        try
        {
            return settings().read(group, name, type, data, bytes);
        }
        catch (...)
        {
            return caught_error();
        }
    }
    DWORD read_dword(std::wstring_view group, std::wstring_view name, DWORD fallback) noexcept
    {
        DWORD value{}, bytes = sizeof(value);
        return read_value(group, name, REG_DWORD, &value, &bytes) == ERROR_SUCCESS && bytes == sizeof(value) ? value
                                                                                                             : fallback;
    }
    std::wstring read_string(std::wstring_view group, std::wstring_view name, std::wstring_view fallback)
    {
        DWORD bytes{};
        if (read_value(group, name, REG_SZ, nullptr, &bytes) != ERROR_SUCCESS || bytes % sizeof(wchar_t))
            return std::wstring(fallback);
        std::vector<wchar_t> text(static_cast<std::size_t>(bytes) / sizeof(wchar_t) + 1);
        if (read_value(group, name, REG_SZ, text.data(), &bytes) != ERROR_SUCCESS)
            return std::wstring(fallback);
        return std::wstring(text.data());
    }
    Batch::Batch(std::wstring_view group, SettingsStore &store) noexcept : store_(store)
    {
        try
        {
            group_ = group;
        }
        catch (...)
        {
            error_ = caught_error();
        }
    }
    void Batch::set(std::wstring_view name, DWORD type, const void *data, DWORD bytes) noexcept
    {
        if (error_)
            return;
        if (bytes && !data)
        {
            error_ = ERROR_INVALID_PARAMETER;
            return;
        }
        try
        {
            const auto first = static_cast<const std::uint8_t *>(data);
            changes_.push_back(
                {group_, std::wstring(name),
                 Value{type, bytes ? std::vector<std::uint8_t>(first, first + bytes) : std::vector<std::uint8_t>{}}});
        }
        catch (...)
        {
            error_ = caught_error();
        }
    }
    void Batch::erase(std::wstring_view name) noexcept
    {
        if (!error_)
            try
            {
                changes_.push_back({group_, std::wstring(name), std::nullopt});
            }
            catch (...)
            {
                error_ = caught_error();
            }
    }
    LSTATUS Batch::commit() noexcept
    {
        if (error_)
            return error_;
        try
        {
            const auto status = store_.apply(changes_);
            if (!status)
                changes_.clear();
            return status;
        }
        catch (...)
        {
            return caught_error();
        }
    }
    LSTATUS delete_value(std::wstring_view group, std::wstring_view name) noexcept
    {
        try
        {
            Batch batch(group);
            batch.erase(name);
            return batch.commit();
        }
        catch (...)
        {
            return caught_error();
        }
    }
    LSTATUS clear_group(std::wstring_view group) noexcept
    {
        try
        {
            return settings().clear(group);
        }
        catch (...)
        {
            return caught_error();
        }
    }
} // namespace glance::contracts::storage
