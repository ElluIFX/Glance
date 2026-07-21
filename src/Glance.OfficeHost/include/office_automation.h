#pragma once

#include <string>

namespace glance::office
{
    [[nodiscard]] int export_to_pdf(const std::wstring& input_path, const std::wstring& output_path);
}
