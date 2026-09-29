#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace glance::app
{
    struct GenericFileInfo
    {
        std::wstring metadata;
        std::wstring header;
    };
    [[nodiscard]] GenericFileInfo load_generic_file_info(std::wstring_view path) noexcept;
    [[nodiscard]] std::optional<std::wstring> load_file_access_mode(std::wstring_view path) noexcept;
}
