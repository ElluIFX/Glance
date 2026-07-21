#include "../include/office_automation.h"

#include <windows.h>
#include <oaidl.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include <winrt/base.h>

namespace
{
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

    int export_word(const std::wstring& input, const std::wstring& output)
    {
        auto application = create_application(L"Word.Application");
        if (!application)
        {
            return 10;
        }
        static_cast<void>(set_property(application.get(), L"Visible", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", integer_argument(0)));
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
