#pragma once
#include "pe_reader.h"
namespace glance::executable
{
    Table verify_signature(const std::wstring& path, const Cancelled& cancelled,
                           const std::wstring& worker_path = {});
    int signature_worker(const wchar_t* path, HANDLE output) noexcept;
} // namespace glance::executable
