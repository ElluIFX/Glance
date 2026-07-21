#include "input_decision.h"

#include <iostream>
#include <string_view>

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

    expect(should_capture_key(VK_SPACE, true, false, true, false), "eligible Space");
    expect(should_capture_key(VK_SPACE, true, true, false, false), "active Space");
    expect(!should_capture_key(VK_SPACE, false, false, true, false), "disconnected Space");
    expect(!should_capture_key(VK_SPACE, true, false, false, false), "ineligible Space");
    expect(!should_capture_key(VK_SPACE, true, false, true, true), "modified Space");

    expect(should_capture_key(VK_ESCAPE, true, true, false, false), "active Escape");
    expect(!should_capture_key(VK_ESCAPE, true, false, true, false), "inactive Escape");
    expect(!should_capture_key(VK_ESCAPE, true, true, false, true), "modified Escape");
    expect(!should_capture_key('A', true, true, true, false), "unrelated key");

    if (failures == 0)
    {
        std::cout << "All input decision tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
