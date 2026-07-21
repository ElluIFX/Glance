#include "pch.h"
#include "generic_file_info.h"
#include "localization.h"

#include <aclapi.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
    std::wstring file_header(std::wstring_view path)
    {
        const HANDLE file = CreateFileW(
            std::wstring(path).c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        std::array<unsigned char, 64> bytes{};
        DWORD count{};
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
        CloseHandle(file);
        if (count == 0)
        {
            return {};
        }

        std::wostringstream output;
        output << glance::app::localize_format(L"GenericHeader", { std::to_wstring(count) });
        for (DWORD offset = 0; offset < count; offset += 16)
        {
            output << L'\n' << std::hex << std::uppercase << std::setfill(L'0')
                   << std::setw(4) << offset << L"  ";
            for (DWORD index = 0; index < 16; ++index)
            {
                if (offset + index < count)
                {
                    output << std::setw(2) << static_cast<unsigned int>(bytes[offset + index]) << L' ';
                }
                else
                {
                    output << L"   ";
                }
            }
            output << L" ";
            for (DWORD index = 0; index < 16 && offset + index < count; ++index)
            {
                const auto value = bytes[offset + index];
                output << static_cast<wchar_t>(value >= 0x20 && value <= 0x7E ? value : '.');
            }
        }
        return output.str();
    }

    std::wstring file_attributes(std::wstring_view path)
    {
        const DWORD attributes = GetFileAttributesW(std::wstring(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return {};
        }
        struct AttributeName
        {
            DWORD flag;
            wchar_t const* name;
        };
        constexpr AttributeName names[]{
            { FILE_ATTRIBUTE_READONLY, L"AttributeReadOnly" },
            { FILE_ATTRIBUTE_HIDDEN, L"AttributeHidden" },
            { FILE_ATTRIBUTE_SYSTEM, L"AttributeSystem" },
            { FILE_ATTRIBUTE_ARCHIVE, L"AttributeArchive" },
            { FILE_ATTRIBUTE_COMPRESSED, L"AttributeCompressed" },
            { FILE_ATTRIBUTE_ENCRYPTED, L"AttributeEncrypted" },
            { FILE_ATTRIBUTE_OFFLINE, L"AttributeOffline" },
            { FILE_ATTRIBUTE_SPARSE_FILE, L"AttributeSparse" },
            { FILE_ATTRIBUTE_REPARSE_POINT, L"AttributeReparsePoint" },
        };
        std::wstring result;
        for (const auto& entry : names)
        {
            if ((attributes & entry.flag) != 0)
            {
                const auto name = glance::app::localize(entry.name);
                result += result.empty() ? name : L", " + name;
            }
        }
        return result.empty() ? glance::app::localize(L"AttributeNormal") : result;
    }

    std::wstring owner_name(PSID owner)
    {
        DWORD account_size{};
        DWORD domain_size{};
        SID_NAME_USE use{};
        LookupAccountSidW(nullptr, owner, nullptr, &account_size, nullptr, &domain_size, &use);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return {};
        }
        std::wstring account(account_size, L'\0');
        std::wstring domain(domain_size, L'\0');
        if (!LookupAccountSidW(
                nullptr,
                owner,
                account.data(),
                &account_size,
                domain.data(),
                &domain_size,
                &use))
        {
            return {};
        }
        account.resize(account_size);
        domain.resize(domain_size);
        return domain.empty() ? account : domain + L"\\" + account;
    }

    std::wstring security_info(std::wstring_view path)
    {
        PSID owner{};
        PACL dacl{};
        PSECURITY_DESCRIPTOR descriptor{};
        if (GetNamedSecurityInfoW(
                const_cast<wchar_t*>(std::wstring(path).c_str()),
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &owner,
                nullptr,
                &dacl,
                nullptr,
                &descriptor) != ERROR_SUCCESS)
        {
            return {};
        }

        std::wstring result;
        if (owner != nullptr)
        {
            const auto owner_text = owner_name(owner);
            if (!owner_text.empty())
            {
                result = glance::app::localize_format(L"GenericOwner", { owner_text });
            }
        }

        HANDLE token{};
        DWORD token_size{};
        if (dacl != nullptr && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
            std::vector<std::byte> token_buffer(token_size);
            if (GetTokenInformation(token, TokenUser, token_buffer.data(), token_size, &token_size))
            {
                const auto token_user = reinterpret_cast<TOKEN_USER*>(token_buffer.data());
                TRUSTEEW trustee{};
                BuildTrusteeWithSidW(&trustee, token_user->User.Sid);
                ACCESS_MASK rights{};
                if (GetEffectiveRightsFromAclW(dacl, &trustee, &rights) == ERROR_SUCCESS)
                {
                    std::wstring access;
                    const auto append = [&access](wchar_t const* key) {
                        const auto value = glance::app::localize(key);
                        access += access.empty() ? value : L", " + value;
                    };
                    if ((rights & (FILE_READ_DATA | FILE_LIST_DIRECTORY | GENERIC_READ)) != 0) append(L"AccessRead");
                    if ((rights & (FILE_WRITE_DATA | FILE_ADD_FILE | GENERIC_WRITE)) != 0) append(L"AccessWrite");
                    if ((rights & (FILE_EXECUTE | FILE_TRAVERSE | GENERIC_EXECUTE)) != 0) append(L"AccessExecute");
                    if ((rights & DELETE) != 0) append(L"AccessDelete");
                    if (!access.empty())
                    {
                        const auto access_text = glance::app::localize_format(L"GenericAccess", { access });
                        result += result.empty() ? access_text : L"\n" + access_text;
                    }
                }
            }
            CloseHandle(token);
        }
        LocalFree(descriptor);
        return result;
    }
}

namespace glance::app
{
    std::wstring load_generic_file_info(std::wstring_view path) noexcept
    {
        try
        {
            std::wstring result;
            const auto attributes = file_attributes(path);
            if (!attributes.empty())
            {
                result = glance::app::localize_format(L"GenericAttributes", { attributes });
            }
            const auto security = security_info(path);
            if (!security.empty())
            {
                result += result.empty() ? security : L"\n" + security;
            }
            const auto header = file_header(path);
            if (!header.empty())
            {
                result += result.empty() ? header : L"\n\n" + header;
            }
            return result;
        }
        catch (...)
        {
            return {};
        }
    }
}
