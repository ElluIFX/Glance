#pragma once

#include <string>

namespace glance::app
{
    [[nodiscard]] std::wstring load_image_metadata(const std::wstring& path);
}
