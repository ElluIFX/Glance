#include "input_decision.h"
#include "pan_interaction.h"
#include "text_font_fallback.h"

#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    int failures{};

    void expect(bool condition, std::string_view name)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << name << '\n';
            ++failures;
        }
    }
}

int main()
{
    using glance::core::should_capture_key;

    expect(should_capture_key(VK_SPACE, true, false, true, false, false), "eligible Space");
    expect(should_capture_key(VK_SPACE, true, true, false, false, false), "active Space");
    expect(!should_capture_key(VK_SPACE, false, false, true, false, false), "disconnected Space");
    expect(!should_capture_key(VK_SPACE, true, false, false, false, false), "ineligible Space");
    expect(!should_capture_key(VK_SPACE, true, false, true, false, true), "modified Space");
    expect(!should_capture_key(VK_SPACE, true, false, true, true, false), "eligible text input Space");
    expect(!should_capture_key(VK_SPACE, true, true, false, true, false), "active text input Space");

    expect(should_capture_key(VK_ESCAPE, true, true, false, true, false), "active Escape");
    expect(!should_capture_key(VK_ESCAPE, true, false, true, false, false), "inactive Escape");
    expect(!should_capture_key(VK_ESCAPE, true, true, false, false, true), "modified Escape");
    expect(!should_capture_key('A', true, true, true, false, false), "unrelated key");

    expect(!glance::app::zoom_allows_pan(1.0F), "fit zoom does not pan");
    expect(!glance::app::zoom_allows_pan(1.001F), "zoom tolerance does not pan");
    expect(glance::app::zoom_allows_pan(1.01F), "enlarged preview pans");

    const auto offsets = glance::app::calculate_pan_offsets(
        { 120.0, 80.0 },
        { 300.0, 200.0 },
        { 260.0, 230.0 });
    expect(std::abs(offsets.horizontal - 160.0) < 0.001, "horizontal pan offset");
    expect(std::abs(offsets.vertical - 50.0) < 0.001, "vertical pan offset");

    const auto clamped_offsets = glance::app::calculate_pan_offsets(
        { 10.0, 10.0 },
        { 20.0, 20.0 },
        { 100.0, 100.0 });
    expect(clamped_offsets.horizontal == 0.0, "horizontal pan clamp");
    expect(clamped_offsets.vertical == 0.0, "vertical pan clamp");

    const std::vector<std::wstring> fonts_without_cascadia{
        L"Arial",
        L"consolas",
        L"Courier New"
    };
    expect(
        glance::app::select_default_text_font_family(fonts_without_cascadia) == L"consolas",
        "font fallback prefers Consolas");

    const std::vector<std::wstring> fonts_without_consolas{
        L"Arial",
        L"Courier New",
        L"Lucida Console"
    };
    expect(
        glance::app::select_default_text_font_family(fonts_without_consolas) == L"Courier New",
        "font fallback prefers Courier New");

    const std::vector<std::wstring> no_fonts;
    expect(
        glance::app::select_default_text_font_family(no_fonts) == L"Cascadia Mono",
        "font fallback keeps primary default");

    if (failures == 0)
    {
        std::cout << "All regression tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
