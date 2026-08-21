#pragma once

#include <initializer_list>
#include <filesystem>
#include <string>
#include <string_view>

namespace glance::app
{
    [[nodiscard]] std::wstring resolve_ui_language(std::wstring_view saved_language);
    void apply_ui_language(std::wstring_view language);
    [[nodiscard]] std::wstring current_ui_language();
    [[nodiscard]] std::wstring localize(std::wstring_view key);
    [[nodiscard]] std::wstring localize_format(
        std::wstring_view key,
        std::initializer_list<std::wstring_view> arguments);
    [[nodiscard]] bool register_component_resources(
        std::wstring_view component_id,
        const std::filesystem::path& resource_path) noexcept;
    void unregister_component_resources(std::wstring_view component_id) noexcept;
    [[nodiscard]] std::wstring localize_component(
        std::wstring_view component_id,
        std::wstring_view key);
}
