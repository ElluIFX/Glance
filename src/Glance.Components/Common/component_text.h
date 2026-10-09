#pragma once
#include "glance/contracts/component_api.h"

#include <algorithm>
#include <string_view>

namespace glance::components
{
    inline bool query_metadata_interface(const GUID* id, std::uint32_t version, void** output) noexcept
    {
        using namespace glance::contracts::components;
        if (!id || !output || !IsEqualGUID(*id, component_metadata_api_id) || version > component_metadata_api_version)
            return false;
        static ComponentMetadataApi metadata{
            .summary_key = L"ComponentMeta.Summary",
            .capabilities_key = L"ComponentMeta.Capabilities",
            .dependencies_key = L"ComponentMeta.Dependencies"};
        *output = &metadata;
        return true;
    }
    template <std::size_t Size>
    bool copy_resource_key(
        std::wstring_view key,
        wchar_t (&destination)[Size]) noexcept
    {
        if (key.empty() || key.size() >= Size)
        {
            return false;
        }
        std::copy(key.begin(), key.end(), destination);
        destination[key.size()] = L'\0';
        return true;
    }
}
