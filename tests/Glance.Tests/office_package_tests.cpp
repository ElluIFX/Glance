#include "../../src/Glance.Components/Office/host/include/presentation_package.h"
#include <msopc.h>
#include <shlwapi.h>
#include <filesystem>
#include <iostream>
#include <vector>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    std::vector<BYTE> read(IStream* stream, ULONG length)
    {
        std::vector<BYTE> bytes(length); ULONG count{};
        winrt::check_hresult(stream->Read(bytes.data(), length, &count)); bytes.resize(count); return bytes;
    }
}

int run_office_package_tests()
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::path directory;
    int result = 1;
    try
    {
        winrt::check_hresult(initialized);
        GUID id{}; winrt::check_hresult(CoCreateGuid(&id));
        wchar_t name[40]{}; StringFromGUID2(id, name, 40);
        directory = std::filesystem::current_path() / L".tmp" / (std::wstring(L"office-package-") + name);
        std::filesystem::create_directories(directory);
        const auto path = directory / L"fixture.pptx";
        std::vector<BYTE> expected(8192);
        for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = static_cast<BYTE>(index % 251);
        auto factory = winrt::create_instance<IOpcFactory>(__uuidof(OpcFactory));
        {
            winrt::com_ptr<IOpcPackage> package; winrt::check_hresult(factory->CreatePackage(package.put()));
            winrt::com_ptr<IOpcPartSet> parts; winrt::check_hresult(package->GetPartSet(parts.put()));
            for (int index = 0; index < 3; ++index)
            {
                const std::wstring part_name = index == 0 ? L"/slides/slide.xml" : L"/media/video" + std::to_wstring(index) + L".mp4";
                winrt::com_ptr<IOpcPartUri> uri; winrt::check_hresult(factory->CreatePartUri(part_name.c_str(), uri.put()));
                winrt::com_ptr<IOpcPart> part; winrt::check_hresult(parts->CreatePart(uri.get(), index ? L"video/mp4" : L"application/xml",
                    index == 1 ? OPC_COMPRESSION_NONE : OPC_COMPRESSION_NORMAL, part.put()));
                winrt::com_ptr<IStream> content; winrt::check_hresult(part->GetContentStream(content.put()));
                ULONG written{}; winrt::check_hresult(content->Write(expected.data(), static_cast<ULONG>(expected.size()), &written));
                if (index == 0)
                {
                    winrt::com_ptr<IOpcRelationshipSet> relationships; winrt::check_hresult(part->GetRelationshipSet(relationships.put()));
                    for (int media = 1; media <= 2; ++media)
                    {
                        winrt::com_ptr<IUri> target;
                        winrt::check_hresult(CreateUri((L"../media/video" + std::to_wstring(media) + L".mp4").c_str(), Uri_CREATE_ALLOW_RELATIVE, 0, target.put()));
                        winrt::com_ptr<IOpcRelationship> relation;
                        winrt::check_hresult(relationships->CreateRelationship(nullptr, L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/video",
                            target.get(), OPC_URI_TARGET_MODE_INTERNAL, relation.put()));
                    }
                }
            }
            winrt::com_ptr<IStream> output; winrt::check_hresult(SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_CREATE | STGM_SHARE_EXCLUSIVE,
                FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, output.put()));
            winrt::check_hresult(factory->WritePackageToStream(package.get(), OPC_WRITE_DEFAULT, output.get()));
        }
        glance::office::PresentationPackage preview(path.wstring());
        const auto projected = preview.project();
        {
            winrt::com_ptr<IStream> stream; stream.attach(SHCreateMemStream(projected.data(), static_cast<UINT>(projected.size())));
            winrt::com_ptr<IOpcPackage> package; winrt::check_hresult(factory->ReadPackageFromStream(stream.get(), OPC_READ_DEFAULT, package.put()));
            winrt::com_ptr<IOpcPartSet> parts; winrt::check_hresult(package->GetPartSet(parts.put()));
            winrt::com_ptr<IOpcPartEnumerator> items; winrt::check_hresult(parts->GetEnumerator(items.put()));
            BOOL has{}; winrt::check_hresult(items->MoveNext(&has)); require(has, "Projected slide missing");
            winrt::com_ptr<IOpcPart> part; winrt::check_hresult(items->GetCurrent(part.put()));
            winrt::com_ptr<IStream> content; winrt::check_hresult(part->GetContentStream(content.put()));
            require(read(content.get(), 8192) == expected, "Projection changed non-media content");
            winrt::check_hresult(items->MoveNext(&has)); require(!has, "Projected package retained media bytes");
            winrt::com_ptr<IOpcRelationshipSet> relationships; winrt::check_hresult(part->GetRelationshipSet(relationships.put()));
            winrt::com_ptr<IOpcRelationshipEnumerator> relations; winrt::check_hresult(relationships->GetEnumerator(relations.put()));
            int count = 0;
            for (;;)
            {
                winrt::check_hresult(relations->MoveNext(&has)); if (!has) break;
                winrt::com_ptr<IOpcRelationship> relation; winrt::check_hresult(relations->GetCurrent(relation.put()));
                OPC_URI_TARGET_MODE mode{}; winrt::check_hresult(relation->GetTargetMode(&mode));
                require(mode == OPC_URI_TARGET_MODE_EXTERNAL, "Media relation was not externalized"); ++count;
            }
            require(count == 2, "Media relationships were lost");
        }
        for (std::size_t index = 0; index < 2; ++index)
        {
            auto full = preview.media(index, L"", false);
            require(full.status == 200 && read(full.stream.get(), 9000) == expected, "Stored or compressed media changed");
            auto range = preview.media(index, L"bytes=100-299", false);
            require(range.status == 206 && read(range.stream.get(), 9000) == std::vector<BYTE>(expected.begin() + 100, expected.begin() + 300), "Media range boundaries failed");
            auto suffix = preview.media(index, L"bytes=-16", false);
            require(read(suffix.stream.get(), 9000) == std::vector<BYTE>(expected.end() - 16, expected.end()), "Suffix range failed");
            require(!preview.media(index, L"", true).stream, "HEAD created a body");
        }
        for (const auto* range : { L"bytes=8192-", L"bytes=20-10", L"bytes=0-1-2", L"bytes=0-2,5-7", L"bytes=-0" })
            require(preview.media(0, range, false).status == 416, "Malformed range accepted");
        require(preview.media(5, L"", false).status == 404, "Unknown media index accepted");
        auto retained = preview.media(0, L"bytes=0-31", false);
        {
            winrt::handle exclusive(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            require(exclusive.get() != INVALID_HANDLE_VALUE, "Preview retained a source file handle");
            FILETIME changed{}; GetSystemTimeAsFileTime(&changed); winrt::check_bool(SetFileTime(exclusive.get(), nullptr, nullptr, &changed));
        }
        BYTE value{}; ULONG count{};
        require(FAILED(retained.stream->Read(&value, 1, &count)), "Changed source was read through old preview");
        glance::office::PresentationPackage cancelled(path.wstring()); cancelled.cancel();
        bool rejected = false; try { static_cast<void>(cancelled.project()); } catch (...) { rejected = true; }
        require(rejected, "Early cancellation ignored");
        preview.cancel();
        require(FAILED(retained.stream->Read(&value, 1, &count)), "Cancelled media read succeeded");
        std::filesystem::remove_all(directory);
        std::cout << "Office package regression tests passed\n";
        result = 0;
    }
    catch (const winrt::hresult_error& error) { std::wcerr << L"Office package failure: " << error.message().c_str() << L"\n"; }
    catch (const std::exception& error) { std::cerr << "Office package failure: " << error.what() << '\n'; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
