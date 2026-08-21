#pragma once

#include <algorithm>
#include <string_view>

namespace glance::components
{
    template <std::size_t Size>
    bool copy_resource_key(
        std::wstring_view key,
        wchar_t (&destination)[Size]) noexcept
    {
        if (key.empty() || key.size() >= Size)
        {
            return false;
        }
        std::copy(key.begin(), key.end(), destination);
        destination[key.size()] = L'\0';
        return true;
    }
}
