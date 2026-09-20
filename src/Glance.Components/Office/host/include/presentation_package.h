#pragma once

#include <windows.h>
#include <objidl.h>
#include <winrt/base.h>
#include <memory>
#include <string>
#include <vector>

namespace glance::office
{
    struct PresentationMediaResponse
    {
        winrt::com_ptr<IStream> stream;
        int status{ 404 };
        std::wstring headers;
    };

    class PresentationPackage
    {
    public:
        explicit PresentationPackage(std::wstring path);
        std::vector<BYTE> project();
        PresentationMediaResponse media(std::size_t index, const std::wstring& range, bool head);
        void cancel() noexcept;
    private:
        struct State;
        std::shared_ptr<State> state_;
    };
}
