#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace glance::app
{
    struct GenericInformationField
    {
        std::wstring label_key;
        std::wstring value;
        std::vector<std::wstring> value_keys;
    };
    struct GenericFileInfo
    {
        std::vector<GenericInformationField> fields;
        std::wstring header;
        std::uint32_t header_size{};
    };
    [[nodiscard]] GenericFileInfo load_generic_file_info(std::wstring_view path) noexcept;
    [[nodiscard]] std::optional<std::wstring> load_file_access_mode(std::wstring_view path) noexcept;
}
