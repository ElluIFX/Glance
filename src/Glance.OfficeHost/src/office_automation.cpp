#include "../include/office_automation.h"

#include <windows.h>
#include <oaidl.h>

#include "glance/contracts/office_preview_protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <winrt/base.h>

namespace
{
    using namespace glance::contracts::office;

    bool read_exact(HANDLE handle, void* destination, std::size_t size)
    {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size != 0)
        {
            DWORD read{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(handle, bytes, request, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE handle, const void* source, std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(source);
        while (size != 0)
        {
            DWORD written{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(handle, bytes, request, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    template <typename T>
    void append_value(std::vector<std::byte>& output, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), begin, begin + sizeof(T));
    }

    void append_utf16(std::vector<std::byte>& output, std::wstring_view value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        output.insert(output.end(), begin, begin + value.size() * sizeof(wchar_t));
    }

    VARIANTARG string_argument(const std::wstring& value)
    {
        VARIANTARG argument;
        VariantInit(&argument);
        argument.vt = VT_BSTR;
        argument.bstrVal = SysAllocString(value.c_str());
        return argument;
    }

    VARIANTARG integer_argument(LONG value)
    {
        VARIANTARG argument;
        VariantInit(&argument);
        argument.vt = VT_I4;
        argument.lVal = value;
        return argument;
    }

    VARIANTARG boolean_argument(bool value)
    {
        VARIANTARG argument;
        VariantInit(&argument);
        argument.vt = VT_BOOL;
        argument.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        return argument;
    }

    HRESULT invoke(
        IDispatch* object,
        const wchar_t* member_name,
        WORD flags,
        VARIANT* result,
        std::vector<VARIANTARG> arguments = {})
    {
        LPOLESTR name = const_cast<LPOLESTR>(member_name);
        DISPID member{};
        HRESULT status = object->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &member);
        if (FAILED(status))
        {
            for (auto& argument : arguments)
            {
                VariantClear(&argument);
            }
            return status;
        }

        std::ranges::reverse(arguments);
        DISPPARAMS parameters{};
        parameters.rgvarg = arguments.data();
        parameters.cArgs = static_cast<UINT>(arguments.size());
        DISPID property_put = DISPID_PROPERTYPUT;
        if ((flags & DISPATCH_PROPERTYPUT) != 0)
        {
            parameters.rgdispidNamedArgs = &property_put;
            parameters.cNamedArgs = 1;
        }
        EXCEPINFO exception{};
        UINT argument_error{};
        status = object->Invoke(
            member,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            flags,
            &parameters,
            result,
            &exception,
            &argument_error);
        for (auto& argument : arguments)
        {
            VariantClear(&argument);
        }
        SysFreeString(exception.bstrSource);
        SysFreeString(exception.bstrDescription);
        SysFreeString(exception.bstrHelpFile);
        return status;
    }

    winrt::com_ptr<IDispatch> dispatch_result(VARIANT& value)
    {
        winrt::com_ptr<IDispatch> result;
        if (value.vt == VT_DISPATCH && value.pdispVal != nullptr)
        {
            result.attach(value.pdispVal);
            value.vt = VT_EMPTY;
            value.pdispVal = nullptr;
        }
        return result;
    }

    winrt::com_ptr<IDispatch> get_dispatch_property(IDispatch* object, const wchar_t* name)
    {
        VARIANT result;
        VariantInit(&result);
        if (FAILED(invoke(object, name, DISPATCH_PROPERTYGET, &result)))
        {
            VariantClear(&result);
            return {};
        }
        auto dispatch = dispatch_result(result);
        VariantClear(&result);
        return dispatch;
    }

    winrt::com_ptr<IDispatch> invoke_dispatch_method(
        IDispatch* object,
        const wchar_t* name,
        std::vector<VARIANTARG> arguments)
    {
        VARIANT result;
        VariantInit(&result);
        if (FAILED(invoke(object, name, DISPATCH_METHOD, &result, std::move(arguments))))
        {
            VariantClear(&result);
            return {};
        }
        auto dispatch = dispatch_result(result);
        VariantClear(&result);
        return dispatch;
    }

    winrt::com_ptr<IDispatch> invoke_dispatch_member(
        IDispatch* object,
        const wchar_t* name,
        std::vector<VARIANTARG> arguments)
    {
        VARIANT result;
        VariantInit(&result);
        if (FAILED(invoke(
                object,
                name,
                DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                &result,
                std::move(arguments))))
        {
            VariantClear(&result);
            return {};
        }
        auto dispatch = dispatch_result(result);
        VariantClear(&result);
        return dispatch;
    }

    HRESULT invoke_method(
        IDispatch* object,
        const wchar_t* name,
        std::vector<VARIANTARG> arguments = {})
    {
        VARIANT result;
        VariantInit(&result);
        const HRESULT status = invoke(object, name, DISPATCH_METHOD, &result, std::move(arguments));
        VariantClear(&result);
        return status;
    }

    HRESULT set_property(IDispatch* object, const wchar_t* name, VARIANTARG value)
    {
        VARIANT result;
        VariantInit(&result);
        const HRESULT status = invoke(object, name, DISPATCH_PROPERTYPUT, &result, { value });
        VariantClear(&result);
        return status;
    }

    winrt::com_ptr<IDispatch> create_application(const wchar_t* programmatic_id)
    {
        CLSID class_id{};
        if (FAILED(CLSIDFromProgID(programmatic_id, &class_id)))
        {
            return {};
        }
        winrt::com_ptr<IDispatch> application;
        if (FAILED(CoCreateInstance(
                class_id,
                nullptr,
                CLSCTX_LOCAL_SERVER,
                IID_PPV_ARGS(application.put()))))
        {
            return {};
        }
        return application;
    }

    bool numeric_property(IDispatch* object, const wchar_t* name, double& value)
    {
        VARIANT result;
        VariantInit(&result);
        if (FAILED(invoke(object, name, DISPATCH_PROPERTYGET, &result)))
        {
            VariantClear(&result);
            return false;
        }
        VARIANT converted;
        VariantInit(&converted);
        const HRESULT status = VariantChangeType(&converted, &result, 0, VT_R8);
        VariantClear(&result);
        if (FAILED(status))
        {
            VariantClear(&converted);
            return false;
        }
        value = converted.dblVal;
        VariantClear(&converted);
        return true;
    }

    bool write_byte_array(const std::filesystem::path& path, VARIANT& value)
    {
        if (value.vt != (VT_ARRAY | VT_UI1) || value.parray == nullptr ||
            SafeArrayGetDim(value.parray) != 1)
        {
            return false;
        }
        LONG lower{};
        LONG upper{};
        if (FAILED(SafeArrayGetLBound(value.parray, 1, &lower)) ||
            FAILED(SafeArrayGetUBound(value.parray, 1, &upper)) || upper < lower)
        {
            return false;
        }
        void* data{};
        if (FAILED(SafeArrayAccessData(value.parray, &data)))
        {
            return false;
        }
        const auto byte_count = static_cast<std::size_t>(upper - lower + 1);
        const auto temporary = path.wstring() + L".tmp";
        HANDLE file = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        bool written{};
        if (file != INVALID_HANDLE_VALUE)
        {
            written = write_exact(file, data, byte_count);
            CloseHandle(file);
        }
        SafeArrayUnaccessData(value.parray);
        if (!written || !MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }

    class WordPreviewSession
    {
    public:
        WordPreviewSession(
            std::wstring input,
            std::filesystem::path cache,
            HANDLE request,
            HANDLE response)
            : input_(std::move(input)),
              cache_(std::move(cache)),
              request_(request),
              response_(response)
        {
        }

        ~WordPreviewSession()
        {
            if (document_)
            {
                static_cast<void>(invoke_method(document_.get(), L"Close", { boolean_argument(false) }));
            }
            pages_ = nullptr;
            pane_ = nullptr;
            panes_ = nullptr;
            active_window_ = nullptr;
            document_ = nullptr;
            if (application_)
            {
                static_cast<void>(invoke_method(application_.get(), L"Quit", { boolean_argument(false) }));
            }
            application_ = nullptr;
        }

        int run()
        {
            for (;;)
            {
                RequestHeader request{};
                if (!read_exact(request_, &request, sizeof(request)))
                {
                    return 0;
                }
                if (request.magic != protocol_magic || request.version != protocol_version ||
                    request.payload_size > maximum_payload_size)
                {
                    return 6;
                }
                std::vector<std::byte> payload(request.payload_size);
                if (!payload.empty() && !read_exact(request_, payload.data(), payload.size()))
                {
                    return 7;
                }
                switch (request.command)
                {
                case Command::open_session:
                    send_response(
                        payload.empty() && initialize() ? Status::success : Status::open_failed,
                        {});
                    break;
                case Command::render_page:
                    handle_render_page(payload);
                    break;
                case Command::get_page_count:
                    handle_page_count(payload);
                    break;
                case Command::shutdown:
                    send_response(Status::success, {});
                    return 0;
                default:
                    send_response(Status::invalid_request, {});
                    break;
                }
            }
        }

    private:
        struct CachedPage
        {
            std::wstring path;
            float width_points{};
            float height_points{};
        };

        bool initialize()
        {
            if (pages_)
            {
                return true;
            }
            application_ = create_application(L"Word.Application");
            if (!application_)
            {
                return false;
            }
            static_cast<void>(set_property(application_.get(), L"Visible", boolean_argument(false)));
            static_cast<void>(set_property(application_.get(), L"DisplayAlerts", integer_argument(0)));
            static_cast<void>(set_property(application_.get(), L"ScreenUpdating", boolean_argument(false)));
            static_cast<void>(set_property(application_.get(), L"AutomationSecurity", integer_argument(3)));
            auto documents = get_dispatch_property(application_.get(), L"Documents");
            document_ = documents
                ? invoke_dispatch_method(
                      documents.get(),
                      L"Open",
                      { string_argument(input_), boolean_argument(false), boolean_argument(true) })
                : winrt::com_ptr<IDispatch>{};
            if (!document_)
            {
                return false;
            }
            active_window_ = get_dispatch_property(document_.get(), L"ActiveWindow");
            panes_ = active_window_
                ? get_dispatch_property(active_window_.get(), L"Panes")
                : winrt::com_ptr<IDispatch>{};
            pane_ = panes_
                ? invoke_dispatch_member(panes_.get(), L"Item", { integer_argument(1) })
                : winrt::com_ptr<IDispatch>{};
            pages_ = pane_
                ? get_dispatch_property(pane_.get(), L"Pages")
                : winrt::com_ptr<IDispatch>{};
            return pages_ != nullptr;
        }

        bool send_response(Status status, const std::vector<std::byte>& payload)
        {
            const ResponseHeader header{
                .status = status,
                .payload_size = static_cast<std::uint32_t>(payload.size()),
            };
            return write_exact(response_, &header, sizeof(header)) &&
                (payload.empty() || write_exact(response_, payload.data(), payload.size()));
        }

        void handle_page_count(const std::vector<std::byte>& payload)
        {
            double count{};
            if (!payload.empty() || !pages_ || !numeric_property(pages_.get(), L"Count", count) ||
                count <= 0.0 || count > static_cast<double>(UINT32_MAX))
            {
                send_response(Status::render_failed, {});
                return;
            }
            std::vector<std::byte> response;
            append_value(response, PageCountResponse{
                .page_count = static_cast<std::uint32_t>(count),
            });
            send_response(Status::success, response);
        }

        void handle_render_page(const std::vector<std::byte>& payload)
        {
            if (!pages_ || payload.size() != sizeof(PageRequest))
            {
                send_response(Status::invalid_request, {});
                return;
            }
            PageRequest request{};
            std::memcpy(&request, payload.data(), sizeof(request));
            const auto cached = cached_pages_.find(request.page_index);
            if (cached != cached_pages_.end())
            {
                send_page_response(request.page_index, cached->second);
                return;
            }
            auto page = invoke_dispatch_member(
                pages_.get(),
                L"Item",
                { integer_argument(static_cast<LONG>(request.page_index + 1U)) });
            if (!page)
            {
                send_response(Status::invalid_page, {});
                return;
            }
            double width{};
            double height{};
            if (!numeric_property(page.get(), L"Width", width) ||
                !numeric_property(page.get(), L"Height", height) ||
                width <= 0.0 || height <= 0.0)
            {
                send_response(Status::render_failed, {});
                return;
            }
            VARIANT bits;
            VariantInit(&bits);
            const HRESULT status = invoke(
                page.get(),
                L"EnhMetaFileBits",
                DISPATCH_PROPERTYGET,
                &bits);
            const auto path = cache_ / (L"page-" + std::to_wstring(request.page_index + 1U) + L".emf");
            const bool saved = SUCCEEDED(status) && write_byte_array(path, bits);
            VariantClear(&bits);
            if (!saved)
            {
                send_response(Status::render_failed, {});
                return;
            }
            CachedPage metadata{
                .path = path.wstring(),
                .width_points = static_cast<float>(width),
                .height_points = static_cast<float>(height),
            };
            cached_pages_.emplace(request.page_index, metadata);
            send_page_response(request.page_index, metadata);
        }

        void send_page_response(std::uint32_t page_index, const CachedPage& page)
        {
            const PageResponse metadata{
                .page_index = page_index,
                .path_characters = static_cast<std::uint32_t>(page.path.size()),
                .page_width_points = page.width_points,
                .page_height_points = page.height_points,
            };
            std::vector<std::byte> response;
            append_value(response, metadata);
            append_utf16(response, page.path);
            if (response.size() > maximum_payload_size)
            {
                send_response(Status::render_failed, {});
                return;
            }
            send_response(Status::success, response);
        }

        std::wstring input_;
        std::filesystem::path cache_;
        HANDLE request_{};
        HANDLE response_{};
        winrt::com_ptr<IDispatch> application_;
        winrt::com_ptr<IDispatch> document_;
        winrt::com_ptr<IDispatch> active_window_;
        winrt::com_ptr<IDispatch> panes_;
        winrt::com_ptr<IDispatch> pane_;
        winrt::com_ptr<IDispatch> pages_;
        std::unordered_map<std::uint32_t, CachedPage> cached_pages_;
    };

    int export_word(const std::wstring& input, const std::wstring& output)
    {
        auto application = create_application(L"Word.Application");
        if (!application)
        {
            return 10;
        }
        static_cast<void>(set_property(application.get(), L"Visible", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", integer_argument(0)));
        static_cast<void>(set_property(application.get(), L"ScreenUpdating", boolean_argument(false)));
        auto documents = get_dispatch_property(application.get(), L"Documents");
        auto document = documents
            ? invoke_dispatch_method(
                  documents.get(),
                  L"Open",
                  { string_argument(input), boolean_argument(false), boolean_argument(true) })
            : winrt::com_ptr<IDispatch>{};
        const HRESULT export_status = document
            ? invoke_method(document.get(), L"ExportAsFixedFormat", { string_argument(output), integer_argument(17) })
            : E_FAIL;
        if (document)
        {
            static_cast<void>(invoke_method(document.get(), L"Close", { boolean_argument(false) }));
        }
        static_cast<void>(invoke_method(application.get(), L"Quit", { boolean_argument(false) }));
        return SUCCEEDED(export_status) ? 0 : 11;
    }

    int export_excel(const std::wstring& input, const std::wstring& output)
    {
        auto application = create_application(L"Excel.Application");
        if (!application)
        {
            return 20;
        }
        static_cast<void>(set_property(application.get(), L"Visible", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"ScreenUpdating", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"EnableEvents", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"Interactive", boolean_argument(false)));
        auto workbooks = get_dispatch_property(application.get(), L"Workbooks");
        auto workbook = workbooks
            ? invoke_dispatch_method(
                  workbooks.get(),
                  L"Open",
                  { string_argument(input), integer_argument(0), boolean_argument(true) })
            : winrt::com_ptr<IDispatch>{};
        const HRESULT export_status = workbook
            ? invoke_method(workbook.get(), L"ExportAsFixedFormat", { integer_argument(0), string_argument(output) })
            : E_FAIL;
        if (workbook)
        {
            static_cast<void>(invoke_method(workbook.get(), L"Close", { boolean_argument(false) }));
        }
        static_cast<void>(invoke_method(application.get(), L"Quit"));
        return SUCCEEDED(export_status) ? 0 : 21;
    }

    int export_powerpoint(const std::wstring& input, const std::wstring& output)
    {
        auto application = create_application(L"PowerPoint.Application");
        if (!application)
        {
            return 30;
        }
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", integer_argument(1)));
        auto presentations = get_dispatch_property(application.get(), L"Presentations");
        auto presentation = presentations
            ? invoke_dispatch_method(
                  presentations.get(),
                  L"Open",
                  { string_argument(input), integer_argument(-1), integer_argument(0), integer_argument(0) })
            : winrt::com_ptr<IDispatch>{};
        const HRESULT export_status = presentation
            ? invoke_method(
                  presentation.get(),
                  L"SaveAs",
                  { string_argument(output), integer_argument(32), integer_argument(-1) })
            : E_FAIL;
        if (presentation)
        {
            static_cast<void>(invoke_method(presentation.get(), L"Close"));
        }
        static_cast<void>(invoke_method(application.get(), L"Quit"));
        return SUCCEEDED(export_status) ? 0 : 31;
    }
}

namespace glance::office
{
    int run_word_preview_session(
        const std::wstring& input_path,
        const std::wstring& cache_directory,
        HANDLE request_pipe,
        HANDLE response_pipe)
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment))
        {
            return 4;
        }
        const int result = WordPreviewSession(
            input_path,
            std::filesystem::path(cache_directory),
            request_pipe,
            response_pipe).run();
        CoUninitialize();
        return result;
    }

    int export_to_pdf(const std::wstring& input_path, const std::wstring& output_path)
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment))
        {
            return 4;
        }

        const auto extension = std::filesystem::path(input_path).extension().wstring();
        int result{};
        if (_wcsicmp(extension.c_str(), L".doc") == 0 || _wcsicmp(extension.c_str(), L".docx") == 0)
        {
            result = export_word(input_path, output_path);
        }
        else if (_wcsicmp(extension.c_str(), L".xls") == 0 || _wcsicmp(extension.c_str(), L".xlsx") == 0)
        {
            result = export_excel(input_path, output_path);
        }
        else if (_wcsicmp(extension.c_str(), L".ppt") == 0 || _wcsicmp(extension.c_str(), L".pptx") == 0)
        {
            result = export_powerpoint(input_path, output_path);
        }
        else
        {
            result = 5;
        }
        CoUninitialize();
        return result;
    }
}
