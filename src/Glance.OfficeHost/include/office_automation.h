#pragma once

#include <windows.h>

#include <string>

namespace glance::office
{
    [[nodiscard]] int export_to_pdf(const std::wstring& input_path, const std::wstring& output_path);
    [[nodiscard]] int run_word_preview_session(
        const std::wstring& input_path,
        const std::wstring& cache_directory,
        HANDLE request_pipe,
        HANDLE response_pipe);
}
