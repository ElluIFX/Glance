#include "engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <d2d1.h>
#include <dwrite_3.h>
#include <limits>
#include <sstream>
#include <wincodec.h>
#include <winrt/base.h>

namespace glance::font
{
namespace
{
using winrt::check_hresult;
template <std::size_t N> void copy(wchar_t (&target)[N], std::wstring_view text)
{
    const auto length = std::min(text.size(), N - 1);
    std::copy_n(text.data(), length, target);
    target[length] = 0;
}
struct MemoryStream : winrt::implements<MemoryStream, IDWriteFontFileStream>
{
    std::vector<std::byte> bytes;
    HRESULT __stdcall ReadFileFragment(const void **data, UINT64 offset, UINT64 length,
                                       void **context) noexcept override
    {
        *data = nullptr;
        *context = nullptr;
        if (offset > bytes.size() || length > bytes.size() - offset)
            return E_INVALIDARG;
        *data = bytes.data() + offset;
        return S_OK;
    }
    void __stdcall ReleaseFileFragment(void *) noexcept override
    {
    }
    HRESULT __stdcall GetFileSize(UINT64 *size) noexcept override
    {
        *size = bytes.size();
        return S_OK;
    }
    HRESULT __stdcall GetLastWriteTime(UINT64 *time) noexcept override
    {
        *time = 0;
        return E_NOTIMPL;
    }
};
struct Loader : winrt::implements<Loader, IDWriteFontFileLoader>
{
    winrt::com_ptr<IDWriteFontFileStream> stream;
    HRESULT __stdcall CreateStreamFromKey(const void *, UINT32,
                                          IDWriteFontFileStream **result) noexcept override
    {
        stream.copy_to(result);
        return S_OK;
    }
};
std::wstring localized(IDWriteLocalizedStrings *strings)
{
    if (!strings || !strings->GetCount())
        return {};
    UINT32 index{}, length{};
    BOOL exists{};
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    strings->FindLocaleName(locale, &index, &exists);
    if (!exists)
        strings->FindLocaleName(L"en-US", &index, &exists);
    if (!exists)
        index = 0;
    check_hresult(strings->GetStringLength(index, &length));
    if (length > 65536)
        return {};
    std::wstring text(length + 1, L'\0');
    check_hresult(strings->GetString(index, text.data(), length + 1));
    text.resize(length);
    return text;
}
std::wstring family(IDWriteFontFace3 *face)
{
    winrt::com_ptr<IDWriteLocalizedStrings> names;
    check_hresult(face->GetFamilyNames(names.put()));
    return localized(names.get());
}
std::wstring style(IDWriteFontFace3 *face)
{
    winrt::com_ptr<IDWriteLocalizedStrings> names;
    check_hresult(face->GetFaceNames(names.put()));
    return localized(names.get());
}
std::vector<UINT32> codepoints(std::wstring_view text)
{
    std::vector<UINT32> result;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        UINT32 c = text[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 &&
            text[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + text[++i] - 0xdc00;
        result.push_back(c);
    }
    return result;
}
bool supports(IDWriteFontFace *face, std::wstring_view text)
{
    auto points = codepoints(text);
    std::vector<UINT16> glyphs(points.size());
    check_hresult(face->GetGlyphIndices(points.data(), static_cast<UINT32>(points.size()), glyphs.data()));
    for (std::size_t i = 0; i < points.size(); ++i)
        if (!glyphs[i] && points[i] != 0x20 && points[i] != 0x09 && points[i] != 0x0a &&
            points[i] != 0x0d && points[i] != 0x200d &&
            !(points[i] >= 0xfe00 && points[i] <= 0xfe0f))
            return false;
    return true;
}
std::wstring sample(IDWriteFontFace1 *face)
{
    constexpr std::array samples{L"清风徐来，水波不兴。\n山川湖海，日月星辰。",
                                 L"The quick brown fox jumps over the lazy "
                                 L"dog.\nABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz",
                                 L"Съешь ещё этих мягких французских булок.",
                                 L"Ταχίστη αλώπηξ βαφής ψημένη γη.",
                                 L"いろはにほへと ちりぬるを",
                                 L"키스의 고유조건은 입술끼리 만나야 하고",
                                 L"العربية لغة جميلة",
                                 L"यह एक सुंदर भाषा है"};
    std::wstring result;
    for (const auto text : samples)
        if (supports(face, text))
        {
            result = text;
            break;
        }
    if (result == samples[0])
        result += L"\n" + std::wstring(samples[1]);
    if (result.empty())
    {
        UINT32 count{};
        face->GetUnicodeRanges(0, nullptr, &count);
        std::vector<DWRITE_UNICODE_RANGE> ranges(std::min(count, 65536U));
        if (!ranges.empty() &&
            SUCCEEDED(face->GetUnicodeRanges(static_cast<UINT32>(ranges.size()), ranges.data(), &count)))
            for (const auto &range : ranges)
            {
                for (UINT32 c = std::max(0x21U, range.first); c <= range.last && result.size() < 96; ++c)
                {
                    if (c >= 0xd800 && c <= 0xdfff)
                        continue;
                    if (c < 0x10000)
                        result += static_cast<wchar_t>(c);
                    else if (c <= 0x10ffff)
                    {
                        result += static_cast<wchar_t>(0xd800 + ((c - 0x10000) >> 10));
                        result += static_cast<wchar_t>(0xdc00 + ((c - 0x10000) & 1023));
                    }
                    result += L' ';
                }
                if (result.size() >= 96)
                    break;
            }
    }
    if (supports(face, L"0123456789"))
        result += L"\n0123456789";
    if (supports(face, L".,:;!? () [] {} +−= @#%&"))
        result += L"\n.,:;!? () [] {} +−= @#%&";
    return result;
}
} // namespace
struct Engine::State
{
    winrt::com_ptr<IDWriteFactory6> factory;
    winrt::com_ptr<Loader> loader;
    winrt::com_ptr<IDWriteFontFile> file;
    DWRITE_FONT_FACE_TYPE type{};
    unsigned count{};
    unsigned selected{UINT_MAX};
    Metadata info{};
    winrt::com_ptr<IDWriteFontFace5> face;
    winrt::com_ptr<IDWriteFontResource> resource;
    std::vector<DWRITE_FONT_AXIS_VALUE> axes;
    winrt::com_ptr<ID2D1Factory> drawing;
    winrt::com_ptr<IWICImagingFactory> imaging;
    ~State()
    {
        if (factory && loader)
            factory->UnregisterFontFileLoader(loader.get());
    }
    winrt::com_ptr<IDWriteFontFace5> load(unsigned index)
    {
        if (index >= count)
            winrt::throw_hresult(E_INVALIDARG);
        auto *raw = file.get();
        winrt::com_ptr<IDWriteFontFace> value;
        check_hresult(
            factory->CreateFontFace(type, 1, &raw, index, DWRITE_FONT_SIMULATIONS_NONE, value.put()));
        return value.as<IDWriteFontFace5>();
    }
};
Engine::Engine(const std::wstring &path) : state_(std::make_unique<State>())
{
    auto &s = *state_;
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory6),
                                      reinterpret_cast<IUnknown **>(s.factory.put())));
    auto stream = winrt::make_self<MemoryStream>();
    {
        winrt::handle input(CreateFileW(path.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input)
            winrt::throw_last_error();
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(input.get(), &size))
            winrt::throw_last_error();
        if (size.QuadPart < 12 || size.QuadPart > maximum_file_size)
            winrt::throw_hresult(E_INVALIDARG);
        stream->bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read{};
        if (!ReadFile(input.get(), stream->bytes.data(), static_cast<DWORD>(stream->bytes.size()), &read,
                      nullptr) ||
            read != stream->bytes.size())
            winrt::throw_hresult(E_FAIL);
    }
    s.loader = winrt::make_self<Loader>();
    const auto container =
        s.factory->AnalyzeContainerType(stream->bytes.data(), static_cast<UINT32>(stream->bytes.size()));
    if (container != DWRITE_CONTAINER_TYPE_UNKNOWN)
    {
        if (stream->bytes.size() < 20)
            winrt::throw_hresult(E_INVALIDARG);
        const auto *b = reinterpret_cast<const unsigned char *>(stream->bytes.data());
        const UINT32 unpacked = (UINT32(b[16]) << 24) | (UINT32(b[17]) << 16) | (UINT32(b[18]) << 8) | b[19];
        if (!unpacked || unpacked > maximum_file_size)
            winrt::throw_hresult(E_INVALIDARG);
        check_hresult(s.factory->UnpackFontFile(container, stream->bytes.data(),
                                                static_cast<UINT32>(stream->bytes.size()),
                                                s.loader->stream.put()));
        UINT64 actual{};
        check_hresult(s.loader->stream->GetFileSize(&actual));
        if (actual > maximum_file_size)
            winrt::throw_hresult(E_INVALIDARG);
    }
    else
        s.loader->stream = stream.as<IDWriteFontFileStream>();
    check_hresult(s.factory->RegisterFontFileLoader(s.loader.get()));
    const UINT32 key = 1;
    check_hresult(s.factory->CreateCustomFontFileReference(&key, sizeof(key), s.loader.get(), s.file.put()));
    BOOL supported{};
    DWRITE_FONT_FILE_TYPE file_type{};
    check_hresult(s.file->Analyze(&supported, &file_type, &s.type, &s.count));
    if (!supported || !s.count || s.count > maximum_faces)
        winrt::throw_hresult(E_INVALIDARG);
    s.info.count = s.count;
    for (unsigned i = 0; i < s.count; ++i)
    {
        auto face = s.load(i);
        copy(s.info.faces[i], family(face.get()) + L" · " + style(face.get()));
    }
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, s.drawing.put()));
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory), s.imaging.put_void()));
}
Engine::~Engine() = default;
Metadata Engine::metadata(unsigned index)
{
    auto &s = *state_;
    if (s.selected == index)
        return s.info;
    auto face = s.load(index);
    s.face = face;
    s.selected = index;
    s.info.selected = index;
    s.info.entry_count = 0;
    copy(s.info.family, family(face.get()));
    copy(s.info.style, style(face.get()));
    copy(s.info.sample, sample(face.get()));
    s.resource = nullptr;
    check_hresult(face->GetFontResource(s.resource.put()));
    s.axes.resize(s.resource->GetFontAxisCount());
    check_hresult(s.resource->GetDefaultFontAxisValues(s.axes.data(), static_cast<UINT32>(s.axes.size())));
    std::vector<DWRITE_FONT_AXIS_RANGE> ranges(s.axes.size());
    check_hresult(s.resource->GetFontAxisRanges(ranges.data(), static_cast<UINT32>(ranges.size())));
    s.info.variable = FALSE;
    s.info.weight = static_cast<float>(face->GetWeight());
    auto add = [&](const wchar_t *key, const std::wstring &value) {
        if (value.empty() || s.info.entry_count >= std::size(s.info.entries))
            return;
        auto &row = s.info.entries[s.info.entry_count++];
        copy(row.key, key);
        copy(row.value, value);
    };
    constexpr std::array ids{
        DWRITE_INFORMATIONAL_STRING_FULL_NAME,          DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,
        DWRITE_INFORMATIONAL_STRING_VERSION_STRINGS,    DWRITE_INFORMATIONAL_STRING_DESIGNER,
        DWRITE_INFORMATIONAL_STRING_MANUFACTURER,       DWRITE_INFORMATIONAL_STRING_COPYRIGHT_NOTICE,
        DWRITE_INFORMATIONAL_STRING_LICENSE_DESCRIPTION};
    constexpr std::array keys{L"FullName",     L"PostScript", L"Version", L"Designer",
                              L"Manufacturer", L"Copyright",  L"License"};
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        winrt::com_ptr<IDWriteLocalizedStrings> text;
        BOOL exists{};
        if (SUCCEEDED(face->GetInformationalStrings(ids[i], text.put(), &exists)) && exists)
            add(keys[i], localized(text.get()));
    }
    add(L"Weight", std::to_wstring(face->GetWeight()));
    add(L"Stretch", std::to_wstring(face->GetStretch()));
    add(L"Glyphs", std::to_wstring(face->GetGlyphCount()));
    std::wstring axes;
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        const auto &range = ranges[i];
        if (range.minValue == range.maxValue)
            continue;
        if (range.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT)
        {
            s.info.variable = TRUE;
            s.info.minimum = range.minValue;
            s.info.maximum = range.maxValue;
            s.info.weight = s.axes[i].value;
        }
        if (!axes.empty())
            axes += L"\n";
        for (unsigned b = 0; b < 4; ++b)
            axes += wchar_t((range.axisTag >> (b * 8)) & 255);
        std::wostringstream value;
        value << L"  " << range.minValue << L" – " << range.maxValue << L" (" << s.axes[i].value << L")";
        axes += value.str();
    }
    add(L"Axes", axes);
    return s.info;
}
Raster Engine::render(const Request &request)
{
    if (!std::isfinite(request.size) || !std::isfinite(request.scale) || !std::isfinite(request.weight) ||
        !std::isfinite(request.offset) || request.width < 1 || request.width > 4096 || request.height < 1 ||
        request.height > 4096 || request.scale < 0.5f || request.scale > 8 || request.size < 8 ||
        request.size > 512 || request.offset < 0)
        winrt::throw_hresult(E_INVALIDARG);
    metadata(request.face);
    auto &s = *state_;
    auto axes = s.axes;
    for (auto &axis : axes)
        if (axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT && s.info.variable)
            axis.value = std::clamp(request.weight, s.info.minimum, s.info.maximum);
    winrt::com_ptr<IDWriteFontFaceReference1> reference;
    check_hresult(s.resource->CreateFontFaceReference(DWRITE_FONT_SIMULATIONS_NONE, axes.data(),
                                                      static_cast<UINT32>(axes.size()), reference.put()));
    winrt::com_ptr<IDWriteFontSetBuilder> builder;
    check_hresult(s.factory->CreateFontSetBuilder(builder.put()));
    check_hresult(builder->AddFontFaceReference(reference.get()));
    winrt::com_ptr<IDWriteFontSet> set;
    check_hresult(builder->CreateFontSet(set.put()));
    winrt::com_ptr<IDWriteFontCollection1> collection;
    check_hresult(s.factory->CreateFontCollectionFromFontSet(set.get(), collection.put()));
    winrt::com_ptr<IDWriteTextFormat> format;
    check_hresult(s.factory->CreateTextFormat(s.info.family, collection.get(), s.face->GetWeight(),
                                              s.face->GetStyle(), s.face->GetStretch(),
                                              request.size * 96.0f / 72.0f, L"", format.put()));
    auto variable = format.as<IDWriteTextFormat3>();
    check_hresult(variable->SetFontAxisValues(axes.data(), static_cast<UINT32>(axes.size())));
    winrt::com_ptr<IDWriteFontFallbackBuilder> fallback_builder;
    check_hresult(s.factory->CreateFontFallbackBuilder(fallback_builder.put()));
    winrt::com_ptr<IDWriteFontFallback> fallback;
    check_hresult(fallback_builder->CreateFontFallback(fallback.put()));
    check_hresult(variable->SetFontFallback(fallback.get()));
    const std::wstring text =
        request.custom_text ? std::wstring(request.text, wcsnlen_s(request.text, std::size(request.text)))
                       : s.info.sample;
    winrt::com_ptr<IDWriteTextLayout> layout;
    const float width = request.width / request.scale, height = request.height / request.scale;
    check_hresult(s.factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
                                              width, 100000.0f, layout.put()));
    DWRITE_TEXT_METRICS metrics{};
    check_hresult(layout->GetMetrics(&metrics));
    Raster raster;
    raster.info.width = request.width;
    raster.info.height = request.height;
    raster.info.content_height = std::max(height, metrics.height + 24);
    raster.info.missing = !supports(s.face.get(), text);
    winrt::com_ptr<IWICBitmap> bitmap;
    check_hresult(s.imaging->CreateBitmap(request.width, request.height, GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapCacheOnLoad, bitmap.put()));
    winrt::com_ptr<ID2D1RenderTarget> target;
    auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * request.scale,
        96 * request.scale);
    check_hresult(s.drawing->CreateWicBitmapRenderTarget(bitmap.get(), properties, target.put()));
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(request.color & 0xffffff), brush.put()));
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->DrawTextLayout(D2D1::Point2F(0, -request.offset), layout.get(), brush.get(),
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    check_hresult(target->EndDraw());
    raster.pixels.resize(static_cast<std::size_t>(request.width) * request.height * 4);
    raster.info.bytes = static_cast<unsigned>(raster.pixels.size());
    check_hresult(bitmap->CopyPixels(nullptr, request.width * 4, raster.info.bytes,
                                     reinterpret_cast<BYTE *>(raster.pixels.data())));
    return raster;
}
} // namespace glance::font
