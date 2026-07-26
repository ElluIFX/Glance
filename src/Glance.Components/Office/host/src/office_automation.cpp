#include "../include/office_automation.h"

#include <windows.h>
#include <oaidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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
        HRESULT status = object->GetIDsOfNames(
            IID_NULL,
            &name,
            1,
            LOCALE_USER_DEFAULT,
            &member);
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
        const HRESULT status = invoke(
            object,
            name,
            DISPATCH_METHOD,
            &result,
            std::move(arguments));
        VariantClear(&result);
        return status;
    }

    HRESULT set_property(IDispatch* object, const wchar_t* name, VARIANTARG value)
    {
        VARIANT result;
        VariantInit(&result);
        const HRESULT status = invoke(
            object,
            name,
            DISPATCH_PROPERTYPUT,
            &result,
            { value });
        VariantClear(&result);
        return status;
    }

    std::unordered_set<DWORD> process_snapshot()
    {
        std::unordered_set<DWORD> result;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return result;
        }
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                result.insert(entry.th32ProcessID);
            }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    DWORD application_process_id(IDispatch* application)
    {
        VARIANT result;
        VariantInit(&result);
        if (FAILED(invoke(
                application,
                L"Hwnd",
                DISPATCH_PROPERTYGET,
                &result)))
        {
            VariantClear(&result);
            return 0;
        }
        HWND window{};
        if (result.vt == VT_I4 || result.vt == VT_INT)
        {
            window = reinterpret_cast<HWND>(
                static_cast<std::intptr_t>(result.lVal));
        }
        else if (result.vt == VT_UI4 || result.vt == VT_UINT)
        {
            window = reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(result.ulVal));
        }
        VariantClear(&result);
        DWORD process_id{};
        if (window != nullptr)
        {
            GetWindowThreadProcessId(window, &process_id);
        }
        return process_id;
    }

    DWORD new_process_id(
        const std::unordered_set<DWORD>& existing_processes,
        const wchar_t* executable_name)
    {
        const auto process_name_is = [](DWORD process_id, const wchar_t* expected_name) {
            bool matches{};
            HANDLE process_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (process_snapshot == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            PROCESSENTRY32W process_entry{ sizeof(process_entry) };
            if (Process32FirstW(process_snapshot, &process_entry))
            {
                do
                {
                    if (process_entry.th32ProcessID == process_id)
                    {
                        matches = _wcsicmp(
                            process_entry.szExeFile,
                            expected_name) == 0;
                        break;
                    }
                }
                while (Process32NextW(process_snapshot, &process_entry));
            }
            CloseHandle(process_snapshot);
            return matches;
        };

        DWORD result{};
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (!existing_processes.contains(entry.th32ProcessID) &&
                    _wcsicmp(entry.szExeFile, executable_name) == 0 &&
                    process_name_is(entry.th32ParentProcessID, L"svchost.exe"))
                {
                    if (result != 0)
                    {
                        result = 0;
                        break;
                    }
                    result = entry.th32ProcessID;
                }
            }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    winrt::com_ptr<IDispatch> create_application(
        const wchar_t* programmatic_id,
        const wchar_t* executable_name,
        std::atomic<DWORD>& owned_process_id)
    {
        const auto existing_processes = process_snapshot();
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
        DWORD process_id = application_process_id(application.get());
        if (process_id == 0 || existing_processes.contains(process_id))
        {
            process_id = new_process_id(existing_processes, executable_name);
        }
        if (process_id != 0 && !existing_processes.contains(process_id))
        {
            owned_process_id.store(process_id, std::memory_order_release);
        }
        return application;
    }

    int export_word(
        const std::wstring& input,
        const std::wstring& output,
        std::atomic<DWORD>& owned_process_id)
    {
        auto application = create_application(
            L"Word.Application",
            L"WINWORD.EXE",
            owned_process_id);
        if (!application)
        {
            return 10;
        }
        static_cast<void>(set_property(application.get(), L"Visible", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", integer_argument(0)));
        static_cast<void>(set_property(application.get(), L"ScreenUpdating", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"AutomationSecurity", integer_argument(3)));

        auto documents = get_dispatch_property(application.get(), L"Documents");
        auto document = documents
            ? invoke_dispatch_method(
                  documents.get(),
                  L"Open",
                  { string_argument(input), boolean_argument(false), boolean_argument(true) })
            : winrt::com_ptr<IDispatch>{};
        const HRESULT export_status = document
            ? invoke_method(
                  document.get(),
                  L"ExportAsFixedFormat",
                  { string_argument(output), integer_argument(17) })
            : E_FAIL;
        if (document)
        {
            static_cast<void>(invoke_method(
                document.get(),
                L"Close",
                { boolean_argument(false) }));
        }
        static_cast<void>(invoke_method(
            application.get(),
            L"Quit",
            { boolean_argument(false) }));
        return SUCCEEDED(export_status) ? 0 : 11;
    }

    int export_excel(
        const std::wstring& input,
        const std::wstring& output,
        std::atomic<DWORD>& owned_process_id)
    {
        auto application = create_application(
            L"Excel.Application",
            L"EXCEL.EXE",
            owned_process_id);
        if (!application)
        {
            return 20;
        }
        static_cast<void>(set_property(application.get(), L"Visible", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"ScreenUpdating", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"EnableEvents", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"Interactive", boolean_argument(false)));
        static_cast<void>(set_property(application.get(), L"AutomationSecurity", integer_argument(3)));

        auto workbooks = get_dispatch_property(application.get(), L"Workbooks");
        auto workbook = workbooks
            ? invoke_dispatch_method(
                  workbooks.get(),
                  L"Open",
                  { string_argument(input), integer_argument(0), boolean_argument(true) })
            : winrt::com_ptr<IDispatch>{};
        const HRESULT export_status = workbook
            ? invoke_method(
                  workbook.get(),
                  L"ExportAsFixedFormat",
                  { integer_argument(0), string_argument(output) })
            : E_FAIL;
        if (workbook)
        {
            static_cast<void>(invoke_method(
                workbook.get(),
                L"Close",
                { boolean_argument(false) }));
        }
        static_cast<void>(invoke_method(application.get(), L"Quit"));
        return SUCCEEDED(export_status) ? 0 : 21;
    }

    int export_powerpoint(
        const std::wstring& input,
        const std::wstring& output,
        std::atomic<DWORD>& owned_process_id)
    {
        auto application = create_application(
            L"PowerPoint.Application",
            L"POWERPNT.EXE",
            owned_process_id);
        if (!application)
        {
            return 30;
        }
        static_cast<void>(set_property(application.get(), L"DisplayAlerts", integer_argument(1)));
        static_cast<void>(set_property(application.get(), L"AutomationSecurity", integer_argument(3)));

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
    int export_to_pdf(
        const std::wstring& input_path,
        const std::wstring& output_path,
        HANDLE cancellation_event)
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment))
        {
            return 4;
        }

        const HRESULT cancellation_enabled = CoEnableCallCancellation(nullptr);
        HANDLE watcher_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const DWORD apartment_thread = GetCurrentThreadId();
        std::atomic<DWORD> owned_process_id{};
        const auto extension = std::filesystem::path(input_path).extension().wstring();
        const wchar_t* expected_executable =
            _wcsicmp(extension.c_str(), L".doc") == 0 ||
                _wcsicmp(extension.c_str(), L".docx") == 0
            ? L"WINWORD.EXE"
            : _wcsicmp(extension.c_str(), L".xls") == 0 ||
                    _wcsicmp(extension.c_str(), L".xlsx") == 0
                ? L"EXCEL.EXE"
                : L"POWERPNT.EXE";
        const auto existing_office_processes = process_snapshot();
        std::thread cancellation_watcher;
        if (SUCCEEDED(cancellation_enabled) && watcher_stop != nullptr)
        {
            cancellation_watcher = std::thread([
                cancellation_event,
                watcher_stop,
                apartment_thread,
                &owned_process_id,
                expected_executable,
                existing_office_processes] {
                HANDLE handles[]{ cancellation_event, watcher_stop };
                if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
                {
                    static_cast<void>(CoCancelCall(apartment_thread, 0));
                    static_cast<void>(WaitForSingleObject(watcher_stop, 3000));
                    DWORD process_id =
                        owned_process_id.load(std::memory_order_acquire);
                    if (process_id == 0)
                    {
                        process_id = new_process_id(
                            existing_office_processes,
                            expected_executable);
                    }
                    HANDLE process = process_id == 0
                        ? nullptr
                        : OpenProcess(
                            SYNCHRONIZE | PROCESS_TERMINATE,
                            FALSE,
                            process_id);
                    if (process != nullptr)
                    {
                        if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT)
                        {
                            TerminateProcess(process, ERROR_CANCELLED);
                        }
                        CloseHandle(process);
                    }
                }
            });
        }

        int result{};
        if (_wcsicmp(extension.c_str(), L".doc") == 0 ||
            _wcsicmp(extension.c_str(), L".docx") == 0)
        {
            result = export_word(input_path, output_path, owned_process_id);
        }
        else if (_wcsicmp(extension.c_str(), L".xls") == 0 ||
                 _wcsicmp(extension.c_str(), L".xlsx") == 0)
        {
            result = export_excel(input_path, output_path, owned_process_id);
        }
        else if (_wcsicmp(extension.c_str(), L".ppt") == 0 ||
                 _wcsicmp(extension.c_str(), L".pptx") == 0)
        {
            result = export_powerpoint(input_path, output_path, owned_process_id);
        }
        else
        {
            result = 5;
        }

        if (watcher_stop != nullptr)
        {
            SetEvent(watcher_stop);
        }
        if (cancellation_watcher.joinable())
        {
            cancellation_watcher.join();
        }
        if (SUCCEEDED(cancellation_enabled))
        {
            CoDisableCallCancellation(nullptr);
        }
        if (watcher_stop != nullptr)
        {
            CloseHandle(watcher_stop);
        }
        CoUninitialize();
        return result;
    }
}
