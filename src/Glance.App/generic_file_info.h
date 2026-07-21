#pragma once

#include <string>
#include <string_view>

namespace glance::app
{
    [[nodiscard]] std::wstring load_generic_file_info(std::wstring_view path) noexcept;
}
