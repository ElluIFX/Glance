#pragma once
#include "pe_reader.h"
#include <memory>
namespace glance::executable
{
    struct ResourceImage
    {
        std::shared_ptr<void> bitmap;
        int width{}, height{};
    };
    ResourceImage resource_image(const std::wstring& path, const Identity& identity, const Row& row);
    void save_image(const std::wstring& path, const Identity& identity, const Row& row,
                    const std::wstring& destination);
} // namespace glance::executable
