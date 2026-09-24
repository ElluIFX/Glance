#include "pe_reader.h"
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>

namespace glance::executable
{
    namespace
    {
        constexpr std::size_t maximum_rows = 100000;
        struct Handle
        {
            HANDLE value{INVALID_HANDLE_VALUE};
            ~Handle()
            {
                if (value != INVALID_HANDLE_VALUE && value)
                    CloseHandle(value);
            }
        };
        class Reader
        {
          public:
            Reader(const std::wstring& path, Cancelled cancelled = {}) : cancelled_(std::move(cancelled))
            {
                handle_.value = CreateFileW(path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle_.value == INVALID_HANDLE_VALUE)
                    throw std::runtime_error("Error");
                identity = current_identity();
            }
            Identity current_identity() const
            {
                BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(handle_.value, &info))
                    throw std::runtime_error("Error");
                return {(std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                            info.ftLastWriteTime.dwLowDateTime,
                        (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
                        info.dwVolumeSerialNumber};
            }
            void check() const
            {
                if (cancelled_ && cancelled_())
                    throw std::runtime_error("Cancelled");
                if (GetTickCount64() - started_ > 10000)
                    throw std::runtime_error("Partial");
            }
            void verify(const Identity& expected) const
            {
                if (current_identity() != expected)
                    throw std::runtime_error("Changed");
            }
            void read(std::uint64_t offset, void* destination, std::size_t size)
            {
                check();
                if (offset > identity.size || size > identity.size - offset || size > MAXDWORD)
                    throw std::runtime_error("Invalid");
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                DWORD received{};
                if (!SetFilePointerEx(handle_.value, position, nullptr, FILE_BEGIN) ||
                    !ReadFile(handle_.value, destination, static_cast<DWORD>(size), &received, nullptr) ||
                    received != size)
                    throw std::runtime_error("Error");
            }
            template <class T> T at(std::uint64_t offset)
            {
                T result{};
                read(offset, &result, sizeof(result));
                return result;
            }
            std::vector<std::byte> bytes(std::uint64_t offset, std::size_t length)
            {
                if (length > 32 * 1024 * 1024)
                    throw std::runtime_error("Partial");
                std::vector<std::byte> result(length);
                read(offset, result.data(), length);
                return result;
            }
            std::wstring ascii(std::uint64_t offset, std::size_t limit = 4096)
            {
                if (offset >= identity.size)
                    throw std::runtime_error("Invalid");
                const auto data = bytes(
                    offset, static_cast<std::size_t>(std::min<std::uint64_t>(limit, identity.size - offset)));
                std::wstring result;
                for (auto c : data)
                {
                    const auto value = std::to_integer<unsigned char>(c);
                    if (!value)
                        return result;
                    result += static_cast<wchar_t>(value);
                }
                throw std::runtime_error("Invalid");
            }
            void renew_budget()
            {
                started_ = GetTickCount64();
            }
            Identity identity;

          private:
            Handle handle_;
            Cancelled cancelled_;
            ULONGLONG started_{GetTickCount64()};
        };
        class Pe
        {
          public:
            explicit Pe(Reader& source) : file(source)
            {
                const auto dos = file.at<IMAGE_DOS_HEADER>(0);
                if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
                    throw std::runtime_error("Invalid");
                const auto nt = static_cast<std::uint64_t>(dos.e_lfanew);
                if (file.at<DWORD>(nt) != IMAGE_NT_SIGNATURE)
                    throw std::runtime_error("Invalid");
                header = file.at<IMAGE_FILE_HEADER>(nt + 4);
                if (header.NumberOfSections > 4096)
                    throw std::runtime_error("Partial");
                const auto offset = nt + 4 + sizeof(header);
                const auto magic = file.at<WORD>(offset);
                if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                {
                    if (header.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory))
                        throw std::runtime_error("Invalid");
                    IMAGE_OPTIONAL_HEADER64 optional{};
                    file.read(offset, &optional,
                              std::min<std::size_t>(sizeof(optional), header.SizeOfOptionalHeader));
                    is64 = true;
                    init(optional,
                         header.SizeOfOptionalHeader - offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory));
                }
                else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
                {
                    if (header.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory))
                        throw std::runtime_error("Invalid");
                    IMAGE_OPTIONAL_HEADER32 optional{};
                    file.read(offset, &optional,
                              std::min<std::size_t>(sizeof(optional), header.SizeOfOptionalHeader));
                    init(optional,
                         header.SizeOfOptionalHeader - offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory));
                }
                else
                    throw std::runtime_error("Invalid");
                for (unsigned i = 0; i < header.NumberOfSections; ++i)
                    sections.push_back(file.at<IMAGE_SECTION_HEADER>(offset + header.SizeOfOptionalHeader +
                                                                     i * sizeof(IMAGE_SECTION_HEADER)));
            }
            template <class T> void init(const T& optional, std::size_t available)
            {
                base = optional.ImageBase;
                entry = optional.AddressOfEntryPoint;
                headers = optional.SizeOfHeaders;
                subsystem = optional.Subsystem;
                flags = optional.DllCharacteristics;
                file_alignment = optional.FileAlignment;
                section_alignment = optional.SectionAlignment;
                linker = std::to_wstring(optional.MajorLinkerVersion) + L"." +
                         std::to_wstring(optional.MinorLinkerVersion);
                const auto count = std::min({std::size_t(optional.NumberOfRvaAndSizes), directories.size(),
                                             available / sizeof(IMAGE_DATA_DIRECTORY)});
                std::copy_n(optional.DataDirectory, count, directories.begin());
            }
            std::uint64_t offset(std::uint64_t rva, std::uint64_t length = 1) const
            {
                if (rva < headers && length <= headers - rva && rva <= file.identity.size &&
                    length <= file.identity.size - rva)
                    return rva;
                for (const auto& section : sections)
                {
                    if (rva >= section.VirtualAddress &&
                        rva - section.VirtualAddress <= section.SizeOfRawData &&
                        length <= section.SizeOfRawData - (rva - section.VirtualAddress))
                    {
                        const auto result =
                            std::uint64_t(section.PointerToRawData) + rva - section.VirtualAddress;
                        if (result <= file.identity.size && length <= file.identity.size - result)
                            return result;
                    }
                }
                throw std::runtime_error("Invalid");
            }
            Reader& file;
            IMAGE_FILE_HEADER header{};
            std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> directories{};
            std::vector<IMAGE_SECTION_HEADER> sections;
            std::uint64_t base{};
            DWORD entry{}, headers{}, file_alignment{}, section_alignment{};
            WORD subsystem{}, flags{};
            bool is64{};
            std::wstring linker;
        };
        void append_row(Table& table, Row row)
        {
            if (table.rows.size() >= maximum_rows)
                throw std::runtime_error("Partial");
            std::size_t bytes{};
            for (const auto& value : row.cells)
                bytes += value.size() * sizeof(wchar_t);
            if (bytes > 32 * 1024 * 1024 - std::min<std::size_t>(table.text_bytes, 32 * 1024 * 1024))
                throw std::runtime_error("Partial");
            table.text_bytes += bytes;
            table.rows.push_back(std::move(row));
        }
        void add(Table& table, std::initializer_list<std::wstring> cells)
        {
            append_row(table, {std::vector<std::wstring>(cells)});
        }
        std::wstring architecture(WORD machine)
        {
            switch (machine)
            {
            case IMAGE_FILE_MACHINE_I386:
                return L"x86";
            case IMAGE_FILE_MACHINE_AMD64:
                return L"x64";
            case IMAGE_FILE_MACHINE_ARM64:
                return L"ARM64";
            case IMAGE_FILE_MACHINE_ARMNT:
                return L"ARM";
            case 0xA641:
                return L"ARM64EC";
            case 0xA64E:
                return L"ARM64X";
            default:
                return hex(machine);
            }
        }
        std::wstring subsystem_name(WORD value)
        {
            switch (value)
            {
            case IMAGE_SUBSYSTEM_WINDOWS_GUI:
                return L"Windows GUI";
            case IMAGE_SUBSYSTEM_WINDOWS_CUI:
                return L"Windows Console";
            case IMAGE_SUBSYSTEM_NATIVE:
                return L"Windows Native";
            case IMAGE_SUBSYSTEM_EFI_APPLICATION:
                return L"EFI Application";
            case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
                return L"EFI Boot Service Driver";
            case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
                return L"EFI Runtime Driver";
            default:
                return hex(value);
            }
        }
        void resources(Pe& pe, Table& table)
        {
            table.columns = {L"Type", L"Name", L"Language", L"Size", L"Offset"};
            const auto directory = pe.directories[IMAGE_DIRECTORY_ENTRY_RESOURCE];
            if (!directory.VirtualAddress || !directory.Size)
                return;
            const auto root = pe.offset(directory.VirtualAddress, directory.Size);
            std::set<DWORD> visited;
            const auto bounded = [&](DWORD offset, std::size_t length) {
                if (offset > directory.Size || length > directory.Size - offset)
                    throw std::runtime_error("Invalid");
                return root + offset;
            };
            const auto name = [&](DWORD value) -> std::wstring {
                if (!(value & 0x80000000U))
                    return std::to_wstring(value);
                const auto relative = value & 0x7fffffffU;
                const auto count = pe.file.at<WORD>(bounded(relative, 2));
                if (count > 2048)
                    throw std::runtime_error("Partial");
                const auto data = pe.file.bytes(bounded(relative + 2, count * 2ULL), count * 2ULL);
                return {reinterpret_cast<const wchar_t*>(data.data()), count};
            };
            std::function<void(DWORD, unsigned, std::wstring, std::wstring, DWORD, DWORD)> walk;
            walk = [&](DWORD position, unsigned depth, std::wstring type, std::wstring item, DWORD type_id,
                       DWORD item_id) {
                pe.file.check();
                if (depth > 16 || visited.size() >= maximum_rows)
                    throw std::runtime_error("Partial");
                if (!visited.insert(position).second)
                    throw std::runtime_error("Invalid");
                const auto header =
                    pe.file.at<IMAGE_RESOURCE_DIRECTORY>(bounded(position, sizeof(IMAGE_RESOURCE_DIRECTORY)));
                const auto count = unsigned(header.NumberOfNamedEntries) + header.NumberOfIdEntries;
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto entry =
                        pe.file.at<IMAGE_RESOURCE_DIRECTORY_ENTRY>(bounded(position + 16 + i * 8, 8));
                    const auto label = name(entry.Name);
                    const auto next_type = depth == 0 ? label : type;
                    const auto next_item = depth == 1 ? label : item;
                    const auto next_type_id = depth == 0 ? entry.Name : type_id;
                    const auto next_item_id = depth == 1 ? entry.Name : item_id;
                    if (entry.DataIsDirectory)
                        walk(entry.OffsetToDirectory, depth + 1, next_type, next_item, next_type_id,
                             next_item_id);
                    else
                    {
                        if (table.rows.size() >= maximum_rows)
                            throw std::runtime_error("Partial");
                        const auto data = pe.file.at<IMAGE_RESOURCE_DATA_ENTRY>(
                            bounded(entry.OffsetToData, sizeof(IMAGE_RESOURCE_DATA_ENTRY)));
                        const auto location = pe.offset(data.OffsetToData, data.Size);
                        append_row(table,
                                   {{next_type, next_item, label, std::to_wstring(data.Size), hex(location)},
                                    location,
                                    data.Size,
                                    next_type_id,
                                    next_item_id,
                                    static_cast<WORD>(entry.Name)});
                    }
                }
            };
            walk(0, 0, {}, {}, 0, 0);
        }
        void imports(Pe& pe, Table& table)
        {
            table.columns = {L"Name", L"Type", L"Value", L"Ordinal"};
            for (const auto index : {IMAGE_DIRECTORY_ENTRY_IMPORT, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT})
            {
                const auto directory = pe.directories[index];
                if (!directory.VirtualAddress || !directory.Size)
                    continue;
                const bool delay = index == IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT;
                const unsigned stride = delay ? 32U : 20U;
                for (std::uint64_t cursor = 0; cursor + stride <= directory.Size; cursor += stride)
                {
                    std::array<DWORD, 8> descriptor{};
                    pe.file.read(pe.offset(std::uint64_t(directory.VirtualAddress) + cursor, stride),
                                 descriptor.data(), stride);
                    if (std::all_of(descriptor.begin(), descriptor.end(), [](DWORD x) { return !x; }))
                        break;
                    const auto rva = [&](DWORD value) -> std::uint64_t {
                        if (delay && !(descriptor[0] & 1))
                        {
                            if (value < pe.base)
                                throw std::runtime_error("Invalid");
                            return value - pe.base;
                        }
                        return value;
                    };
                    const auto module = pe.file.ascii(pe.offset(rva(descriptor[delay ? 1 : 3])));
                    const auto thunk =
                        rva(delay ? descriptor[4] : (descriptor[0] ? descriptor[0] : descriptor[4]));
                    for (std::uint64_t i = 0;; ++i)
                    {
                        const auto address = pe.offset(thunk + i * (pe.is64 ? 8 : 4), pe.is64 ? 8 : 4);
                        const auto value =
                            pe.is64 ? pe.file.at<std::uint64_t>(address) : pe.file.at<DWORD>(address);
                        if (!value)
                            break;
                        const auto ordinal = value & (pe.is64 ? IMAGE_ORDINAL_FLAG64 : IMAGE_ORDINAL_FLAG32);
                        add(table, {module, delay ? L"Delay" : L"Direct",
                                    ordinal ? L"" : pe.file.ascii(pe.offset(value, 3) + 2),
                                    ordinal ? std::to_wstring(value & 0xffff) : L""});
                    }
                }
            }
        }
        void exports(Pe& pe, Table& table)
        {
            table.columns = {L"Name", L"Ordinal", L"Address", L"Forwarder"};
            const auto directory = pe.directories[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!directory.VirtualAddress || !directory.Size)
                return;
            const auto data = pe.file.at<IMAGE_EXPORT_DIRECTORY>(
                pe.offset(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
            if (data.NumberOfFunctions > maximum_rows || data.NumberOfNames > maximum_rows)
                throw std::runtime_error("Partial");
            std::vector<std::wstring> names(data.NumberOfFunctions);
            for (DWORD i = 0; i < data.NumberOfNames; ++i)
            {
                const auto ordinal =
                    pe.file.at<WORD>(pe.offset(std::uint64_t(data.AddressOfNameOrdinals) + i * 2ULL, 2));
                if (ordinal >= names.size())
                    throw std::runtime_error("Invalid");
                const auto address =
                    pe.file.at<DWORD>(pe.offset(std::uint64_t(data.AddressOfNames) + i * 4ULL, 4));
                const auto name = pe.file.ascii(pe.offset(address));
                if (!names[ordinal].empty())
                    names[ordinal] += L", ";
                names[ordinal] += name;
            }
            for (DWORD i = 0; i < data.NumberOfFunctions; ++i)
            {
                const auto address =
                    pe.file.at<DWORD>(pe.offset(std::uint64_t(data.AddressOfFunctions) + i * 4ULL, 4));
                if (!address)
                    continue;
                std::wstring forwarder;
                if (address >= directory.VirtualAddress &&
                    std::uint64_t(address) < std::uint64_t(directory.VirtualAddress) + directory.Size)
                    forwarder = pe.file.ascii(pe.offset(address));
                add(table,
                    {names[i], std::to_wstring(std::uint64_t(data.Base) + i), hex(address), forwarder});
            }
        }
        void structure(Pe& pe, Table& table)
        {
            table.columns = {L"Name", L"Value", L"Size", L"VirtualSize", L"Flags"};
            add(table, {L"Entry", hex(pe.entry)});
            add(table, {L"Base", hex(pe.base)});
            add(table, {L"Timestamp", hex(pe.header.TimeDateStamp)});
            add(table, {L"Linker", pe.linker});
            add(table, {L"Alignment",
                        std::to_wstring(pe.file_alignment) + L" / " + std::to_wstring(pe.section_alignment)});
            for (const auto& section : pe.sections)
            {
                std::wstring name;
                for (auto c : section.Name)
                {
                    if (!c)
                        break;
                    name += static_cast<wchar_t>(c);
                }
                std::wstring flags;
                if (section.Characteristics & IMAGE_SCN_MEM_READ)
                    flags += L"R ";
                if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
                    flags += L"W ";
                if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    flags += L"X ";
                add(table, {name, hex(section.VirtualAddress), std::to_wstring(section.SizeOfRawData),
                            std::to_wstring(section.Misc.VirtualSize), flags});
            }
            constexpr std::array names{L"Export",      L"Import",      L"Resource",   L"Exception",
                                       L"Certificate", L"Relocation",  L"Debug",      L"Architecture",
                                       L"GlobalPtr",   L"TLS",         L"LoadConfig", L"BoundImport",
                                       L"IAT",         L"DelayImport", L"CLR",        L"Reserved"};
            for (std::size_t i = 0; i < pe.directories.size(); ++i)
                if (pe.directories[i].Size)
                    add(table, {names[i], hex(pe.directories[i].VirtualAddress),
                                std::to_wstring(pe.directories[i].Size)});
            const auto debug = pe.directories[IMAGE_DIRECTORY_ENTRY_DEBUG];
            for (std::uint64_t i = 0; debug.VirtualAddress && i + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug.Size;
                 i += sizeof(IMAGE_DEBUG_DIRECTORY))
            {
                const auto entry = pe.file.at<IMAGE_DEBUG_DIRECTORY>(
                    pe.offset(std::uint64_t(debug.VirtualAddress) + i, sizeof(IMAGE_DEBUG_DIRECTORY)));
                if (entry.Type == IMAGE_DEBUG_TYPE_CODEVIEW && entry.SizeOfData >= 25 &&
                    pe.file.at<DWORD>(entry.PointerToRawData) == 0x53445352)
                    add(table, {L"PDB", pe.file.ascii(std::uint64_t(entry.PointerToRawData) + 24,
                                                      std::min<std::size_t>(4096, entry.SizeOfData - 24))});
            }
        }
        void manifest(Pe& pe, const Table& resource_table, Table& table)
        {
            for (const auto& resource : resource_table.rows)
            {
                if (resource.type != 24 || resource.length > 1024 * 1024)
                    continue;
                const auto data = pe.file.bytes(resource.offset, resource.length);
                Microsoft::WRL::ComPtr<IStream> stream;
                stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()),
                                                static_cast<UINT>(data.size())));
                Microsoft::WRL::ComPtr<IXmlReader> reader;
                if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), &reader, nullptr)) ||
                    FAILED(reader->SetInput(stream.Get())))
                    continue;
                reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
                XmlNodeType type{};
                std::wstring element;
                while (reader->Read(&type) == S_OK)
                {
                    pe.file.check();
                    const wchar_t* value{};
                    if (type == XmlNodeType_Element)
                    {
                        reader->GetLocalName(&value, nullptr);
                        element = value;
                        const auto attribute = [&](const wchar_t* name, const wchar_t* key) {
                            if (reader->MoveToAttributeByName(name, nullptr) == S_OK)
                            {
                                reader->GetValue(&value, nullptr);
                                add(table, {key, value});
                                reader->MoveToElement();
                            }
                        };
                        if (element == L"requestedExecutionLevel")
                        {
                            attribute(L"level", L"Permission");
                            attribute(L"uiAccess", L"UIAccess");
                        }
                        if (element == L"supportedOS")
                            attribute(L"Id", L"Compatibility");
                    }
                    else if (type == XmlNodeType_Text)
                    {
                        reader->GetValue(&value, nullptr);
                        if (element == L"dpiAware" || element == L"dpiAwareness")
                            add(table, {L"Dpi", value});
                        if (element == L"longPathAware")
                            add(table, {L"LongPath", value});
                    }
                }
            }
        }
        // ECMA-335 table widths. Heap and coded indices are sized by their declared row counts.
        class Metadata
        {
          public:
            explicit Metadata(Pe& pe)
            {
                const auto directory = pe.directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
                if (!directory.VirtualAddress)
                    return;
                const auto clr = pe.file.at<IMAGE_COR20_HEADER>(
                    pe.offset(directory.VirtualAddress, sizeof(IMAGE_COR20_HEADER)));
                flags = clr.Flags;
                bytes = pe.file.bytes(pe.offset(clr.MetaData.VirtualAddress, clr.MetaData.Size),
                                      clr.MetaData.Size);
                if (get(0, 4) != 0x424a5342)
                    throw std::runtime_error("Invalid");
                const auto version_length = get(12, 4);
                version = string(16, version_length);
                auto cursor = (16 + version_length + 3) & ~std::size_t(3);
                const auto stream_count = get(cursor + 2, 2);
                cursor += 4;
                for (std::size_t i = 0; i < stream_count; ++i)
                {
                    const auto offset = get(cursor, 4), length = get(cursor + 4, 4);
                    const auto name = string(cursor + 8, 32);
                    if (offset > bytes.size() || length > bytes.size() - offset)
                        throw std::runtime_error("Invalid");
                    if (name == L"#~" || name == L"#-")
                    {
                        tables = offset;
                        tables_end = offset + length;
                    }
                    if (name == L"#Strings")
                    {
                        strings = offset;
                        strings_length = length;
                    }
                    if (name == L"#Blob")
                    {
                        blobs = offset;
                        blobs_length = length;
                    }
                    if (name == L"#GUID")
                    {
                        guids = offset;
                        guids_length = length;
                    }
                    cursor += 8 + ((name.size() + 1 + 3) & ~std::size_t(3));
                }
                if (!tables)
                    throw std::runtime_error("Invalid");
                const auto heap_flags = get(tables + 6, 1);
                string_width = heap_flags & 1 ? 4 : 2;
                guid_width = heap_flags & 2 ? 4 : 2;
                blob_width = heap_flags & 4 ? 4 : 2;
                const auto valid = get(tables + 8, 8);
                cursor = tables + 24;
                for (unsigned i = 0; i < 64; ++i)
                    if (valid & (1ULL << i))
                    {
                        counts[i] = get(cursor, 4);
                        cursor += 4;
                        if (i > 44 || counts[i] > 1000000)
                            throw std::runtime_error("Partial");
                    }
                for (unsigned i = 0; i <= 44; ++i)
                {
                    offsets[i] = cursor;
                    widths[i] = width(i);
                    if (cursor > tables_end || counts[i] > (tables_end - cursor) / widths[i])
                        throw std::runtime_error("Invalid");
                    cursor += widths[i] * counts[i];
                }
            }
            std::uint64_t get(std::size_t offset, std::size_t length) const
            {
                if (offset > bytes.size() || length > bytes.size() - offset || length > 8)
                    throw std::runtime_error("Invalid");
                std::uint64_t result{};
                memcpy(&result, bytes.data() + offset, length);
                return result;
            }
            std::wstring string(std::size_t offset, std::size_t limit) const
            {
                if (offset >= bytes.size())
                    throw std::runtime_error("Invalid");
                std::size_t length{};
                while (length < limit && length < bytes.size() - offset &&
                       bytes[offset + length] != std::byte{})
                    ++length;
                if (length == limit || length == bytes.size() - offset)
                    throw std::runtime_error("Invalid");
                return decode_text({bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + length)});
            }
            std::wstring heap_string(std::size_t index) const
            {
                if (index >= strings_length)
                    throw std::runtime_error("Invalid");
                return string(strings + index, std::min<std::size_t>(4096, strings_length - index));
            }
            std::size_t table_index(unsigned table) const
            {
                return counts[table] < 65536 ? 2 : 4;
            }
            std::size_t coded(std::initializer_list<unsigned> tables_list, unsigned bits) const
            {
                std::size_t maximum{};
                for (auto table : tables_list)
                    maximum = std::max(maximum, counts[table]);
                return maximum < (1ULL << (16 - bits)) ? 2 : 4;
            }
            std::size_t width(unsigned table) const
            {
                const auto s = string_width, g = guid_width, b = blob_width;
                const auto td = coded({2, 1, 27}, 2), resolution = coded({0, 26, 35, 1}, 2);
                const auto member = coded({2, 1, 26, 6, 27}, 3), method = coded({6, 10}, 1);
                switch (table)
                {
                case 0:
                    return 2 + s + 3 * g;
                case 1:
                    return resolution + 2 * s;
                case 2:
                    return 4 + 2 * s + td + table_index(4) + table_index(6);
                case 3:
                    return table_index(4);
                case 4:
                    return 2 + s + b;
                case 5:
                    return table_index(6);
                case 6:
                    return 8 + s + b + table_index(8);
                case 7:
                    return table_index(8);
                case 8:
                    return 4 + s;
                case 9:
                    return table_index(2) + td;
                case 10:
                    return member + s + b;
                case 11:
                    return 2 + coded({4, 8, 23}, 2) + b;
                case 12:
                    return coded({6,  4,  1,  2,  8,  9,  10, 0,  14, 23, 20,
                                  17, 26, 27, 32, 35, 38, 39, 40, 42, 44, 43},
                                 5) +
                           coded({6, 10}, 3) + b;
                case 13:
                    return coded({4, 8}, 1) + b;
                case 14:
                    return 2 + coded({2, 6, 32}, 2) + b;
                case 15:
                    return 6 + table_index(2);
                case 16:
                    return 4 + table_index(4);
                case 17:
                    return b;
                case 18:
                    return table_index(2) + table_index(20);
                case 19:
                    return table_index(20);
                case 20:
                    return 2 + s + td;
                case 21:
                    return table_index(2) + table_index(23);
                case 22:
                    return table_index(23);
                case 23:
                    return 2 + s + b;
                case 24:
                    return 2 + table_index(6) + coded({20, 23}, 1);
                case 25:
                    return table_index(2) + 2 * method;
                case 26:
                    return s;
                case 27:
                    return b;
                case 28:
                    return 2 + coded({4, 6}, 1) + s + table_index(26);
                case 29:
                    return 4 + table_index(4);
                case 30:
                    return 8;
                case 31:
                    return 4;
                case 32:
                    return 16 + b + 2 * s;
                case 33:
                    return 4;
                case 34:
                    return 12;
                case 35:
                    return 12 + 2 * b + 2 * s;
                case 36:
                    return 4 + table_index(35);
                case 37:
                    return 12 + table_index(35);
                case 38:
                    return 4 + s + b;
                case 39:
                    return 8 + 2 * s + coded({38, 35, 39}, 2);
                case 40:
                    return 8 + s + coded({38, 35, 39}, 2);
                case 41:
                    return 2 * table_index(2);
                case 42:
                    return 4 + coded({2, 6}, 1) + s;
                case 43:
                    return method + b;
                case 44:
                    return table_index(42) + td;
                default:
                    throw std::runtime_error("Invalid");
                }
            }
            std::size_t row(unsigned table, std::size_t index) const
            {
                if (!index || index > counts[table])
                    throw std::runtime_error("Invalid");
                return offsets[table] + (index - 1) * widths[table];
            }
            std::vector<std::byte> blob(std::size_t index) const
            {
                if (!index)
                    return {};
                if (index >= blobs_length)
                    throw std::runtime_error("Invalid");
                auto cursor = blobs + index;
                const auto end = blobs + blobs_length;
                const auto first = get(cursor++, 1);
                std::size_t length{};
                if (!(first & 0x80))
                    length = first;
                else if ((first & 0xc0) == 0x80)
                {
                    length = ((first & 0x3f) << 8) | get(cursor++, 1);
                }
                else if ((first & 0xe0) == 0xc0)
                {
                    length = (first & 0x1f) << 24;
                    for (unsigned shift : {16U, 8U, 0U})
                        length |= get(cursor++, 1) << shift;
                }
                else
                    throw std::runtime_error("Invalid");
                if (cursor > end || length > end - cursor)
                    throw std::runtime_error("Invalid");
                return {bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                        bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length)};
            }
            std::vector<std::byte> bytes;
            std::array<std::size_t, 64> counts{}, offsets{}, widths{};
            std::size_t strings{}, strings_length{}, blobs{}, blobs_length{}, guids{}, guids_length{},
                tables{}, tables_end{};
            std::size_t string_width{}, guid_width{}, blob_width{};
            DWORD flags{};
            std::wstring version;
        };
        std::wstring public_key_token(const std::vector<std::byte>& key, bool full_key)
        {
            if (key.empty())
                return {};
            std::array<unsigned char, 20> digest{};
            if (full_key && BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())),
                                       static_cast<ULONG>(key.size()), digest.data(),
                                       static_cast<ULONG>(digest.size())) < 0)
                throw std::runtime_error("Error");
            std::wostringstream text;
            for (std::size_t i = 0; i < (full_key ? 8 : key.size()); ++i)
                text << std::hex << std::setfill(L'0') << std::setw(2)
                     << (full_key ? unsigned(digest[19 - i]) : std::to_integer<unsigned>(key[i]));
            return text.str();
        }
        std::wstring blob_hex(const std::vector<std::byte>& bytes)
        {
            std::wostringstream value;
            value << std::hex << std::uppercase << std::setfill(L'0');
            for (const auto byte : bytes) value << std::setw(2) << std::to_integer<unsigned>(byte) << L' ';
            return value.str();
        }
        void managed_members(Pe& pe, Table& table, Section section)
        {
            if (!pe.directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress) return;
            Metadata metadata(pe);
            const bool types = section == Section::managed_types;
            const bool methods = section == Section::managed_methods;
            table.columns = types
                ? std::vector<std::wstring>{L"Name", L"Flags", L"BaseType", L"MetadataToken"}
                : methods
                    ? std::vector<std::wstring>{L"Name", L"Address", L"Flags", L"ImplementationFlags", L"SignatureBlob", L"Parameters", L"MetadataToken"}
                    : std::vector<std::wstring>{L"Name", L"Flags", L"SignatureBlob", L"MetadataToken"};
            for (std::size_t i = 1; i <= metadata.counts[2]; ++i)
            {
                pe.file.check();
                const auto row = metadata.row(2, i);
                const auto name = metadata.heap_string(metadata.get(row + 4, metadata.string_width));
                const auto space = metadata.heap_string(metadata.get(row + 4 + metadata.string_width, metadata.string_width));
                const auto full_name = space.empty() ? name : space + L"." + name;
                const auto extends_offset = row + 4 + 2 * metadata.string_width;
                const auto extends_width = metadata.coded({2, 1, 27}, 2);
                if (types)
                {
                    const auto base = metadata.get(extends_offset, extends_width);
                    constexpr unsigned targets[]{2, 1, 27};
                    if ((base & 3) == 3) throw std::runtime_error("Invalid");
                    add(table, {full_name, hex(metadata.get(row, 4)),
                        base ? hex((std::uint64_t(targets[base & 3]) << 24) | (base >> 2)) : L"",
                        hex(0x02000000 | i)});
                    continue;
                }
                const unsigned target = methods ? 6 : 4;
                const unsigned pointers = methods ? 5 : 3;
                const auto count = metadata.counts[pointers] ? metadata.counts[pointers] : metadata.counts[target];
                const auto index_offset = 4 + 2 * metadata.string_width + extends_width +
                    (methods ? metadata.table_index(4) : 0);
                const auto first = metadata.get(row + index_offset, metadata.table_index(target));
                const auto end = i < metadata.counts[2]
                    ? metadata.get(metadata.row(2, i + 1) + index_offset, metadata.table_index(target)) : count + 1;
                if (!first || first > end || end > count + 1) throw std::runtime_error("Invalid");
                for (auto index = first; index < end; ++index)
                {
                    pe.file.check();
                    const auto rid = metadata.counts[pointers]
                        ? metadata.get(metadata.row(pointers, index), metadata.table_index(target)) : index;
                    const auto member = metadata.row(target, rid);
                    const auto member_name = metadata.heap_string(metadata.get(member + (methods ? 8 : 2), metadata.string_width));
                    const auto signature_offset = member + (methods ? 8 : 2) + metadata.string_width;
                    const auto signature = blob_hex(metadata.blob(metadata.get(signature_offset, metadata.blob_width)));
                    if (!methods)
                    {
                        add(table, {full_name + L"::" + member_name, hex(metadata.get(member, 2)), signature, hex(0x04000000 | rid)});
                        continue;
                    }
                    const auto parameter_offset = 8 + metadata.string_width + metadata.blob_width;
                    const auto first_parameter = metadata.get(member + parameter_offset, metadata.table_index(8));
                    const auto parameter_count = metadata.counts[7] ? metadata.counts[7] : metadata.counts[8];
                    const auto end_parameter = rid < metadata.counts[6]
                        ? metadata.get(metadata.row(6, rid + 1) + parameter_offset, metadata.table_index(8)) : parameter_count + 1;
                    if (!first_parameter || first_parameter > end_parameter || end_parameter > parameter_count + 1)
                        throw std::runtime_error("Invalid");
                    std::wstring parameters;
                    for (auto parameter = first_parameter; parameter < end_parameter; ++parameter)
                    {
                        pe.file.check();
                        const auto parameter_rid = metadata.counts[7]
                            ? metadata.get(metadata.row(7, parameter), metadata.table_index(8)) : parameter;
                        const auto entry = metadata.row(8, parameter_rid);
                        if (!parameters.empty()) parameters += L", ";
                        parameters += std::to_wstring(metadata.get(entry + 2, 2)) + L": " +
                            metadata.heap_string(metadata.get(entry + 4, metadata.string_width));
                    }
                    add(table, {full_name + L"::" + member_name, hex(metadata.get(member, 4)),
                        hex(metadata.get(member + 6, 2)), hex(metadata.get(member + 4, 2)), signature,
                        parameters, hex(0x06000000 | rid)});
                }
            }
        }
        void managed(Pe& pe, Table& table)
        {
            table.columns = {L"Name", L"Value", L"Version", L"Culture", L"Token"};
            table.field_keys = true;
            if (!pe.directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress)
                return;
            Metadata metadata(pe);
            add(table, {L"Metadata", metadata.version});
            for (std::size_t i = 1; i <= metadata.counts[2]; ++i)
            {
                pe.file.check();
                const auto row = metadata.row(2, i);
                const auto visibility = metadata.get(row, 4) & 7;
                if (visibility != 1 && visibility != 2) continue;
                const auto name = metadata.heap_string(metadata.get(row + 4, metadata.string_width));
                const auto space = metadata.heap_string(metadata.get(row + 4 + metadata.string_width, metadata.string_width));
                add(table, {L"PublicType", space.empty() ? name : space + L"." + name});
            }
            std::wstring flags;
            if (metadata.flags & COMIMAGE_FLAGS_ILONLY)
                flags += L"IL-only ";
            if (metadata.flags & COMIMAGE_FLAGS_32BITREQUIRED)
                flags += L"32-bit required ";
            if (metadata.flags & 0x20000)
                flags += L"32-bit preferred";
            add(table, {L"Flags", flags});
            if (metadata.counts[0])
            {
                const auto module = metadata.row(0, 1);
                const auto index = metadata.get(module + 2 + metadata.string_width, metadata.guid_width);
                if (index && index <= metadata.guids_length / 16)
                {
                    GUID guid{};
                    memcpy(&guid, metadata.bytes.data() + metadata.guids + (index - 1) * 16, 16);
                    wchar_t value[40]{};
                    StringFromGUID2(guid, value, 40);
                    add(table, {L"MVID", value});
                }
            }
            for (unsigned type : {32U, 35U})
                for (std::size_t i = 1; i <= metadata.counts[type]; ++i)
                {
                    pe.file.check();
                    const auto row = metadata.row(type, i), version = row + (type == 32 ? 4 : 0);
                    std::wstring version_text;
                    for (std::size_t field = 0; field < 4; ++field)
                    {
                        if (field)
                            version_text += L'.';
                        version_text += std::to_wstring(metadata.get(version + field * 2, 2));
                    }
                    const auto offset = row + (type == 32 ? 16 : 12);
                    const auto key = metadata.blob(metadata.get(offset, metadata.blob_width));
                    const auto name = metadata.heap_string(
                        metadata.get(offset + metadata.blob_width, metadata.string_width));
                    const auto culture = metadata.heap_string(metadata.get(
                        offset + metadata.blob_width + metadata.string_width, metadata.string_width));
                    const auto assembly_flags = metadata.get(row + (type == 32 ? 12 : 8), 4);
                    add(table, {type == 32 ? L"Assembly" : L"References", name, version_text, culture,
                                public_key_token(key, (assembly_flags & 1) != 0)});
                }
            const auto parent_width = metadata.coded(
                {6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17, 26, 27, 32, 35, 38, 39, 40, 42, 44, 43}, 5);
            const auto ctor_width = metadata.coded({6, 10}, 3);
            for (std::size_t i = 1; i <= metadata.counts[12]; ++i)
            {
                pe.file.check();
                const auto row = metadata.row(12, i);
                const auto parent = metadata.get(row, parent_width),
                           ctor = metadata.get(row + parent_width, ctor_width);
                if ((parent & 31) != 14 || (ctor & 7) != 3)
                    continue;
                const auto member = metadata.row(10, ctor >> 3);
                const auto member_parent = metadata.get(member, metadata.coded({2, 1, 26, 6, 27}, 3));
                if ((member_parent & 7) != 1)
                    continue;
                const auto ref = metadata.row(1, member_parent >> 3) + metadata.coded({0, 26, 35, 1}, 2);
                if (metadata.heap_string(metadata.get(ref, metadata.string_width)) !=
                        L"TargetFrameworkAttribute" ||
                    metadata.heap_string(metadata.get(ref + metadata.string_width, metadata.string_width)) !=
                        L"System.Runtime.Versioning")
                    continue;
                const auto value =
                    metadata.blob(metadata.get(row + parent_width + ctor_width, metadata.blob_width));
                if (value.size() < 3 || value[0] != std::byte{1} || value[1] != std::byte{})
                    continue;
                std::size_t cursor = 3, length = std::to_integer<unsigned>(value[2]);
                if ((length & 0xc0) == 0x80 && value.size() > 3)
                {
                    length = ((length & 0x3f) << 8) | std::to_integer<unsigned>(value[3]);
                    cursor = 4;
                }
                if (length < 128 || cursor == 4)
                    if (length <= value.size() - cursor)
                        add(table,
                            {L"Framework",
                             decode_text({value.begin() + static_cast<std::ptrdiff_t>(cursor),
                                          value.begin() + static_cast<std::ptrdiff_t>(cursor + length)})});
            }
        }
    } // namespace
    std::wstring hex(std::uint64_t value)
    {
        std::wostringstream text;
        text << L"0x" << std::uppercase << std::hex << value;
        return text.str();
    }
    Summary read_summary(const std::wstring& path, const Cancelled& cancelled)
    {
        Reader file(path, cancelled);
        Pe pe(file);
        Summary result;
        result.identity = file.identity;
        result.title = std::filesystem::path(path).filename().wstring();
        result.architecture = architecture(pe.header.Machine);
        result.dll = (pe.header.Characteristics & IMAGE_FILE_DLL) != 0;
        result.managed = pe.directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
        result.type = result.dll ? L"DLL" : L"EXE";
        auto& table = result.overview;
        table.columns = {L"Name", L"Value"};
        table.field_keys = true;
        add(table, {L"File", path});
        Table resource_table;
        try
        {
            resources(pe, resource_table);
            manifest(pe, resource_table, table);
            for (const auto& row : resource_table.rows)
            {
                if (row.type != 16 || row.length > 1024 * 1024)
                    continue;
                auto data = file.bytes(row.offset, row.length);
                struct Translation
                {
                    WORD language;
                    WORD codepage;
                };
                Translation* translations{};
                UINT length{};
                if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                                    reinterpret_cast<void**>(&translations), &length))
                    continue;
                if (length < sizeof(Translation))
                    continue;
                const auto translation = *translations;
                constexpr std::array properties{L"FileDescription", L"ProductName", L"FileVersion",
                                                L"ProductVersion",  L"CompanyName", L"OriginalFilename",
                                                L"LegalCopyright"};
                constexpr std::array keys{L"Description", L"Product",  L"FileVersion", L"ProductVersion",
                                          L"Company",     L"Original", L"Copyright"};
                for (std::size_t i = 0; i < properties.size(); ++i)
                {
                    wchar_t query[160]{};
                    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translation.language,
                               translation.codepage, properties[i]);
                    wchar_t* value{};
                    if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &length) &&
                        length && value)
                    {
                        const std::wstring text(value, wcsnlen_s(value, length));
                        if (!text.empty())
                            add(table, {keys[i], text});
                        if (i == 0 && !text.empty())
                            result.title = text;
                        if (i == 2)
                            result.version = text;
                    }
                }
                break;
            }
            const auto icon = std::find_if(resource_table.rows.begin(), resource_table.rows.end(),
                [](const Row& row) { return row.type == 14 && row.length <= 1024 * 1024; });
            if (icon != resource_table.rows.end())
            {
                auto ico = icon_file(path, result.identity, *icon);
                if (ico.size() >= 22)
                {
                    WORD count{}; memcpy(&count, ico.data() + 4, sizeof(count));
                    if (count && count <= (ico.size() - 6) / 16)
                    {
                        std::size_t best = 6; unsigned best_size{};
                        for (unsigned i = 0; i < count; ++i)
                        {
                            const auto entry = 6 + i * 16;
                            const auto width = std::to_integer<unsigned>(ico[entry]);
                            const auto size = width ? width : 256;
                            if (size > best_size) { best = entry; best_size = size; }
                        }
                        DWORD size{}, offset{};
                        memcpy(&size, ico.data() + best + 8, sizeof(size));
                        memcpy(&offset, ico.data() + best + 12, sizeof(offset));
                        if (offset <= ico.size() && size <= ico.size() - offset && size <= 1024 * 1024 - 22)
                        {
                            result.icon.resize(22 + size);
                            memcpy(result.icon.data(), ico.data(), 6);
                            const WORD single = 1; memcpy(result.icon.data() + 4, &single, sizeof(single));
                            memcpy(result.icon.data() + 6, ico.data() + best, 16);
                            const DWORD start = 22; memcpy(result.icon.data() + 18, &start, sizeof(start));
                            memcpy(result.icon.data() + 22, ico.data() + offset, size);
                        }
                    }
                }
            }
        }
        catch (const std::exception& error)
        {
            if (std::string_view(error.what()) == "Cancelled" || std::string_view(error.what()) == "Changed")
                throw;
        }
        add(table, {L"Architecture", result.architecture});
        add(table, {L"Type", result.type});
        add(table, {L"Subsystem", subsystem_name(pe.subsystem)});
        add(table, {L"Size", std::to_wstring(file.identity.size)});
        std::wstring mitigations;
        if (pe.flags & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)
            mitigations += L"ASLR ";
        if (pe.flags & IMAGE_DLLCHARACTERISTICS_NX_COMPAT)
            mitigations += L"DEP ";
        if (pe.flags & IMAGE_DLLCHARACTERISTICS_GUARD_CF)
            mitigations += L"CFG ";
        if (pe.flags & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA)
            mitigations += L"High-entropy ASLR";
        add(table, {L"Security", mitigations});
        file.verify(result.identity);
        return result;
    }
    Table read_section(const std::wstring& path, const Identity& identity, Section section,
                       const Cancelled& cancelled)
    {
        Table result;
        try
        {
            Reader file(path, cancelled);
            file.verify(identity);
            Pe pe(file);
            switch (section)
            {
            case Section::overview:
                return read_summary(path, cancelled).overview;
            case Section::imports:
                imports(pe, result);
                break;
            case Section::exports:
                exports(pe, result);
                break;
            case Section::resources:
                resources(pe, result);
                break;
            case Section::structure:
                structure(pe, result);
                break;
            case Section::managed:
                managed(pe, result);
                break;
            case Section::managed_types:
            case Section::managed_methods:
            case Section::managed_fields:
                managed_members(pe, result, section);
                break;
            default:
                result.state = L"NotLoaded";
                break;
            }
            file.verify(identity);
        }
        catch (const std::exception& error)
        {
            const std::string message(error.what());
            result.state.assign(message.begin(), message.end());
            if (result.state != L"Partial")
                result.rows.clear();
        }
        return result;
    }
    std::vector<std::byte> read_resource(const std::wstring& path, const Identity& identity, const Row& row,
                                         std::size_t limit)
    {
        Reader file(path);
        file.verify(identity);
        auto result = file.bytes(row.offset, std::min<std::size_t>(row.length, limit));
        file.verify(identity);
        return result;
    }
    void verify_identity(const std::wstring& path, const Identity& identity)
    {
        Reader file(path);
        file.verify(identity);
    }
    std::wstring decode_text(const std::vector<std::byte>& bytes)
    {
        if (bytes.size() >= 2 && bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xfe})
            return {reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2};
        const auto* data = reinterpret_cast<const char*>(bytes.data());
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
                                               static_cast<int>(bytes.size()), nullptr, 0);
        if (!count)
            return {};
        std::wstring result(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(bytes.size()),
                            result.data(), count);
        return result;
    }
    std::vector<std::byte> icon_file(const std::wstring& path, const Identity& identity, const Row& group)
    {
        const auto directory = read_resource(path, identity, group, 65536);
        if (directory.size() < 6 || (group.type != 14 && group.type != 12))
            throw std::runtime_error("Invalid");
        WORD count{};
        memcpy(&count, directory.data() + 4, 2);
        if (!count || count > 256 || directory.size() < 6 + count * 14ULL)
            throw std::runtime_error("Invalid");
        const auto table = read_section(path, identity, Section::resources, {});
        if (table.state != L"Complete")
            throw std::runtime_error("Invalid");
        std::vector<std::byte> output(6 + count * 16ULL);
        const WORD type = group.type == 14 ? 1 : 2;
        memcpy(output.data() + 2, &type, 2);
        memcpy(output.data() + 4, &count, 2);
        for (unsigned i = 0; i < count; ++i)
        {
            WORD id{};
            memcpy(&id, directory.data() + 6 + i * 14 + 12, 2);
            auto leaf = std::find_if(table.rows.begin(), table.rows.end(), [&](const Row& row) {
                return row.type == (type == 1 ? 3U : 1U) && row.id == id && row.language == group.language;
            });
            if (leaf == table.rows.end())
                leaf = std::find_if(table.rows.begin(), table.rows.end(), [&](const Row& row) {
                    return row.type == (type == 1 ? 3U : 1U) && row.id == id;
                });
            if (leaf == table.rows.end() || leaf->length > 4 * 1024 * 1024 ||
                output.size() > 32 * 1024 * 1024)
                throw std::runtime_error("Partial");
            auto data = read_resource(path, identity, *leaf, leaf->length);
            auto* entry = output.data() + 6 + i * 16;
            if (type == 1)
                memcpy(entry, directory.data() + 6 + i * 14, 8);
            else
            {
                if (data.size() < 4)
                    throw std::runtime_error("Invalid");
                WORD width{}, height{};
                memcpy(&width, directory.data() + 6 + i * 14, 2);
                memcpy(&height, directory.data() + 8 + i * 14, 2);
                entry[0] = static_cast<std::byte>(width >= 256 ? 0 : width);
                entry[1] = static_cast<std::byte>(height / 2 >= 256 ? 0 : height / 2);
                memcpy(entry + 4, data.data(), 4);
                data.erase(data.begin(), data.begin() + 4);
            }
            const auto size = static_cast<DWORD>(data.size()), offset = static_cast<DWORD>(output.size());
            memcpy(entry + 8, &size, 4);
            memcpy(entry + 12, &offset, 4);
            output.insert(output.end(), data.begin(), data.end());
        }
        return output;
    }
    std::wstring sha256(const std::wstring& path, const Identity& identity, const Cancelled& cancelled,
                        const std::function<void(unsigned)>& progress)
    {
        Reader file(path, cancelled);
        file.verify(identity);
        BCRYPT_HASH_HANDLE raw{};
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &raw, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Error");
        const auto cleanup = [](void* hash) { BCryptDestroyHash(hash); };
        std::unique_ptr<void, decltype(cleanup)> hash(raw, cleanup);
        std::vector<unsigned char> buffer(256 * 1024);
        unsigned last_progress{};
        for (std::uint64_t offset = 0; offset < identity.size;)
        {
            file.renew_budget();
            const auto count =
                static_cast<ULONG>(std::min<std::uint64_t>(buffer.size(), identity.size - offset));
            file.read(offset, buffer.data(), count);
            if (BCryptHashData(raw, buffer.data(), count, 0) < 0)
                throw std::runtime_error("Error");
            offset += count;
            const auto percent = static_cast<unsigned>(static_cast<double>(offset) /
                                                       static_cast<double>(identity.size) * 100.0);
            if (progress && percent != last_progress)
            {
                last_progress = percent;
                progress(percent);
            }
        }
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(raw, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Error");
        file.verify(identity);
        std::wostringstream output;
        for (auto byte : digest)
            output << std::hex << std::setw(2) << std::setfill(L'0') << unsigned(byte);
        return output.str();
    }
    void export_resource(const std::wstring& path, const Identity& identity, const Row& row,
                         const std::wstring& destination, const Cancelled& cancelled)
    {
        Reader file(path, cancelled);
        file.verify(identity);
        Handle output{CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("SaveFailed");
        try
        {
            std::vector<std::byte> buffer(256 * 1024);
            for (std::uint64_t cursor = 0; cursor < row.length;)
            {
                file.renew_budget();
                const auto count =
                    static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), row.length - cursor));
                file.read(row.offset + cursor, buffer.data(), count);
                DWORD written{};
                if (!WriteFile(output.value, buffer.data(), count, &written, nullptr) || written != count)
                    throw std::runtime_error("SaveFailed");
                cursor += count;
            }
            file.verify(identity);
        }
        catch (...)
        {
            CloseHandle(std::exchange(output.value, INVALID_HANDLE_VALUE));
            DeleteFileW(destination.c_str());
            throw;
        }
    }
} // namespace glance::executable
