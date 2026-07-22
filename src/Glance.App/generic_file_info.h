#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace glance::app
{
    [[nodiscard]] std::wstring load_generic_file_info(std::wstring_view path) noexcept;
    [[nodiscard]] std::optional<std::wstring> load_file_access_mode(std::wstring_view path) noexcept;
}
