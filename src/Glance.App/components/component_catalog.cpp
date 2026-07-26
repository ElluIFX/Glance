#include "pch.h"
#include "component_catalog.h"

#include "supported_components.generated.h"

namespace glance::app
{
    std::span<const SupportedComponent> supported_components() noexcept
    {
        return generated::supported_components;
    }

    const SupportedComponent* find_supported_component(std::wstring_view id) noexcept
    {
        for (const auto& component : generated::supported_components)
        {
            if (component.id == id)
            {
                return &component;
            }
        }
        return nullptr;
    }

    const SupportedComponent* find_component_for_extension(
        std::wstring_view extension) noexcept
    {
        for (const auto& component : generated::supported_components)
        {
            for (const auto supported_extension : component.extensions)
            {
                if (_wcsicmp(
                        std::wstring(extension).c_str(),
                        std::wstring(supported_extension).c_str()) == 0)
                {
                    return &component;
                }
            }
        }
        return nullptr;
    }
}
