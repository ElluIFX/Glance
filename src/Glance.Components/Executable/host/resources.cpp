#include "resources.h"
#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <stdexcept>

namespace glance::executable
{
    Resources::Resources(std::wstring_view language)
    {
        const auto id = LocaleNameToLCID(std::wstring(language).c_str(), 0);
        const auto resource = FindResourceExW(nullptr, RT_RCDATA, MAKEINTRESOURCEW(101), LANGIDFROMLCID(id));
        if (!resource)
            throw std::runtime_error("Missing localization resource");
        const auto loaded = LoadResource(nullptr, resource);
        const auto data = static_cast<const BYTE*>(LockResource(loaded));
        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(data, SizeofResource(nullptr, resource)));
        Microsoft::WRL::ComPtr<IXmlReader> reader;
        if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), &reader, nullptr)) ||
            FAILED(reader->SetInput(stream.Get())))
            throw std::runtime_error("Invalid localization resource");
        reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
        XmlNodeType type{};
        std::wstring key;
        bool value{};
        while (reader->Read(&type) == S_OK)
        {
            const wchar_t* text{};
            if (type == XmlNodeType_Element)
            {
                reader->GetLocalName(&text, nullptr);
                value = wcscmp(text, L"value") == 0;
                if (wcscmp(text, L"data") == 0 && SUCCEEDED(reader->MoveToAttributeByName(L"name", nullptr)))
                {
                    reader->GetValue(&text, nullptr);
                    key = text;
                    reader->MoveToElement();
                }
            }
            else if (type == XmlNodeType_Text && value)
            {
                reader->GetValue(&text, nullptr);
                strings_[key] += text;
            }
            else if (type == XmlNodeType_EndElement)
                value = false;
        }
    }
    std::wstring Resources::text(std::wstring_view key) const
    {
        const auto found = strings_.find(std::wstring(key));
        return found == strings_.end() ? std::wstring(key) : found->second;
    }
} // namespace glance::executable
