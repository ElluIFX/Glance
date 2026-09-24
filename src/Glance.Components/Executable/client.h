#pragma once
#include "protocol.h"
#include <atomic>
namespace glance::executable
{
Result inspect(const std::wstring& host, const Query& query, const std::atomic_bool& cancelled);
}
