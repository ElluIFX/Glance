#include "../../src/Glance.Components/Font/client.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

int run_font_tests()
{
    using namespace glance::font;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    const auto host =
        (std::filesystem::path(executable).parent_path() / L"components/font/Glance.FontHost.exe").wstring();
    wchar_t windows_directory[MAX_PATH]{};
    GetWindowsDirectoryW(windows_directory, MAX_PATH);
    const auto fonts = std::filesystem::path(windows_directory) / L"Fonts";
    std::filesystem::create_directories(std::filesystem::current_path() / L".tmp");
    int failed{};
    auto expect = [&](bool result, const char *label) {
        std::cout << (result ? "PASS " : "FAIL ") << label << std::endl;
        if (!result)
            ++failed;
    };
    try
    {
        for (const auto extension : {L".woff", L".woff2", L".otf"})
        {
            const auto fixture = std::filesystem::current_path() / L".tmp/font-fixtures" /
                                 (std::wstring(L"sample") + extension);
            if (!std::filesystem::exists(fixture))
            {
                std::wcout << L"SKIP optional font fixture " << extension << std::endl;
                continue;
            }
            Client fixture_font;
            fixture_font.open(host, fixture.wstring());
            const auto fixture_info = fixture_font.metadata(0);
            Request fixture_request;
            fixture_request.operation = Operation::render;
            const auto raster = fixture_font.render(fixture_request);
            std::wcout << L"Fixture " << extension << std::endl;
            expect(fixture_info->family[0] && !raster.second.empty(), "font container decodes and renders");
        }
        Client font;
        font.open(host, (fonts / L"segoeui.ttf").wstring());
        auto info = font.metadata(0);
        expect(info->count == 1 && info->family[0] && info->sample[0], "static font metadata and sample");
        Request request;
        request.operation = Operation::render;
        request.width = 640;
        request.height = 360;
        auto normal = font.render(request);
        std::size_t transparent{}, ink{};
        for (std::size_t i = 3; i < normal.second.size(); i += 4)
        {
            if (normal.second[i] == std::byte{})
                ++transparent;
            else
                ++ink;
        }
        expect(transparent > ink && ink > 0,
               "font rendering preserves transparent background and glyph pixels");
        request.size = 64;
        auto larger = font.render(request);
        expect(larger.first.content_height > normal.first.content_height, "font size changes layout");
        request.single = TRUE;
        request.size = 192;
        wcscpy_s(request.text, L"A");
        auto single = font.render(request);
        expect(!single.first.missing, "single character rendering");
        wcscpy_s(request.text, L"\U0010FFFF");
        expect(font.render(request).first.missing, "missing character is reported without font fallback");
        if (std::filesystem::exists(fonts / L"bahnschrift.ttf"))
        {
            Client variable;
            variable.open(host, (fonts / L"bahnschrift.ttf").wstring());
            auto variable_info = variable.metadata(0);
            expect(variable_info->variable && variable_info->minimum < variable_info->maximum,
                   "variable font weight range");
            request.weight = variable_info->minimum;
            wcscpy_s(request.text, L"A");
            auto light = variable.render(request);
            request.weight = variable_info->maximum;
            auto heavy = variable.render(request);
            expect(light.second != heavy.second, "variable weight changes actual glyph rendering");
        }
        else
            std::cout << "SKIP Bahnschrift is not installed\n";
        if (std::filesystem::exists(fonts / L"msyh.ttc"))
        {
            Client collection;
            collection.open(host, (fonts / L"msyh.ttc").wstring());
            auto collection_info = collection.metadata(0);
            expect(collection_info->count > 1, "font collection enumerates faces");
            auto second = collection.metadata(1);
            expect(second->selected == 1 && second->family[0], "font collection selects another face");
        }
        else
            std::cout << "SKIP Microsoft YaHei collection is not installed\n";
        const auto temporary = std::filesystem::current_path() / L".tmp" /
                               (L"font-invalid-" + std::to_wstring(GetCurrentProcessId()) + L".ttf");
        {
            std::ofstream file(temporary, std::ios::binary);
            file << "invalid font";
        }
        bool rejected{};
        try
        {
            Client invalid;
            invalid.open(host, temporary.wstring());
            static_cast<void>(invalid.metadata(0));
        }
        catch (...)
        {
            rejected = true;
        }
        std::filesystem::remove(temporary);
        expect(rejected, "invalid font fails in isolated host");
        font.cancel();
        bool cancelled{};
        try
        {
            static_cast<void>(font.render(request));
        }
        catch (...)
        {
            cancelled = true;
        }
        expect(cancelled, "cancelled font session rejects work");
    }
    catch (const winrt::hresult_error &error)
    {
        std::wcerr << error.message().c_str() << L" (" << std::hex << error.code().value << L")\n";
        ++failed;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        ++failed;
    }
    return failed ? 1 : 0;
}
