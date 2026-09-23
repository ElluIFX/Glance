#pragma once
#include <winrt/Windows.Data.Json.h>
#include <string>

namespace glance::app
{
    winrt::Windows::Data::Json::JsonObject execute_settings_command(
        std::wstring_view command, winrt::Windows::Data::Json::JsonObject const& request);
}
