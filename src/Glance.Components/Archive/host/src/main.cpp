#include "../../archive_protocol.h"

#include <windows.h>
#include <oleauto.h>
#include <propidl.h>

#include "CPP/Common/MyInitGuid.h"
#include "CPP/7zip/Archive/IArchive.h"
#include "CPP/7zip/IPassword.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace glance::components::archive;

    class UniqueHandle final
    {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE value) noexcept : value_(value)
        {
        }
        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        UniqueHandle(UniqueHandle&& other) noexcept
            : value_(std::exchange(other.value_, nullptr))
        {
        }
        ~UniqueHandle()
        {
            if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value_);
            }
        }
        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE value_{};
    };

    class UniqueModule final
    {
    public:
        explicit UniqueModule(HMODULE value) noexcept : value_(value)
        {
        }
        UniqueModule(const UniqueModule&) = delete;
        UniqueModule& operator=(const UniqueModule&) = delete;
        ~UniqueModule()
        {
            if (value_ != nullptr)
            {
                FreeLibrary(value_);
            }
        }
        [[nodiscard]] HMODULE get() const noexcept
        {
            return value_;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr;
        }

    private:
        HMODULE value_{};
    };

    template <typename Interface>
    class ComPtr final
    {
    public:
        ComPtr() = default;
        explicit ComPtr(Interface* value) noexcept : value_(value)
        {
        }
        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;
        ComPtr(ComPtr&& other) noexcept
            : value_(std::exchange(other.value_, nullptr))
        {
        }
        ComPtr& operator=(ComPtr&& other) noexcept
        {
            if (this != &other)
            {
                if (value_ != nullptr)
                {
                    value_->Release();
                }
                value_ = std::exchange(other.value_, nullptr);
            }
            return *this;
        }
        ~ComPtr()
        {
            if (value_ != nullptr)
            {
                value_->Release();
            }
        }
        [[nodiscard]] Interface* get() const noexcept
        {
            return value_;
        }
        [[nodiscard]] Interface** put() noexcept
        {
            if (value_ != nullptr)
            {
                value_->Release();
                value_ = nullptr;
            }
            return &value_;
        }
        [[nodiscard]] Interface* operator->() const noexcept
        {
            return value_;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr;
        }

    private:
        Interface* value_{};
    };

    class FileStream final : public IInStream
    {
    public:
        explicit FileStream(HANDLE file) noexcept : file_(file)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID interface_id,
            void** object) noexcept override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }
            *object = nullptr;
            if (interface_id == IID_IUnknown ||
                interface_id == IID_ISequentialInStream ||
                interface_id == IID_IInStream)
            {
                *object = static_cast<IInStream*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override
        {
            return references_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const ULONG remaining =
                references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
            {
                delete this;
            }
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE Read(
            void* data,
            UInt32 size,
            UInt32* processed_size) noexcept override
        {
            if (processed_size == nullptr)
            {
                return E_POINTER;
            }
            *processed_size = 0;
            DWORD read{};
            if (!ReadFile(file_, data, size, &read, nullptr))
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            *processed_size = read;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Seek(
            Int64 offset,
            UInt32 origin,
            UInt64* new_position) noexcept override
        {
            LARGE_INTEGER distance{};
            distance.QuadPart = offset;
            LARGE_INTEGER position{};
            if (!SetFilePointerEx(file_, distance, &position, origin))
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (new_position != nullptr)
            {
                *new_position = static_cast<UInt64>(position.QuadPart);
            }
            return S_OK;
        }

    private:
        std::atomic_ulong references_{ 1 };
        HANDLE file_{};
    };

    class OpenCallback final : public IArchiveOpenCallback, public ICryptoGetTextPassword
    {
    public:
        explicit OpenCallback(std::wstring password)
            : password_(std::move(password))
        {
        }

        [[nodiscard]] bool password_requested() const noexcept
        {
            return password_requested_;
        }

        [[nodiscard]] bool password_defined() const noexcept
        {
            return !password_.empty();
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID interface_id,
            void** object) noexcept override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }
            *object = nullptr;
            if (interface_id == IID_IUnknown || interface_id == IID_IArchiveOpenCallback)
            {
                *object = static_cast<IArchiveOpenCallback*>(this);
            }
            else if (interface_id == IID_ICryptoGetTextPassword)
            {
                *object = static_cast<ICryptoGetTextPassword*>(this);
            }
            else
            {
                return E_NOINTERFACE;
            }
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override
        {
            return references_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const ULONG remaining =
                references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0)
            {
                delete this;
            }
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE SetTotal(
            const UInt64*,
            const UInt64*) noexcept override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetCompleted(
            const UInt64*,
            const UInt64*) noexcept override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) noexcept override
        {
            if (password == nullptr)
            {
                return E_POINTER;
            }
            password_requested_ = true;
            if (password_.empty())
            {
                return E_ABORT;
            }
            *password = SysAllocStringLen(
                password_.data(),
                static_cast<UINT>(password_.size()));
            return *password == nullptr ? E_OUTOFMEMORY : S_OK;
        }

    private:
        std::atomic_ulong references_{ 1 };
        std::wstring password_;
        bool password_requested_{};
    };

    struct Format
    {
        GUID class_id{};
        std::wstring name;
        std::vector<std::wstring> extensions;
    };

    struct Node
    {
        std::uint64_t id{};
        std::uint64_t parent{};
        std::wstring name;
        std::wstring type;
        bool folder{};
        bool has_children{};
        bool modified_known{};
        bool packed_known{};
        bool size_known{};
        bool encrypted{};
        std::uint64_t modified{};
        std::uint64_t packed{};
        std::uint64_t size{};
    };

    struct Index
    {
        HostStatus status{ HostStatus::failed };
        std::wstring format_name;
        std::vector<Node> nodes;
        std::uint32_t flags{};
        std::uint64_t file_count{};
        std::uint64_t packed_size{};
        std::uint64_t original_size{};
    };

    using CreateObjectFunction = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
    using GetNumberOfFormatsFunction = HRESULT(WINAPI*)(UInt32*);
    using GetHandlerProperty2Function = HRESULT(WINAPI*)(UInt32, PROPID, PROPVARIANT*);

    bool read_exact(HANDLE input, void* data, std::size_t size)
    {
        auto* bytes = static_cast<std::byte*>(data);
        while (size != 0)
        {
            DWORD read{};
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(input, bytes, requested, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE output, const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(data);
        while (size != 0)
        {
            DWORD written{};
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(output, bytes, requested, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    bool read_string(HANDLE input, std::uint32_t characters, std::wstring& value)
    {
        if (characters > 32767)
        {
            return false;
        }
        value.resize(characters);
        return characters == 0 ||
            read_exact(input, value.data(), characters * sizeof(wchar_t));
    }

    std::wstring lower(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    std::vector<std::wstring> split_words(std::wstring_view value)
    {
        std::vector<std::wstring> words;
        std::size_t begin{};
        while (begin < value.size())
        {
            while (begin < value.size() && std::iswspace(value[begin]))
            {
                ++begin;
            }
            const auto end = value.find_first_of(L" \t", begin);
            if (begin < value.size())
            {
                words.push_back(lower(std::wstring(value.substr(
                    begin,
                    end == std::wstring_view::npos ? value.size() - begin : end - begin))));
            }
            if (end == std::wstring_view::npos)
            {
                break;
            }
            begin = end + 1;
        }
        return words;
    }

    std::wstring property_string(PROPVARIANT& value)
    {
        return value.vt == VT_BSTR && value.bstrVal != nullptr
            ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal))
            : std::wstring{};
    }

    std::vector<Format> load_formats(
        GetNumberOfFormatsFunction get_number,
        GetHandlerProperty2Function get_property)
    {
        std::vector<Format> formats;
        UInt32 count{};
        if (FAILED(get_number(&count)))
        {
            return formats;
        }
        formats.reserve(count);
        for (UInt32 index = 0; index < count; ++index)
        {
            PROPVARIANT class_id{};
            PROPVARIANT name{};
            PROPVARIANT extensions{};
            PropVariantInit(&class_id);
            PropVariantInit(&name);
            PropVariantInit(&extensions);
            const bool valid =
                SUCCEEDED(get_property(
                    index,
                    NArchive::NHandlerPropID::kClassID,
                    &class_id)) &&
                class_id.vt == VT_BSTR &&
                class_id.bstrVal != nullptr &&
                SysStringByteLen(class_id.bstrVal) == sizeof(GUID) &&
                SUCCEEDED(get_property(index, NArchive::NHandlerPropID::kName, &name)) &&
                SUCCEEDED(get_property(
                    index,
                    NArchive::NHandlerPropID::kExtension,
                    &extensions));
            if (valid)
            {
                Format format;
                std::memcpy(&format.class_id, class_id.bstrVal, sizeof(format.class_id));
                format.name = property_string(name);
                format.extensions = split_words(property_string(extensions));
                formats.push_back(std::move(format));
            }
            PropVariantClear(&extensions);
            PropVariantClear(&name);
            PropVariantClear(&class_id);
        }
        return formats;
    }

    bool get_property(IInArchive* archive, UInt32 index, PROPID id, PROPVARIANT& value)
    {
        PropVariantInit(&value);
        return SUCCEEDED(archive->GetProperty(index, id, &value));
    }

    bool property_bool(IInArchive* archive, UInt32 index, PROPID id, bool& value)
    {
        PROPVARIANT property{};
        if (!get_property(archive, index, id, property))
        {
            return false;
        }
        const bool known = property.vt == VT_BOOL;
        if (known)
        {
            value = property.boolVal != VARIANT_FALSE;
        }
        PropVariantClear(&property);
        return known;
    }

    bool property_uint64(IInArchive* archive, UInt32 index, PROPID id, std::uint64_t& value)
    {
        PROPVARIANT property{};
        if (!get_property(archive, index, id, property))
        {
            return false;
        }
        bool known = true;
        switch (property.vt)
        {
        case VT_UI1:
            value = property.bVal;
            break;
        case VT_UI2:
            value = property.uiVal;
            break;
        case VT_UI4:
            value = property.ulVal;
            break;
        case VT_UI8:
            value = property.uhVal.QuadPart;
            break;
        default:
            known = false;
            break;
        }
        PropVariantClear(&property);
        return known;
    }

    bool property_time(IInArchive* archive, UInt32 index, PROPID id, std::uint64_t& value)
    {
        PROPVARIANT property{};
        if (!get_property(archive, index, id, property))
        {
            return false;
        }
        const bool known = property.vt == VT_FILETIME;
        if (known)
        {
            value = static_cast<std::uint64_t>(property.filetime.dwHighDateTime) << 32U |
                property.filetime.dwLowDateTime;
        }
        PropVariantClear(&property);
        return known;
    }

    std::wstring item_path(IInArchive* archive, UInt32 index)
    {
        PROPVARIANT property{};
        if (!get_property(archive, index, kpidPath, property))
        {
            return {};
        }
        auto path = property_string(property);
        PropVariantClear(&property);
        if (path.empty())
        {
            path = L"[Content]";
        }
        std::ranges::replace(path, L'/', L'\\');
        return path;
    }

    std::vector<std::wstring> split_path(std::wstring_view path)
    {
        std::vector<std::wstring> parts;
        std::size_t begin{};
        while (begin < path.size())
        {
            const auto end = path.find(L'\\', begin);
            const auto part = path.substr(
                begin,
                end == std::wstring_view::npos ? path.size() - begin : end - begin);
            if (!part.empty() && part != L".")
            {
                parts.emplace_back(part.substr(0, 1024));
            }
            if (end == std::wstring_view::npos)
            {
                break;
            }
            begin = end + 1;
        }
        return parts;
    }

    std::wstring type_from_name(std::wstring_view name)
    {
        const auto extension = std::filesystem::path(name).extension().wstring();
        if (extension.size() <= 1)
        {
            return {};
        }
        std::wstring type(extension.substr(1));
        std::ranges::transform(type, type.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towupper(character));
        });
        return type;
    }

    std::uint64_t ensure_folder(
        std::vector<Node>& nodes,
        std::map<std::wstring, std::uint64_t, std::less<>>& folders,
        std::wstring key,
        std::uint64_t parent,
        std::wstring name,
        bool& truncated)
    {
        if (const auto existing = folders.find(key); existing != folders.end())
        {
            return existing->second;
        }
        if (nodes.size() >= maximum_entries)
        {
            truncated = true;
            return 0;
        }
        const auto id = static_cast<std::uint64_t>(nodes.size()) + 1;
        nodes.push_back(Node{
            .id = id,
            .parent = parent,
            .name = std::move(name),
            .folder = true });
        folders.emplace(std::move(key), id);
        return id;
    }

    Index read_index(
        const std::filesystem::path& source,
        std::wstring password,
        const std::filesystem::path& library_path)
    {
        Index result;
        UniqueModule library(LoadLibraryExW(
            library_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (!library)
        {
            return result;
        }
        const auto create_object = reinterpret_cast<CreateObjectFunction>(
            GetProcAddress(library.get(), "CreateObject"));
        const auto get_number = reinterpret_cast<GetNumberOfFormatsFunction>(
            GetProcAddress(library.get(), "GetNumberOfFormats"));
        const auto get_handler_property = reinterpret_cast<GetHandlerProperty2Function>(
            GetProcAddress(library.get(), "GetHandlerProperty2"));
        if (create_object == nullptr || get_number == nullptr ||
            get_handler_property == nullptr)
        {
            return result;
        }

        UniqueHandle file(CreateFileW(
            source.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr));
        if (!file)
        {
            result.status = HostStatus::unavailable;
            return result;
        }
        LARGE_INTEGER physical_size{};
        if (GetFileSizeEx(file.get(), &physical_size) && physical_size.QuadPart >= 0)
        {
            result.packed_size = static_cast<std::uint64_t>(physical_size.QuadPart);
        }

        auto extension = lower(source.extension().wstring());
        if (!extension.empty() && extension.front() == L'.')
        {
            extension.erase(extension.begin());
        }
        if (extension == L"tgz")
        {
            extension = L"gz";
        }
        else if (extension == L"tbz" || extension == L"tbz2")
        {
            extension = L"bz2";
        }
        else if (extension == L"txz")
        {
            extension = L"xz";
        }

        const auto formats = load_formats(get_number, get_handler_property);
        ComPtr<IInArchive> archive;
        ComPtr<FileStream> stream(new FileStream(file.get()));
        ComPtr<OpenCallback> callback(new OpenCallback(std::move(password)));
        for (const auto& format : formats)
        {
            if (std::ranges::find(format.extensions, extension) == format.extensions.end())
            {
                continue;
            }
            ComPtr<IInArchive> candidate;
            if (FAILED(create_object(
                    &format.class_id,
                    &IID_IInArchive,
                    reinterpret_cast<void**>(candidate.put()))))
            {
                continue;
            }
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN))
            {
                continue;
            }
            constexpr UInt64 scan_size = 1U << 23U;
            if (candidate->Open(stream.get(), &scan_size, callback.get()) == S_OK)
            {
                result.format_name = format.name;
                archive = std::move(candidate);
                break;
            }
            candidate->Close();
        }
        if (!archive)
        {
            result.status = callback->password_requested()
                ? callback->password_defined()
                    ? HostStatus::invalid_password
                    : HostStatus::password_required
                : HostStatus::unavailable;
            return result;
        }

        UInt32 count{};
        if (FAILED(archive->GetNumberOfItems(&count)))
        {
            archive->Close();
            return result;
        }
        std::map<std::wstring, std::uint64_t, std::less<>> folders;
        bool truncated{};
        for (UInt32 index = 0; index < count; ++index)
        {
            bool is_folder{};
            static_cast<void>(property_bool(archive.get(), index, kpidIsDir, is_folder));
            std::uint64_t size{};
            const bool size_known = property_uint64(archive.get(), index, kpidSize, size);
            std::uint64_t packed{};
            const bool packed_known = property_uint64(
                archive.get(), index, kpidPackSize, packed);
            std::uint64_t modified{};
            const bool modified_known = property_time(
                archive.get(), index, kpidMTime, modified);
            bool encrypted{};
            static_cast<void>(property_bool(
                archive.get(), index, kpidEncrypted, encrypted));
            if (!is_folder)
            {
                ++result.file_count;
                if (size_known &&
                    result.original_size <=
                        std::numeric_limits<std::uint64_t>::max() - size)
                {
                    result.original_size += size;
                }
            }
            if (modified_known)
            {
                result.flags |= response_has_modified_time;
            }
            if (packed_known)
            {
                result.flags |= response_has_packed_size;
            }
            if (size_known)
            {
                result.flags |= response_has_original_size;
            }
            if (encrypted)
            {
                result.flags |= response_has_encrypted_items;
            }

            auto parts = split_path(item_path(archive.get(), index));
            if (parts.empty())
            {
                continue;
            }
            const bool depth_limited = parts.size() > maximum_depth;
            if (depth_limited)
            {
                result.flags |= response_depth_limited;
                result.flags |= response_truncated;
                parts.resize(maximum_depth);
            }
            std::uint64_t parent{};
            std::wstring key;
            bool insertion_failed{};
            for (std::size_t part = 0; part + 1 < parts.size(); ++part)
            {
                if (!key.empty())
                {
                    key.push_back(L'\\');
                }
                key += parts[part];
                parent = ensure_folder(
                    result.nodes,
                    folders,
                    key,
                    parent,
                    parts[part],
                    truncated);
                if (parent == 0)
                {
                    insertion_failed = true;
                    break;
                }
            }
            if (insertion_failed || depth_limited)
            {
                continue;
            }
            if (!key.empty())
            {
                key.push_back(L'\\');
            }
            key += parts.back();
            if (is_folder)
            {
                const auto id = ensure_folder(
                    result.nodes,
                    folders,
                    key,
                    parent,
                    parts.back(),
                    truncated);
                if (id != 0)
                {
                    auto& node = result.nodes[static_cast<std::size_t>(id - 1)];
                    node.modified_known = modified_known;
                    node.modified = modified;
                    node.encrypted = encrypted;
                }
            }
            else if (result.nodes.size() < maximum_entries)
            {
                const auto id = static_cast<std::uint64_t>(result.nodes.size()) + 1;
                result.nodes.push_back(Node{
                    .id = id,
                    .parent = parent,
                    .name = parts.back(),
                    .type = type_from_name(parts.back()),
                    .folder = false,
                    .modified_known = modified_known,
                    .packed_known = packed_known,
                    .size_known = size_known,
                    .encrypted = encrypted,
                    .modified = modified,
                    .packed = packed,
                    .size = size });
            }
            else
            {
                truncated = true;
            }
        }
        archive->Close();
        if (truncated)
        {
            result.flags |= response_truncated;
        }
        for (const auto& node : result.nodes)
        {
            if (node.parent != 0 && node.parent <= result.nodes.size())
            {
                result.nodes[static_cast<std::size_t>(node.parent - 1)].has_children = true;
            }
        }
        result.status = HostStatus::ready;
        return result;
    }

    bool write_response(HANDLE output, const Index& index)
    {
        ResponseHeader header{
            .status = index.status,
            .flags = index.flags,
            .format_name_characters = static_cast<std::uint32_t>(index.format_name.size()),
            .entry_count = static_cast<std::uint32_t>(index.nodes.size()),
            .file_count = index.file_count,
            .packed_size = index.packed_size,
            .original_size = index.original_size };
        if (!write_exact(output, &header, sizeof(header)) ||
            (!index.format_name.empty() &&
             !write_exact(
                 output,
                 index.format_name.data(),
                 index.format_name.size() * sizeof(wchar_t))))
        {
            return false;
        }
        for (const auto& node : index.nodes)
        {
            std::uint32_t flags = node.folder ? entry_is_folder : 0U;
            flags |= node.has_children ? entry_has_children : 0U;
            flags |= node.modified_known ? entry_has_modified_time : 0U;
            flags |= node.packed_known ? entry_has_packed_size : 0U;
            flags |= node.size_known ? entry_has_original_size : 0U;
            flags |= node.encrypted ? entry_is_encrypted : 0U;
            const EntryHeader entry{
                .node_id = node.id,
                .parent_id = node.parent,
                .flags = flags,
                .name_characters = static_cast<std::uint32_t>(node.name.size()),
                .type_characters = static_cast<std::uint32_t>(node.type.size()),
                .modified_time = node.modified,
                .packed_size = node.packed,
                .original_size = node.size };
            if (!write_exact(output, &entry, sizeof(entry)) ||
                (!node.name.empty() &&
                 !write_exact(
                     output,
                     node.name.data(),
                     node.name.size() * sizeof(wchar_t))) ||
                (!node.type.empty() &&
                 !write_exact(
                     output,
                     node.type.data(),
                     node.type.size() * sizeof(wchar_t))))
            {
                return false;
            }
        }
        return true;
    }

    HANDLE parse_handle(const wchar_t* value)
    {
        wchar_t* end{};
        const auto number = _wcstoui64(value, &end, 10);
        return end != value && *end == L'\0'
            ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number))
            : nullptr;
    }
}

int wmain(int argument_count, wchar_t** arguments)
{
    if (argument_count != 3)
    {
        return ERROR_INVALID_PARAMETER;
    }
    UniqueHandle input(parse_handle(arguments[1]));
    UniqueHandle output(parse_handle(arguments[2]));
    if (!input || !output)
    {
        return ERROR_INVALID_HANDLE;
    }
    RequestHeader request;
    std::wstring path;
    std::wstring password;
    if (!read_exact(input.get(), &request, sizeof(request)) ||
        request.magic != request_magic ||
        request.version != protocol_version ||
        !read_string(input.get(), request.path_characters, path) ||
        !read_string(input.get(), request.password_characters, password))
    {
        return ERROR_INVALID_DATA;
    }
    wchar_t module_path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module_path, std::size(module_path));
    if (length == 0 || length >= std::size(module_path))
    {
        return ERROR_PATH_NOT_FOUND;
    }
    const auto library =
        std::filesystem::path(module_path).parent_path() / L"7z.dll";
    const auto index = read_index(path, std::move(password), library);
    return write_response(output.get(), index) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
