#include "../protocol.h"
#include <wincrypt.h>
#include <algorithm>
#include <set>
#include <cwctype>

namespace glance::executable
{
namespace
{
std::wstring joined(const std::vector<std::wstring>& values, std::size_t limit)
{
    std::wstring result;
    for (std::size_t i = 0; i < std::min(values.size(), limit); ++i)
    {
        if (i) result += L", ";
        result += values[i];
    }
    if (values.size() > limit) result += L" …";
    return result;
}
std::wstring signer(const wchar_t* path)
{
    HCERTSTORE store{}; HCRYPTMSG message{};
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &store, &message, nullptr)) return {};
    struct Owner { HCERTSTORE store; HCRYPTMSG message; ~Owner() { if (message) CryptMsgClose(message); if (store) CertCloseStore(store, 0); } } owner{store, message};
    DWORD length{};
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &length) || length > 1024 * 1024) return {};
    std::vector<std::byte> bytes(length);
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, bytes.data(), &length)) return {};
    const auto info = reinterpret_cast<const CMSG_SIGNER_INFO*>(bytes.data());
    CERT_INFO lookup{}; lookup.Issuer = info->Issuer; lookup.SerialNumber = info->SerialNumber;
    const auto certificate = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &lookup, nullptr);
    if (!certificate) return {};
    const auto count = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    std::wstring name(count, L'\0');
    CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), count);
    CertFreeCertificateContext(certificate);
    if (!name.empty()) name.pop_back();
    return name;
}
void add(Table& table, const wchar_t* key, std::wstring value)
{
    if (!value.empty()) table.rows.push_back({{key, std::move(value)}});
}
void add_items(Table& table, const wchar_t* key, const std::vector<std::wstring>& items)
{
    if (!items.empty()) table.rows.push_back({{key, joined(items, 12), std::to_wstring(items.size())}});
}
}
int inspect(Transfer& transfer, HANDLE cancellation) noexcept
{
    Result result;
    try
    {
        const auto& query = transfer.query;
        if (wcsnlen_s(query.path, std::size(query.path)) == std::size(query.path)) throw std::runtime_error("Invalid");
        const auto cancelled = [cancellation] { return WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0; };
        if (cancelled()) throw std::runtime_error("Cancelled");
        result.summary = read_summary(query.path, cancelled);
        const auto& identity = result.summary.identity;
        if (query.detailed)
        {
            add(result.summary.overview, L"Signer", signer(query.path));
            std::size_t remaining = 20 * 1024 * 1024;
            for (const auto section : {Section::exports, Section::imports, Section::managed,
                Section::managed_types, Section::managed_methods, Section::managed_fields,
                Section::resources, Section::structure})
            {
                if (cancelled()) throw std::runtime_error("Cancelled");
                if (!result.summary.managed && (section == Section::managed || section == Section::managed_types ||
                    section == Section::managed_methods || section == Section::managed_fields)) continue;
                auto entries = read_section(query.path, identity, section, cancelled);
                std::size_t kept{};
                for (const auto& row : entries.rows)
                {
                    std::size_t bytes = 64;
                    for (const auto& cell : row.cells) bytes += cell.size() * sizeof(wchar_t) + 4;
                    if (bytes > remaining) { entries.state = L"Partial"; break; }
                    remaining -= bytes; ++kept;
                }
                entries.rows.resize(kept);
                if (entries.state != L"Complete") result.table.state = L"Partial";
                result.details.emplace_back(section_keys[static_cast<unsigned>(section)], std::move(entries));
            }
            verify_identity(query.path, identity);
            Writer{}.result(result, transfer);
            return 0;
        }
        auto imports = read_section(query.path, identity, Section::imports, cancelled);
        const auto exports = result.summary.dll ? read_section(query.path, identity, Section::exports, cancelled) : Table{};
        const auto resources = read_section(query.path, identity, Section::resources, cancelled);
        std::vector<std::wstring> dependencies, interfaces, evidence;
        std::set<std::wstring> unique;
        for (const auto& row : imports.rows)
        {
            if (row.cells.empty()) continue;
            if (unique.insert(row.cells[0]).second) dependencies.push_back(row.cells[0]);
            if (!result.summary.dll && row.cells.size() > 2) evidence.push_back(row.cells[2]);
        }
        for (const auto& row : exports.rows)
        {
            if (row.cells.empty() || row.cells[0].empty()) continue;
            interfaces.push_back(row.cells[0]); evidence.push_back(row.cells[0]);
        }
        struct Capability { const wchar_t* key; std::initializer_list<const wchar_t*> prefixes; };
        const Capability capabilities[]{
            {L"Capability_Image", {L"png_", L"jpeg_", L"webp", L"avif", L"heif_", L"gdi", L"d2d1"}},
            {L"Capability_Media", {L"avcodec_", L"avformat_", L"avfilter_", L"mfcreate", L"mfx", L"opus_", L"vorbis_"}},
            {L"Capability_Archive", {L"zstd_", L"lzma_", L"zip_", L"archive_", L"deflate", L"inflate"}},
            {L"Capability_Database", {L"sqlite3_", L"mysql_", L"pqconnect", L"sqlconnect", L"sqlexec"}},
            {L"Capability_Network", {L"curl_", L"winhttp", L"internetopen", L"httpopen", L"wsastartup", L"socket", L"ssl_"}},
            {L"Capability_Graphics", {L"d3d", L"createdxgi", L"vkcreate", L"opengl", L"wgl"}},
            {L"Capability_Python", {L"pyinit_", L"py_initialize", L"pyrun_"}},
            {L"Capability_Com", {L"dllgetclassobject", L"dllregisterserver"}},
            {L"Capability_Service", {L"startservicectrldispatcher", L"registerservicectrlhandler"}}};
        std::vector<std::wstring> representative;
        for (const auto& capability : capabilities)
        {
            std::vector<std::wstring> matches;
            for (const auto& name : evidence)
            {
                auto lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                if (std::any_of(capability.prefixes.begin(), capability.prefixes.end(), [&](const auto prefix) { return lower.starts_with(prefix); }))
                    matches.push_back(name);
            }
            if (!matches.empty())
            {
                add(result.table, capability.key, joined(matches, 2));
                representative.insert(representative.end(), matches.begin(), matches.end());
            }
        }
        if (result.summary.dll)
        {
            add(result.table, L"Summary_ExportCount", std::to_wstring(exports.rows.size()));
            add(result.table, L"Summary_Interfaces", joined(representative.empty() ? interfaces : representative, 5));
        }
        if (result.summary.managed)
        {
            const auto managed = read_section(query.path, identity, Section::managed, cancelled);
            std::vector<std::wstring> types;
            for (const auto& row : managed.rows)
            {
                if (row.cells.size() < 2) continue;
                if (row.cells[0] == L"Framework" || row.cells[0] == L"Assembly") add(result.table, row.cells[0].c_str(), row.cells[1]);
                else if (row.cells[0] == L"PublicType") types.push_back(row.cells[1]);
                else if (row.cells[0] == L"References" && unique.insert(row.cells[1]).second) dependencies.push_back(row.cells[1]);
            }
            add(result.table, L"Summary_PublicTypes", joined(types, 5));
        }
        add_items(result.table, L"Summary_Dependencies", dependencies);
        std::vector<std::wstring> icons, dialogs, strings, embedded;
        for (const auto& row : resources.rows)
        {
            if (row.cells.size() < 2) continue;
            const auto& name = row.cells[1];
            if (row.type == 14) icons.push_back(name);
            else if (row.type == 5) dialogs.push_back(name);
            else if (row.type == 6) strings.push_back(name);
            else if (row.type == 10) embedded.push_back(name);
        }
        add_items(result.table, L"Summary_Icons", icons);
        add_items(result.table, L"Summary_Dialogs", dialogs);
        add_items(result.table, L"Summary_Strings", strings);
        add_items(result.table, L"Summary_Embedded", embedded);
        add(result.summary.overview, L"Signer", signer(query.path));
        if (imports.state != L"Complete" || exports.state != L"Complete" || resources.state != L"Complete") result.table.state = L"Partial";
        verify_identity(query.path, identity);
    }
    catch (const std::exception& error) { const std::string key(error.what()); result.table.state.assign(key.begin(), key.end()); }
    catch (...) { result.table.state = L"Error"; }
    try { Writer{}.result(result, transfer); return 0; } catch (...) { return 1; }
}
}
