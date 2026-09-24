#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
namespace glance::executable
{
    class Resources
    {
      public:
        explicit Resources(std::wstring_view language = L"en-US");
        std::wstring text(std::wstring_view key) const;

      private:
        std::unordered_map<std::wstring, std::wstring> strings_;
    };
} // namespace glance::executable
