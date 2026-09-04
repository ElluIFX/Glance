#pragma once

#include <windows.h>

namespace glance::panorama
{
    int run_player_host(
        HANDLE request_pipe,
        HANDLE response_pipe,
        HANDLE cancellation_event);
}
