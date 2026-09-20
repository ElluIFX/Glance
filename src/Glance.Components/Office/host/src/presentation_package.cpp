#include "../include/presentation_package.h"
#include <msopc.h>
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace
{
    constexpr ULONGLONG projection_limit = 128ULL * 1024 * 1024;
    struct Apartment
    {
        HRESULT result{ CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
        Apartment() { if (result != RPC_E_CHANGED_MODE) winrt::check_hresult(result); }
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    };

    struct Source
    {
        std::wstring path;
        BY_HANDLE_FILE_INFORMATION identity{};
        std::atomic_bool stopped{};
        explicit Source(std::wstring value) : path(std::move(value)) {}
        void initialize()
        {
            auto file = open();
            winrt::check_bool(GetFileInformationByHandle(file.get(), &identity));
        }
        winrt::handle open() const
        {
            if (stopped.load()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            winrt::handle file(CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE) winrt::throw_last_error();
            return file;
        }
        ULONGLONG size() const { return (static_cast<ULONGLONG>(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow; }
        void verify(HANDLE file) const
        {
            if (stopped.load()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            BY_HANDLE_FILE_INFORMATION now{};
            winrt::check_bool(GetFileInformationByHandle(file, &now));
            if (now.dwVolumeSerialNumber != identity.dwVolumeSerialNumber ||
                now.nFileIndexHigh != identity.nFileIndexHigh || now.nFileIndexLow != identity.nFileIndexLow ||
                now.nFileSizeHigh != identity.nFileSizeHigh || now.nFileSizeLow != identity.nFileSizeLow ||
                CompareFileTime(&now.ftLastWriteTime, &identity.ftLastWriteTime) != 0)
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        }
    };

    template<typename Derived>
    struct ReadOnlyStream : winrt::implements<Derived, IStream>
    {
        HRESULT __stdcall Write(const void*, ULONG, ULONG*) noexcept override { return STG_E_ACCESSDENIED; }
        HRESULT __stdcall SetSize(ULARGE_INTEGER) noexcept override { return STG_E_ACCESSDENIED; }
        HRESULT __stdcall CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) noexcept override { return E_NOTIMPL; }
        HRESULT __stdcall Commit(DWORD) noexcept override { return S_OK; }
        HRESULT __stdcall Revert() noexcept override { return STG_E_ACCESSDENIED; }
        HRESULT __stdcall LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
        HRESULT __stdcall UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    };

    struct FileLease
    {
        winrt::handle file;
        std::mutex mutex;
        explicit FileLease(const std::shared_ptr<Source>& source) : file(source->open()) { source->verify(file.get()); }
    };

    // Projection shares a short-lived handle; playback clones reopen the file for each read.
    struct FileStream : ReadOnlyStream<FileStream>
    {
        std::shared_ptr<Source> source;
        ULONGLONG position{};
        std::mutex mutex;
        std::shared_ptr<FileLease> lease;
        explicit FileStream(std::shared_ptr<Source> value, ULONGLONG offset = 0, std::shared_ptr<FileLease> shared = {})
            : source(std::move(value)), position(offset), lease(std::move(shared)) {}
        HRESULT __stdcall Read(void* bytes, ULONG count, ULONG* actual) noexcept override
        {
            if (actual) *actual = 0;
            if (count && !bytes) return STG_E_INVALIDPOINTER;
            try
            {
                std::lock_guard lock(mutex);
                if (source->stopped.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                std::unique_lock<std::mutex> shared_lock;
                winrt::handle reopened;
                if (lease) shared_lock = std::unique_lock(lease->mutex);
                else { reopened = source->open(); source->verify(reopened.get()); }
                const auto file = lease ? lease->file.get() : reopened.get();
                LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(position);
                winrt::check_bool(SetFilePointerEx(file, offset, nullptr, FILE_BEGIN));
                DWORD read{}; winrt::check_bool(ReadFile(file, bytes, count, &read, nullptr));
                if (!lease) source->verify(file);
                position += read;
                if (actual) *actual = read;
                return read == count ? S_OK : S_FALSE;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        HRESULT __stdcall Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* result) noexcept override
        {
            std::lock_guard lock(mutex);
            if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
            const ULONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position : source->size();
            if (offset.QuadPart < 0 && static_cast<ULONGLONG>(-(offset.QuadPart + 1)) + 1 > base) return STG_E_INVALIDFUNCTION;
            if (offset.QuadPart >= 0 && static_cast<ULONGLONG>(offset.QuadPart) > static_cast<ULONGLONG>(LLONG_MAX) - base) return STG_E_INVALIDFUNCTION;
            position = base + offset.QuadPart;
            if (result) result->QuadPart = position;
            return S_OK;
        }
        HRESULT __stdcall Stat(STATSTG* value, DWORD) noexcept override
        {
            if (!value) return STG_E_INVALIDPOINTER;
            *value = {}; value->type = STGTY_STREAM; value->cbSize.QuadPart = source->size(); value->grfMode = STGM_READ;
            return S_OK;
        }
        HRESULT __stdcall Clone(IStream** result) noexcept override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            try { std::lock_guard lock(mutex); winrt::make<FileStream>(source, position, lease).as<IStream>().copy_to(result); return S_OK; }
            catch (...) { return winrt::to_hresult(); }
        }
    };

    struct Package
    {
        winrt::com_ptr<IOpcFactory> factory;
        winrt::com_ptr<IOpcPackage> package;
        winrt::com_ptr<IOpcPartSet> parts;
        explicit Package(const std::shared_ptr<Source>& source, const std::shared_ptr<FileLease>& lease = {})
        {
            factory = winrt::create_instance<IOpcFactory>(__uuidof(OpcFactory));
            auto stream = winrt::make<FileStream>(source, 0, lease).as<IStream>();
            winrt::check_hresult(factory->ReadPackageFromStream(stream.get(), OPC_READ_DEFAULT, package.put()));
            winrt::check_hresult(package->GetPartSet(parts.put()));
        }
    };

    std::wstring uri_name(IOpcPartUri* uri)
    {
        BSTR name{}; winrt::check_hresult(uri->GetAbsoluteUri(&name));
        std::wstring result(name, SysStringLen(name)); SysFreeString(name); return result;
    }

    // WebView consumes the response on background threads. Serialize OPC access and retain no file handle.
    struct MediaStream : ReadOnlyStream<MediaStream>
    {
        std::shared_ptr<Source> source;
        Package owner;
        winrt::com_ptr<IStream> content;
        ULONGLONG begin{}, length{}, position{};
        std::mutex mutex;
        MediaStream(std::shared_ptr<Source> value, const std::wstring& name, ULONGLONG start, ULONGLONG size)
            : source(std::move(value)), owner(source), begin(start), length(size)
        {
            winrt::com_ptr<IOpcPartUri> uri; winrt::check_hresult(owner.factory->CreatePartUri(name.c_str(), uri.put()));
            winrt::com_ptr<IOpcPart> part; winrt::check_hresult(owner.parts->GetPart(uri.get(), part.put()));
            winrt::check_hresult(part->GetContentStream(content.put()));
            LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(begin);
            winrt::check_hresult(content->Seek(offset, STREAM_SEEK_SET, nullptr));
        }
        HRESULT __stdcall Read(void* bytes, ULONG count, ULONG* actual) noexcept override
        {
            if (actual) *actual = 0;
            if (count && !bytes) return STG_E_INVALIDPOINTER;
            try
            {
                Apartment apartment;
                std::lock_guard lock(mutex);
                auto file = source->open(); source->verify(file.get()); file.close();
                ULONG total{};
                while (total < count && position < length)
                {
                    if (source->stopped.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    const auto chunk = static_cast<ULONG>(std::min<ULONGLONG>({ count - total, length - position, 256 * 1024 }));
                    ULONG read{}; winrt::check_hresult(content->Read(static_cast<BYTE*>(bytes) + total, chunk, &read));
                    total += read; position += read;
                    if (!read) break;
                }
                if (actual) *actual = total;
                return total == count ? S_OK : S_FALSE;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        HRESULT __stdcall Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* result) noexcept override
        {
            try
            {
                Apartment apartment; std::lock_guard lock(mutex);
                if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
                const auto base = static_cast<LONGLONG>(origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position : length);
                if (offset.QuadPart < -base || offset.QuadPart > static_cast<LONGLONG>(length) - base) return STG_E_INVALIDFUNCTION;
                const auto next = static_cast<ULONGLONG>(base + offset.QuadPart);
                LARGE_INTEGER absolute{}; absolute.QuadPart = static_cast<LONGLONG>(begin + next);
                winrt::check_hresult(content->Seek(absolute, STREAM_SEEK_SET, nullptr)); position = next;
                if (result) result->QuadPart = position;
                return S_OK;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        HRESULT __stdcall Stat(STATSTG* value, DWORD) noexcept override
        {
            if (!value) return STG_E_INVALIDPOINTER;
            *value = {}; value->type = STGTY_STREAM; value->cbSize.QuadPart = length; value->grfMode = STGM_READ; return S_OK;
        }
        HRESULT __stdcall Clone(IStream**) noexcept override { return E_NOTIMPL; }
    };
}

namespace glance::office
{
    struct PresentationPackage::State
    {
        struct Media { std::wstring name, type; ULONGLONG size{}; };
        std::shared_ptr<Source> source;
        std::vector<Media> media;
    };

    PresentationPackage::PresentationPackage(std::wstring path) : state_(std::make_shared<State>())
    {
        state_->source = std::make_shared<Source>(std::move(path));
    }
    void PresentationPackage::cancel() noexcept { state_->source->stopped.store(true); }

    std::vector<BYTE> PresentationPackage::project()
    {
        Apartment apartment;
        state_->source->initialize();
        auto lease = std::make_shared<FileLease>(state_->source);
        Package owner(state_->source, lease);
        std::vector<winrt::com_ptr<IOpcPart>> parts;
        winrt::com_ptr<IOpcPartEnumerator> enumerator; winrt::check_hresult(owner.parts->GetEnumerator(enumerator.put()));
        BOOL has{}; ULONGLONG total{};
        for (;;)
        {
            winrt::check_hresult(enumerator->MoveNext(&has)); if (!has) break;
            if (parts.size() >= 4000) throw std::runtime_error("Too many presentation parts");
            winrt::com_ptr<IOpcPart> part; winrt::check_hresult(enumerator->GetCurrent(part.put()));
            winrt::com_ptr<IOpcPartUri> uri; winrt::check_hresult(part->GetName(uri.put()));
            winrt::com_ptr<IStream> stream; winrt::check_hresult(part->GetContentStream(stream.put()));
            STATSTG info{}; winrt::check_hresult(stream->Stat(&info, STATFLAG_NONAME));
            LPWSTR raw{}; winrt::check_hresult(part->GetContentType(&raw));
            std::wstring type(raw); CoTaskMemFree(raw);
            if (type.starts_with(L"video/") || type.starts_with(L"audio/"))
            {
                if (info.cbSize.QuadPart > 2ULL * 1024 * 1024 * 1024) throw std::runtime_error("Media exceeds preview budget");
                state_->media.push_back({ uri_name(uri.get()), type, info.cbSize.QuadPart });
            }
            else
            {
                total += info.cbSize.QuadPart;
                if (info.cbSize.QuadPart > 32 * 1024 * 1024 || total > projection_limit) throw std::runtime_error("Presentation exceeds preview budget");
            }
            parts.push_back(std::move(part));
        }
        enumerator = nullptr;
        for (const auto& part : parts)
        {
            winrt::com_ptr<IOpcRelationshipSet> relationships; winrt::check_hresult(part->GetRelationshipSet(relationships.put()));
            winrt::com_ptr<IOpcRelationshipEnumerator> items; winrt::check_hresult(relationships->GetEnumerator(items.put()));
            struct Change { std::wstring id, type, target; };
            std::vector<Change> changes;
            for (;;)
            {
                winrt::check_hresult(items->MoveNext(&has)); if (!has) break;
                winrt::com_ptr<IOpcRelationship> relationship; winrt::check_hresult(items->GetCurrent(relationship.put()));
                OPC_URI_TARGET_MODE mode{}; winrt::check_hresult(relationship->GetTargetMode(&mode));
                if (mode != OPC_URI_TARGET_MODE_INTERNAL) continue;
                winrt::com_ptr<IOpcUri> base; winrt::check_hresult(relationship->GetSourceUri(base.put()));
                winrt::com_ptr<IUri> target; winrt::check_hresult(relationship->GetTargetUri(target.put()));
                winrt::com_ptr<IOpcPartUri> absolute; winrt::check_hresult(base->CombinePartUri(target.get(), absolute.put()));
                const auto name = uri_name(absolute.get());
                for (std::size_t index = 0; index < state_->media.size(); ++index)
                    if (state_->media[index].name == name)
                    {
                        LPWSTR id{}, type{}; winrt::check_hresult(relationship->GetId(&id));
                        winrt::check_hresult(relationship->GetRelationshipType(&type));
                        changes.push_back({ id, type, L"https://office-media.glance.invalid/" + std::to_wstring(index) });
                        CoTaskMemFree(id); CoTaskMemFree(type); break;
                    }
            }
            items = nullptr;
            for (const auto& change : changes)
            {
                winrt::check_hresult(relationships->DeleteRelationship(change.id.c_str()));
                winrt::com_ptr<IUri> uri; winrt::check_hresult(CreateUri(change.target.c_str(), Uri_CREATE_CANONICALIZE, 0, uri.put()));
                winrt::com_ptr<IOpcRelationship> relationship;
                winrt::check_hresult(relationships->CreateRelationship(change.id.c_str(), change.type.c_str(), uri.get(), OPC_URI_TARGET_MODE_EXTERNAL, relationship.put()));
            }
        }
        for (const auto& media : state_->media)
        {
            winrt::com_ptr<IOpcPartUri> uri; winrt::check_hresult(owner.factory->CreatePartUri(media.name.c_str(), uri.put()));
            winrt::check_hresult(owner.parts->DeletePart(uri.get()));
        }
        winrt::com_ptr<IStream> output; output.attach(SHCreateMemStream(nullptr, 0));
        if (!output) throw std::bad_alloc();
        winrt::check_hresult(owner.factory->WritePackageToStream(owner.package.get(), OPC_WRITE_DEFAULT, output.get()));
        STATSTG info{}; winrt::check_hresult(output->Stat(&info, STATFLAG_NONAME));
        if (info.cbSize.QuadPart > projection_limit) throw std::runtime_error("Projected presentation exceeds budget");
        std::vector<BYTE> bytes(static_cast<std::size_t>(info.cbSize.QuadPart));
        LARGE_INTEGER zero{}; winrt::check_hresult(output->Seek(zero, STREAM_SEEK_SET, nullptr));
        ULONG count{}; winrt::check_hresult(output->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &count));
        if (count != bytes.size()) throw std::runtime_error("Incomplete projected presentation");
        state_->source->verify(lease->file.get());
        return bytes;
    }

    PresentationMediaResponse PresentationPackage::media(std::size_t index, const std::wstring& range, bool head)
    {
        PresentationMediaResponse response;
        if (index >= state_->media.size()) return response;
        const auto& item = state_->media[index];
        ULONGLONG start{}, end = item.size ? item.size - 1 : 0;
        if (!range.empty())
        {
            response.status = 416;
            response.headers = L"Content-Range: bytes */" + std::to_wstring(item.size);
            if (!range.starts_with(L"bytes=") || item.size == 0) return response;
            const auto value = range.substr(6);
            const auto separator = value.find(L'-');
            if (separator == std::wstring::npos || value.find(L'-', separator + 1) != std::wstring::npos ||
                value.find_first_not_of(L"0123456789-") != std::wstring::npos) return response;
            try
            {
                if (separator == 0)
                {
                    const auto suffix = std::stoull(value.substr(1)); if (!suffix) return response;
                    start = item.size - std::min(item.size, suffix);
                }
                else
                {
                    start = std::stoull(value.substr(0, separator));
                    if (separator + 1 < value.size()) end = std::min(end, std::stoull(value.substr(separator + 1)));
                }
            }
            catch (...) { return response; }
            if (start >= item.size || end < start) return response;
        }
        const auto length = item.size ? end - start + 1 : 0;
        response.status = range.empty() ? 200 : 206;
        response.headers = L"Content-Type: " + item.type + L"\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nContent-Length: " + std::to_wstring(length);
        if (!range.empty()) response.headers += L"\r\nContent-Range: bytes " + std::to_wstring(start) + L"-" + std::to_wstring(end) + L"/" + std::to_wstring(item.size);
        if (!head)
        {
            Apartment apartment;
            response.stream = winrt::make<MediaStream>(state_->source, item.name, start, length).as<IStream>();
        }
        return response;
    }
}
