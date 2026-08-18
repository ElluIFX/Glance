#pragma once

#include <windows.h>

namespace glance::office
{
    [[nodiscard]] int run_preview_handler_host(
        HANDLE request_pipe,
        HANDLE response_pipe,
        HANDLE cancellation_event);
}
