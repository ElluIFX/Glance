#include "signature.h"
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace glance::executable
{
    namespace
    {
        struct Handle
        {
            HANDLE value{};
            ~Handle()
            {
                if (value && value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
        };
        std::wstring certificate_name(PCCERT_CONTEXT certificate, DWORD flags)
        {
            const auto count =
                CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, nullptr, 0);
            std::wstring value(count, L'\0');
            CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, value.data(),
                               count);
            if (!value.empty())
                value.pop_back();
            return value;
        }
        std::wstring time_text(FILETIME time)
        {
            SYSTEMTIME system{};
            if (!FileTimeToSystemTime(&time, &system))
                return {};
            wchar_t value[40]{};
            swprintf_s(value, L"%04u-%02u-%02u %02u:%02u:%02u UTC", system.wYear, system.wMonth, system.wDay,
                       system.wHour, system.wMinute, system.wSecond);
            return value;
        }
        void field(Table& table, std::wstring key, std::wstring value)
        {
            table.rows.push_back({{std::move(key), std::move(value)}});
        }
        struct Trust
        {
            WINTRUST_DATA data{sizeof(data)};
            GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            Trust()
            {
                data.dwUIChoice = WTD_UI_NONE;
                data.dwStateAction = WTD_STATEACTION_VERIFY;
                data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
                data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                                   WTD_DISABLE_MD2_MD4;
            }
            ~Trust()
            {
                if (data.hWVTStateData)
                {
                    data.dwStateAction = WTD_STATEACTION_CLOSE;
                    WinVerifyTrust(nullptr, &action, &data);
                }
            }
            LONG verify()
            {
                return WinVerifyTrust(nullptr, &action, &data);
            }
        };
        void describe(Table& table, Trust& trust, LONG status)
        {
            const auto code = static_cast<DWORD>(status);
            const wchar_t* state = status == ERROR_SUCCESS                          ? L"Trusted"
                                   : code == static_cast<DWORD>(TRUST_E_BAD_DIGEST) ? L"InvalidSignature"
                                                                                    : L"Unverified";
            field(table, L"Integrity", state);
            field(table, L"State", hex(code));
            field(table, L"Revocation", L"CacheOnly");
            const auto provider = WTHelperProvDataFromStateData(trust.data.hWVTStateData);
            const auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (!signer)
                return;
            for (DWORD i = 0; i < std::min<DWORD>(signer->csCertChain, 16); ++i)
            {
                const auto cert = signer->pasCertChain[i].pCert;
                if (!cert)
                    continue;
                field(table, i ? L"Certificate" : L"Signer", certificate_name(cert, 0));
                field(table, L"Issuer", certificate_name(cert, CERT_NAME_ISSUER_FLAG));
                field(table, L"ValidFrom", time_text(cert->pCertInfo->NotBefore));
                field(table, L"ValidUntil", time_text(cert->pCertInfo->NotAfter));
            }
            if (signer->psSigner && signer->psSigner->HashAlgorithm.pszObjId)
            {
                const auto info =
                    CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY, signer->psSigner->HashAlgorithm.pszObjId, 0);
                if (info && info->pwszName)
                    field(table, L"Algorithm", info->pwszName);
            }
            if (signer->csCounterSigners)
                field(table, L"SigningTime", time_text(signer->sftVerifyAsOf));
        }
        Table collect(const wchar_t* path)
        {
            Table table;
            table.columns = {L"Name", L"Value"};
            table.field_keys = true;
            WINTRUST_FILE_INFO file{sizeof(file)};
            file.pcwszFilePath = path;
            DWORD count{};
            bool found{};
            for (DWORD index = 0; index <= std::min<DWORD>(count, 15); ++index)
            {
                Trust trust;
                trust.data.dwUnionChoice = WTD_CHOICE_FILE;
                trust.data.pFile = &file;
                WINTRUST_SIGNATURE_SETTINGS settings{sizeof(settings)};
                settings.dwIndex = index;
                settings.dwFlags = index ? WSS_VERIFY_SPECIFIC : WSS_GET_SECONDARY_SIG_COUNT;
                trust.data.pSignatureSettings = &settings;
                const auto status = trust.verify();
                if (index == 0)
                    count = settings.cSecondarySigs;
                if (static_cast<DWORD>(status) == static_cast<DWORD>(TRUST_E_NOSIGNATURE))
                    break;
                found = true;
                field(table, L"Embedded", std::to_wstring(index + 1));
                describe(table, trust, status);
            }
            // Catalog membership is local to this computer and is independent of embedded signatures.
            Handle source{CreateFileW(path, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, 0, nullptr)};
            if (source.value != INVALID_HANDLE_VALUE)
                for (const auto algorithm : {BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA1_ALGORITHM})
                {
                    HCATADMIN admin{};
                    if (!CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0))
                        continue;
                    const auto release = [](void* value) { CryptCATAdminReleaseContext(value, 0); };
                    std::unique_ptr<void, decltype(release)> owner(admin, release);
                    DWORD size{};
                    if (!CryptCATAdminCalcHashFromFileHandle2(admin, source.value, &size, nullptr, 0) ||
                        !size || size > 1024)
                        continue;
                    std::vector<BYTE> hash(size);
                    if (!CryptCATAdminCalcHashFromFileHandle2(admin, source.value, &size, hash.data(), 0))
                        continue;
                    std::wstring tag;
                    for (auto byte : hash)
                    {
                        wchar_t pair[3]{};
                        swprintf_s(pair, L"%02X", unsigned(byte));
                        tag += pair;
                    }
                    HCATINFO previous{};
                    unsigned catalogs{};
                    while (const auto catalog =
                               CryptCATAdminEnumCatalogFromHash(admin, hash.data(), size, 0, &previous))
                    {
                        previous = catalog;
                        CATALOG_INFO info{sizeof(info)};
                        if (CryptCATCatalogInfoFromContext(catalog, &info, 0))
                        {
                            WINTRUST_CATALOG_INFO membership{sizeof(membership)};
                            membership.pcwszCatalogFilePath = info.wszCatalogFile;
                            membership.pcwszMemberTag = tag.c_str();
                            membership.pcwszMemberFilePath = path;
                            membership.hMemberFile = source.value;
                            membership.pbCalculatedFileHash = hash.data();
                            membership.cbCalculatedFileHash = size;
                            membership.hCatAdmin = admin;
                            Trust trust;
                            trust.data.dwUnionChoice = WTD_CHOICE_CATALOG;
                            trust.data.pCatalog = &membership;
                            const auto status = trust.verify();
                            found = true;
                            field(table, L"Catalog", info.wszCatalogFile);
                            describe(table, trust, status);
                        }
                        if (++catalogs >= 16)
                        {
                            CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
                            break;
                        }
                    }
                }
            if (!found)
                field(table, L"Signatures", L"Unsigned");
            return table;
        }
        void append(std::vector<std::byte>& data, std::wstring_view text)
        {
            const auto length = static_cast<std::uint32_t>(text.size());
            const auto old = data.size();
            data.resize(old + 4 + text.size() * 2);
            memcpy(data.data() + old, &length, 4);
            memcpy(data.data() + old + 4, text.data(), text.size() * 2);
        }
    } // namespace
    int signature_worker(const wchar_t* path, HANDLE output) noexcept
    {
        try
        {
            const auto table = collect(path);
            std::vector<std::byte> data;
            for (const auto& row : table.rows)
            {
                append(data, row.cells[0]);
                append(data, row.cells[1]);
            }
            DWORD count{};
            return WriteFile(output, data.data(), static_cast<DWORD>(data.size()), &count, nullptr) &&
                           count == data.size()
                       ? 0
                       : 1;
        }
        catch (...)
        {
            return 1;
        }
    }
    Table verify_signature(const std::wstring& path, const Cancelled& cancelled,
                           const std::wstring& worker_path)
    {
        Table table;
        table.columns = {L"Name", L"Value"};
        table.field_keys = true;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle input, output;
        if (!CreatePipe(&input.value, &output.value, &security, 0))
            throw std::runtime_error("Error");
        SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0);
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        if (!worker_path.empty())
            wcscpy_s(executable, worker_path.c_str());
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --signature " +
                               std::to_wstring(reinterpret_cast<std::uintptr_t>(output.value)) + L" \"" +
                               path + L"\"";
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<std::byte> attributes(size);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
            throw std::runtime_error("Error");
        const bool configured =
            UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &output.value, sizeof(HANDLE), nullptr, nullptr) != FALSE;
        PROCESS_INFORMATION process{};
        const bool launched =
            configured && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                         nullptr, nullptr, &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!launched)
            throw std::runtime_error("Error");
        Handle child{process.hProcess}, thread{process.hThread};
        Handle job{CreateJobObjectW(nullptr, nullptr)};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value ||
            !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.value, child.value) ||
            ResumeThread(thread.value) == static_cast<DWORD>(-1))
        {
            TerminateProcess(child.value, 1);
            throw std::runtime_error("Error");
        }
        CloseHandle(output.value);
        output.value = nullptr;
        std::vector<std::byte> data;
        const auto started = GetTickCount64();
        for (;;)
        {
            if ((cancelled && cancelled()) || GetTickCount64() - started > 30000 || data.size() > 1024 * 1024)
            {
                TerminateProcess(child.value, 1);
                WaitForSingleObject(child.value, 2000);
                table.state = cancelled && cancelled() ? L"Cancelled" : L"Unverified";
                return table;
            }
            DWORD available{};
            if (PeekNamedPipe(input.value, nullptr, 0, nullptr, &available, nullptr) && available)
            {
                const auto old = data.size();
                data.resize(old + available);
                DWORD read{};
                if (!ReadFile(input.value, data.data() + old, available, &read, nullptr))
                    break;
                data.resize(old + read);
                continue;
            }
            if (WaitForSingleObject(child.value, 20) == WAIT_OBJECT_0)
            {
                // The worker can write its final result between PeekNamedPipe and the wait.
                DWORD remaining{};
                if (PeekNamedPipe(input.value, nullptr, 0, nullptr, &remaining, nullptr) && remaining)
                    continue;
                break;
            }
        }
        DWORD code{};
        GetExitCodeProcess(child.value, &code);
        if (code || data.empty())
        {
            table.state = L"Unverified";
            return table;
        }
        std::size_t cursor{};
        const auto next = [&]() {
            if (data.size() - cursor < 4)
                throw std::runtime_error("Invalid");
            DWORD length{};
            memcpy(&length, data.data() + cursor, 4);
            cursor += 4;
            if (length > (data.size() - cursor) / 2)
                throw std::runtime_error("Invalid");
            std::wstring value(reinterpret_cast<const wchar_t*>(data.data() + cursor), length);
            cursor += length * 2ULL;
            return value;
        };
        while (cursor < data.size())
        {
            auto key = next();
            auto value = next();
            field(table, std::move(key), std::move(value));
        }
        return table;
    }
} // namespace glance::executable
