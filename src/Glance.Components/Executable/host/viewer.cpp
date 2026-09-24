#include "viewer.h"
#include "signature.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <objbase.h>
#include <shlwapi.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>

namespace glance::executable
{
    namespace
    {
        constexpr UINT ready_message = WM_APP + 18;
        constexpr unsigned summary_task = 100, hash_task = 101, resource_task = 102, save_resource_task = 103,
                           report_task = 104;
        std::wstring text_of(HWND window)
        {
            std::wstring result(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            result.resize(GetWindowTextW(window, result.data(), static_cast<int>(result.size())));
            return result;
        }
        std::wstring json_string(std::wstring_view value)
        {
            std::wstring result = L"\"";
            for (wchar_t c : value)
            {
                if (c == L'"' || c == L'\\')
                {
                    result += L'\\';
                    result += c;
                }
                else if (c == L'\n')
                    result += L"\\n";
                else if (c == L'\r')
                    result += L"\\r";
                else if (c == L'\t')
                    result += L"\\t";
                else if (c < 32)
                {
                    wchar_t buffer[8]{};
                    swprintf_s(buffer, L"\\u%04x", unsigned(c));
                    result += buffer;
                }
                else
                    result += c;
            }
            return result + L'"';
        }
        std::wstring save_path(HWND owner, const wchar_t* extension, const wchar_t* filter)
        {
            std::array<wchar_t, 32768> buffer{};
            OPENFILENAMEW dialog{sizeof(dialog)};
            dialog.hwndOwner = owner;
            dialog.lpstrFile = buffer.data();
            dialog.nMaxFile = static_cast<DWORD>(buffer.size());
            dialog.lpstrFilter = filter;
            dialog.lpstrDefExt = extension;
            dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
            return GetSaveFileNameW(&dialog) ? std::wstring(buffer.data()) : std::wstring{};
        }
        void write_utf8(const std::wstring& path, const std::wstring& text)
        {
            const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                                    nullptr, 0, nullptr, nullptr);
            std::string encoded(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), encoded.data(),
                                length, nullptr, nullptr);
            std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            output.write(encoded.data(), encoded.size());
            if (!output)
                throw std::runtime_error("SaveFailed");
        }
        void clipboard(HWND window, const std::wstring& text)
        {
            const auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            if (!memory)
                return;
            auto* pointer = GlobalLock(memory);
            if (!pointer)
            {
                GlobalFree(memory);
                return;
            }
            memcpy(pointer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (OpenClipboard(window))
            {
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, memory))
                    GlobalFree(memory);
                CloseClipboard();
            }
            else
                GlobalFree(memory);
        }
        void atomic_output(const std::wstring& destination, const std::wstring& source,
                           const std::function<void(const std::wstring&)>& write)
        {
            std::error_code error;
            if (std::filesystem::equivalent(destination, source, error))
                throw std::runtime_error("SaveFailed");
            GUID id{};
            if (FAILED(CoCreateGuid(&id)))
                throw std::runtime_error("SaveFailed");
            wchar_t token[40]{};
            StringFromGUID2(id, token, 40);
            const auto temporary = destination + L"." + token + L".tmp";
            try
            {
                write(temporary);
                if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("SaveFailed");
            }
            catch (...)
            {
                DeleteFileW(temporary.c_str());
                throw;
            }
        }
        std::wstring format_report(const std::wstring& path, const std::array<Table, 7>& tables,
                                   const Resources& strings, bool json)
        {
            std::wstring output =
                json ? L"{\"file\":" + json_string(path) + L",\"sections\":[" : path + L"\r\n\r\n";
            for (std::size_t i = 0; i < tables.size(); ++i)
            {
                const auto& table = tables[i];
                if (json)
                {
                    if (i)
                        output += L",";
                    output += L"{\"name\":" + json_string(section_keys[i]) + L",\"state\":" +
                              json_string(table.state) + L",\"columns\":[";
                    for (std::size_t j = 0; j < table.columns.size(); ++j)
                    {
                        if (j)
                            output += L",";
                        output += json_string(table.columns[j]);
                    }
                    output += L"],\"rows\":[";
                }
                else
                    output += strings.text(section_keys[i]) + L" — " + strings.text(table.state) + L"\r\n";
                for (std::size_t row = 0; row < table.rows.size(); ++row)
                {
                    if (json)
                    {
                        if (row)
                            output += L",";
                        output += L"[";
                    }
                    for (std::size_t column = 0; column < table.rows[row].cells.size(); ++column)
                    {
                        if (column)
                            output += json ? L"," : L"\t";
                        const auto& value = table.rows[row].cells[column];
                        output += json ? json_string(value)
                                       : ((column == 0 && table.field_keys) || i == 1 ? strings.text(value)
                                                                                      : value);
                    }
                    output += json ? L"]" : L"\r\n";
                }
                output += json ? L"]}" : L"\r\n";
            }
            return output + (json ? L"]}" : L"");
        }
    } // namespace
    Viewer::Viewer()
    {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = procedure;
        klass.lpszClassName = L"Glance.Executable.Viewer";
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&klass);
    }
    Viewer::~Viewer()
    {
        close();
    }
    int Viewer::px(int value) const
    {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }
    bool Viewer::open(HWND parent, const std::wstring& path, const RECT& bounds, unsigned dpi,
                      const contracts::native_preview::PreviewVisuals& value)
    {
        close();
        path_ = path;
        dpi_ = dpi ? dpi : 96;
        stopping_ = false;
        searches_.fill({});
        top_rows_.fill(0);
        sort_columns_.fill(-1);
        sort_directions_.fill(false);
        for (auto& selection : selections_)
            selection.clear();
        for (auto& table : tables_)
        {
            table = {};
            table.state = L"NotLoaded";
        }
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"Glance.Executable.Viewer", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, bounds.left, bounds.top,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top, parent, nullptr,
                                  GetModuleHandleW(nullptr), this);
        if (!window_)
            return false;
        auto control = [&](const wchar_t* klass, DWORD style, int id) {
            return CreateWindowExW(0, klass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0,
                                   window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };
        nav_ =
            control(L"LISTBOX", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 10);
        compact_ = control(WC_COMBOBOXW, CBS_DROPDOWNLIST | WS_VSCROLL, 11);
        search_ = control(L"EDIT", ES_AUTOHSCROLL, 12);
        list_ = control(WC_LISTVIEWW, LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 13);
        ListView_SetExtendedListViewStyle(list_,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        details_ = control(L"EDIT", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 14);
        copy_ = control(L"BUTTON", BS_PUSHBUTTON, 15);
        report_ = control(L"BUTTON", BS_PUSHBUTTON, 16);
        action_ = control(L"BUTTON", BS_PUSHBUTTON, 17);
        status_ = control(L"STATIC", SS_LEFT | SS_ENDELLIPSIS, 18);
        master_ = control(L"LISTBOX", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 19);
        overview_ = control(L"STATIC", WS_VSCROLL | WS_CLIPCHILDREN, 20);
        picture_ = control(L"STATIC", SS_OWNERDRAW, 21);
        SetWindowSubclass(
            overview_,
            [](HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
               DWORD_PTR reference) -> LRESULT {
                auto* self = reinterpret_cast<Viewer*>(reference);
                if (message == WM_VSCROLL || message == WM_MOUSEWHEEL)
                {
                    SCROLLINFO info{sizeof(info), SIF_ALL};
                    GetScrollInfo(window, SB_VERT, &info);
                    int position = info.nPos;
                    if (message == WM_MOUSEWHEEL)
                        position -= GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * self->px(60);
                    else
                        switch (LOWORD(wparam))
                        {
                        case SB_LINEUP:
                            position -= self->px(30);
                            break;
                        case SB_LINEDOWN:
                            position += self->px(30);
                            break;
                        case SB_PAGEUP:
                            position -= info.nPage;
                            break;
                        case SB_PAGEDOWN:
                            position += info.nPage;
                            break;
                        case SB_THUMBTRACK:
                            position = info.nTrackPos;
                            break;
                        }
                    self->overview_scroll_ =
                        std::clamp(position, 0, std::max(0, info.nMax - static_cast<int>(info.nPage) + 1));
                    self->overview_layout();
                    return 0;
                }
                if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
                {
                    SetTextColor(reinterpret_cast<HDC>(wparam), self->foreground_);
                    SetBkColor(reinterpret_cast<HDC>(wparam), self->background_);
                    return reinterpret_cast<LRESULT>(self->brush_);
                }
                return DefSubclassProc(window, message, wparam, lparam);
            },
            1, reinterpret_cast<DWORD_PTR>(this));
        visuals(value);
        language(language_);
        initialized_ = true;
        resize(bounds, dpi_);
        worker_ = std::thread([this] { work(); });
        request(summary_task);
        return true;
    }
    void Viewer::close()
    {
        stopping_ = true;
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (window_)
            DestroyWindow(window_);
        window_ = nullptr;
        initialized_ = false;
        fields_.clear();
        groups_.clear();
        masters_.clear();
        master_filter_.clear();
        overview_scroll_ = 0;
        resource_image_ = {};
        if (font_)
            DeleteObject(font_);
        font_ = nullptr;
        if (heading_)
            DeleteObject(heading_);
        heading_ = nullptr;
        if (section_font_)
            DeleteObject(section_font_);
        section_font_ = nullptr;
        if (code_font_)
            DeleteObject(code_font_);
        code_font_ = nullptr;
        if (brush_)
            DeleteObject(brush_);
        brush_ = nullptr;
        if (icon_)
            DestroyIcon(icon_);
        icon_ = nullptr;
        tasks_.clear();
        completed_.clear();
        summary_.reset();
        filtered_.clear();
        hash_.clear();
        section_ = 0;
    }
    void Viewer::resize(const RECT& bounds, unsigned dpi)
    {
        if (!window_)
            return;
        const auto next = dpi ? dpi : 96;
        if (!font_ || dpi_ != next)
        {
            dpi_ = next;
            auto old = font_;
            auto old_heading = heading_;
            auto old_section = section_font_, old_code = code_font_;
            font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                L"Segoe UI");
            heading_ = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
            section_font_ = CreateFontW(-px(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Segoe UI");
            code_font_ = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                                     L"Consolas");
            for (auto child : {nav_, compact_, search_, list_, details_, copy_, report_, action_, status_,
                               master_, overview_})
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            for (const auto& field : fields_)
            {
                SendMessageW(field.label, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                SendMessageW(field.value, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            }
            for (auto group : groups_)
                SendMessageW(group, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
            if (old)
                DeleteObject(old);
            if (old_heading)
                DeleteObject(old_heading);
            if (old_section)
                DeleteObject(old_section);
            if (old_code)
                DeleteObject(old_code);
            SendMessageW(nav_, LB_SETITEMHEIGHT, 0, px(42));
        }
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                   TRUE);
        layout();
    }
    void Viewer::visuals(const contracts::native_preview::PreviewVisuals& value)
    {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        background_ = contrast.dwFlags & HCF_HIGHCONTRASTON ? GetSysColor(COLOR_WINDOW)
                                                            : value.background_color & 0xffffff;
        foreground_ = contrast.dwFlags & HCF_HIGHCONTRASTON ? GetSysColor(COLOR_WINDOWTEXT)
                                                            : value.text_color & 0xffffff;
        if (brush_)
            DeleteObject(brush_);
        brush_ = CreateSolidBrush(background_);
        if (!window_)
            return;
        ListView_SetBkColor(list_, background_);
        ListView_SetTextBkColor(list_, background_);
        ListView_SetTextColor(list_, foreground_);
        SetWindowTheme(list_, L"Explorer", nullptr);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }
    void Viewer::language(const std::wstring& value)
    {
        language_ = value;
        strings_ = Resources(value);
        if (!window_)
            return;
        navigation_.clear();
        SendMessageW(nav_, LB_RESETCONTENT, 0, 0);
        SendMessageW(compact_, CB_RESETCONTENT, 0, 0);
        for (unsigned i = 0; i < static_cast<unsigned>(Section::count); ++i)
        {
            if (i == static_cast<unsigned>(Section::managed) && summary_ && !summary_->managed)
                continue;
            navigation_.push_back(i);
            auto label = strings_.text(section_keys[i]);
            SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            SendMessageW(compact_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SetWindowTextW(copy_, strings_.text(L"Copy").c_str());
        SetWindowTextW(report_, strings_.text(L"Report").c_str());
        SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(strings_.text(L"Search").c_str()));
        select(section_);
    }
    void Viewer::layout()
    {
        if (!window_ || !nav_)
            return;
        RECT rect{};
        GetClientRect(window_, &rect);
        const int width = rect.right, height = rect.bottom, pad = px(20);
        const bool narrow = width < px(720);
        ShowWindow(nav_, narrow ? SW_HIDE : SW_SHOW);
        ShowWindow(compact_, narrow ? SW_SHOW : SW_HIDE);
        const int left = narrow ? pad : px(186), right = std::max(left + 1, width - pad);
        const int top = narrow ? px(142) : px(104);
        MoveWindow(nav_, pad / 2, px(104), px(158), std::max(1, height - px(128)), TRUE);
        MoveWindow(compact_, pad, px(98), std::max(1, width - pad * 2), px(280), TRUE);
        const int toolbar_height = narrow ? px(72) : px(34);
        const int available = right - left;
        const int button_width = px(100), action_width = px(150), gap = px(8);
        const int buttons_y = narrow ? top + px(38) : top;
        MoveWindow(search_, left, top + px(5),
                   narrow ? available
                          : std::max(px(60), available - button_width * 2 - action_width - gap * 3),
                   px(26), TRUE);
        const int buttons_x = narrow ? left : right - button_width * 2 - action_width - gap * 2;
        MoveWindow(copy_, buttons_x, buttons_y, button_width, px(32), TRUE);
        MoveWindow(report_, buttons_x + button_width + gap, buttons_y, button_width, px(32), TRUE);
        MoveWindow(action_, buttons_x + (button_width + gap) * 2, buttons_y, action_width, px(32), TRUE);
        const int content_top = top + toolbar_height + px(12),
                  bottom = std::max(content_top + 1, height - px(42));
        const bool resource = section_ == static_cast<unsigned>(Section::resources);
        const bool has_master = !narrow && (section_ == 2 || section_ == 4);
        const int master_width = has_master ? std::min(px(190), available / 3) : 0;
        ShowWindow(master_, has_master ? SW_SHOW : SW_HIDE);
        if (has_master)
            MoveWindow(master_, left, content_top, master_width - px(10), std::max(1, bottom - content_top),
                       TRUE);
        const int preview_height = resource ? std::max(px(80), (bottom - content_top) / 3) : 0;
        MoveWindow(list_, left + master_width, content_top, available - master_width,
                   std::max(1, bottom - content_top - preview_height), TRUE);
        ShowWindow(list_, section_ == 0 ? SW_HIDE : SW_SHOW);
        ShowWindow(overview_, section_ == 0 ? SW_SHOW : SW_HIDE);
        MoveWindow(overview_, left, content_top, available, std::max(1, bottom - content_top), TRUE);
        if (section_ == 0)
            overview_layout();
        ShowWindow(details_, resource && !resource_image_.bitmap ? SW_SHOW : SW_HIDE);
        ShowWindow(picture_, resource && resource_image_.bitmap ? SW_SHOW : SW_HIDE);
        if (resource)
            MoveWindow(details_, left + master_width, bottom - preview_height + px(8),
                       available - master_width, std::max(1, preview_height - px(8)), TRUE);
        if (resource)
            MoveWindow(picture_, left + master_width, bottom - preview_height + px(8),
                       available - master_width, std::max(1, preview_height - px(8)), TRUE);
        MoveWindow(status_, left, height - px(30), available, px(24), TRUE);
        const auto columns = tables_[section_].columns.size();
        if (columns == 2)
        {
            ListView_SetColumnWidth(list_, 0, std::min(px(180), available / 3));
            ListView_SetColumnWidth(list_, 1,
                                    std::max(px(100), available - std::min(px(180), available / 3) - px(24)));
        }
        InvalidateRect(window_, nullptr, TRUE);
    }
    void Viewer::select(unsigned section)
    {
        if (section >= tables_.size())
            return;
        top_rows_[section_] = ListView_GetTopIndex(list_);
        selections_[section_].clear();
        for (int item = ListView_GetNextItem(list_, -1, LVNI_SELECTED); item >= 0;
             item = ListView_GetNextItem(list_, item, LVNI_SELECTED))
            if (static_cast<std::size_t>(item) < filtered_.size())
                selections_[section_].push_back(filtered_[item]);
        searches_[section_] = text_of(search_);
        sort_columns_[section_] = sort_column_;
        sort_directions_[section_] = descending_;
        if (section_ != section)
            master_filter_.clear();
        section_ = section;
        const auto position = std::find(navigation_.begin(), navigation_.end(), section);
        if (position == navigation_.end())
        {
            section_ = 0;
            return;
        }
        const auto index = static_cast<WPARAM>(position - navigation_.begin());
        SendMessageW(nav_, LB_SETCURSEL, index, 0);
        SendMessageW(compact_, CB_SETCURSEL, index, 0);
        SetWindowTextW(search_, searches_[section_].c_str());
        sort_column_ = sort_columns_[section_];
        descending_ = sort_directions_[section_];
        if (initialized_ && summary_ && tables_[section_].state == L"NotLoaded" &&
            !(section_ == 1 && summary_->identity.size > 64ULL * 1024 * 1024))
            request(section_);
        refresh();
    }
    void Viewer::refresh()
    {
        updating_ = true;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ListView_SetItemCountEx(list_, 0, LVSICF_NOSCROLL);
        while (ListView_DeleteColumn(list_, 0))
        {
        }
        const auto& table = tables_[section_];
        for (std::size_t i = 0; i < table.columns.size(); ++i)
        {
            auto text = strings_.text(table.columns[i]);
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = text.data();
            column.cx = px(i == 0 ? 220 : 160);
            ListView_InsertColumn(list_, static_cast<int>(i), &column);
        }
        filter();
        rebuild_master();
        if (section_ == 0)
        {
            for (const auto& field : fields_)
            {
                DestroyWindow(field.label);
                DestroyWindow(field.value);
            }
            for (auto group : groups_)
                DestroyWindow(group);
            fields_.clear();
            groups_.clear();
            auto create = [&](const wchar_t* klass, const std::wstring& text, DWORD style) {
                auto child = CreateWindowExW(0, klass, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0,
                                             0, overview_, nullptr, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                return child;
            };
            for (const auto key : {L"Basic", L"Runtime", L"FileSummary"})
            {
                auto group = create(L"STATIC", strings_.text(key), SS_LEFT);
                SendMessageW(group, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
                groups_.push_back(group);
            }
            for (auto row : filtered_)
                fields_.push_back(
                    {create(L"STATIC", cell(table.rows[row], 0), SS_LEFT),
                     create(L"EDIT", cell(table.rows[row], 1), ES_READONLY | ES_MULTILINE | WS_TABSTOP),
                     row});
        }
        for (std::size_t i = 0; i < filtered_.size(); ++i)
            if (std::find(selections_[section_].begin(), selections_[section_].end(), filtered_[i]) !=
                selections_[section_].end())
                ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        if (!filtered_.empty())
            ListView_EnsureVisible(
                list_, std::min(top_rows_[section_], static_cast<int>(filtered_.size() - 1)), FALSE);
        const auto state = strings_.text(table.state);
        const auto label = state + L"  ·  " + std::to_wstring(filtered_.size());
        SetWindowTextW(status_, label.c_str());
        SetWindowTextW(action_, strings_
                                    .text(section_ == 0   ? L"Hash"
                                          : section_ == 1 ? L"Verify"
                                          : section_ == 4 ? L"SaveResource"
                                                          : L"Reload")
                                    .c_str());
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        updating_ = false;
        InvalidateRect(list_, nullptr, TRUE);
        layout();
    }
    std::wstring Viewer::cell(const Row& row, std::size_t column) const
    {
        if (column >= row.cells.size())
            return {};
        if (section_ == 4 && column == 0)
        {
            switch (row.type)
            {
            case 1:
                return strings_.text(L"Cursor");
            case 2:
                return strings_.text(L"Bitmap");
            case 3:
                return strings_.text(L"Icon");
            case 6:
                return strings_.text(L"StringTable");
            case 12:
                return strings_.text(L"CursorGroup");
            case 14:
                return strings_.text(L"IconGroup");
            case 16:
                return strings_.text(L"VersionResource");
            case 24:
                return L"Manifest";
            default:
                break;
            }
        }
        if ((column == 0 && (tables_[section_].field_keys || section_ == 6)) ||
            (section_ == 2 && column == 1) || section_ == 1)
            return strings_.text(row.cells[column]);
        return row.cells[column];
    }
    void Viewer::filter()
    {
        filtered_.clear();
        const auto query = text_of(search_);
        const auto& rows = tables_[section_].rows;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            bool match = query.empty();
            if (!master_filter_.empty() && !rows[i].cells.empty() && rows[i].cells[0] != master_filter_)
                continue;
            for (std::size_t column = 0; !match && column < rows[i].cells.size(); ++column)
            {
                const auto value = cell(rows[i], column);
                match = FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE, value.data(),
                                        static_cast<int>(value.size()), query.data(),
                                        static_cast<int>(query.size()), nullptr, nullptr, nullptr, 0) >= 0;
            }
            if (match)
                filtered_.push_back(i);
        }
        if (sort_column_ >= 0)
            std::stable_sort(filtered_.begin(), filtered_.end(), [&](auto a, auto b) {
                const auto left = cell(rows[a], static_cast<std::size_t>(sort_column_));
                const auto right = cell(rows[b], static_cast<std::size_t>(sort_column_));
                const auto compare = StrCmpLogicalW(left.c_str(), right.c_str());
                return descending_ ? compare > 0 : compare < 0;
            });
        ListView_SetItemCountEx(list_, static_cast<int>(filtered_.size()), LVSICF_NOSCROLL);
        InvalidateRect(list_, nullptr, TRUE);
    }
    void Viewer::rebuild_master()
    {
        SendMessageW(master_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(master_, LB_RESETCONTENT, 0, 0);
        masters_.clear();
        masters_.push_back({});
        SendMessageW(master_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(strings_.text(L"AllEntries").c_str()));
        if (section_ == 2 || section_ == 4)
        {
            for (const auto& row : tables_[section_].rows)
                if (!row.cells.empty())
                    masters_.push_back(row.cells[0]);
            std::sort(masters_.begin() + 1, masters_.end());
            masters_.erase(std::unique(masters_.begin(), masters_.end()), masters_.end());
            for (std::size_t i = 1; i < masters_.size(); ++i)
            {
                auto label = masters_[i];
                if (section_ == 4)
                {
                    const auto row =
                        std::find_if(tables_[4].rows.begin(), tables_[4].rows.end(),
                                     [&](const Row& value) { return value.cells[0] == masters_[i]; });
                    if (row != tables_[4].rows.end())
                        label = cell(*row, 0);
                }
                SendMessageW(master_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        const auto selected = std::find(masters_.begin(), masters_.end(), master_filter_);
        SendMessageW(master_, LB_SETCURSEL,
                     selected == masters_.end() ? 0 : static_cast<WPARAM>(selected - masters_.begin()), 0);
        SendMessageW(master_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(master_, nullptr, TRUE);
    }
    void Viewer::overview_layout()
    {
        if (!overview_ || groups_.size() != 3)
            return;
        RECT rect{};
        GetClientRect(overview_, &rect);
        const int width = std::max(px(100), static_cast<int>(rect.right) - px(18));
        const bool columns = width >= px(760);
        int y = px(8);
        HDC dc = GetDC(overview_);
        SelectObject(dc, font_);
        for (unsigned group = 0; group < 3; ++group)
        {
            const auto category_of = [&](const FieldControl& field) {
                const auto& key = tables_[0].rows[field.row].cells[0];
                return (key == L"File" || key == L"Size" || key == L"SHA-256") ? 2U
                       : (key == L"Architecture" || key == L"Type" || key == L"Subsystem" ||
                          key == L"Security" || key == L"Permission" || key == L"UIAccess" || key == L"Dpi" ||
                          key == L"LongPath" || key == L"Compatibility")
                           ? 1U
                           : 0U;
            };
            const bool any = std::any_of(fields_.begin(), fields_.end(),
                                         [&](const auto& field) { return category_of(field) == group; });
            ShowWindow(groups_[group], any ? SW_SHOW : SW_HIDE);
            if (!any)
                continue;
            MoveWindow(groups_[group], 0, y - overview_scroll_, width, px(30), TRUE);
            y += px(38);
            int slot{}, row_height{};
            for (const auto& field : fields_)
            {
                const auto& row = tables_[0].rows[field.row];
                const auto& key = row.cells[0];
                const unsigned category =
                    (key == L"File" || key == L"Size" || key == L"SHA-256") ? 2
                    : (key == L"Architecture" || key == L"Type" || key == L"Subsystem" ||
                       key == L"Security" || key == L"Permission" || key == L"UIAccess" || key == L"Dpi" ||
                       key == L"LongPath" || key == L"Compatibility")
                        ? 1
                        : 0;
                if (category != group)
                    continue;
                const bool full = !columns || key == L"File" || key == L"Copyright" || key == L"SHA-256" ||
                                  key == L"Product";
                if (full && slot)
                {
                    y += row_height + px(8);
                    slot = 0;
                    row_height = 0;
                }
                const int slot_width = full ? width : (width - px(24)) / 2;
                const int label_width = std::min(px(112), slot_width / 3);
                const int x = slot * (slot_width + px(24));
                const int value_width = std::max(px(80), slot_width - label_width - px(16));
                RECT measured{0, 0, value_width - px(8), 0};
                DrawTextW(dc, row.cells[1].c_str(), -1, &measured,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                const int height = std::max(px(26), static_cast<int>(measured.bottom) + px(8));
                MoveWindow(field.label, x, y - overview_scroll_, label_width, height, TRUE);
                MoveWindow(field.value, x + label_width + px(12), y - overview_scroll_, value_width, height,
                           TRUE);
                SendMessageW(field.value, WM_SETFONT,
                             reinterpret_cast<WPARAM>(key == L"SHA-256" ? code_font_ : font_), TRUE);
                row_height = std::max(row_height, height);
                if (full || slot == 1)
                {
                    y += row_height + px(8);
                    slot = 0;
                    row_height = 0;
                }
                else
                    slot = 1;
            }
            if (slot)
                y += row_height + px(8);
            y += px(18);
        }
        ReleaseDC(overview_, dc);
        overview_height_ = y;
        SCROLLINFO info{sizeof(info),       SIF_RANGE | SIF_PAGE | SIF_POS, 0,
                        std::max(0, y - 1), static_cast<UINT>(rect.bottom), overview_scroll_};
        SetScrollInfo(overview_, SB_VERT, &info, TRUE);
        InvalidateRect(overview_, nullptr, TRUE);
    }
    void Viewer::request(unsigned task)
    {
        if (task < tables_.size())
            tables_[task].state = L"Loading";
        enqueue({task});
    }
    void Viewer::enqueue(Work task)
    {
        task.epoch = epoch_;
        {
            std::scoped_lock lock(mutex_);
            if (task.task == resource_task)
                std::erase_if(tasks_, [](const Work& item) { return item.task == resource_task; });
            if (std::none_of(tasks_.begin(), tasks_.end(),
                             [&](const Work& item) { return item.task == task.task; }))
                tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
    }
    void Viewer::work()
    {
        std::optional<Summary> summary;
        std::wstring computed_hash;
        while (!stopping_)
        {
            Work job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                if (stopping_)
                    return;
                job = std::move(tasks_.front());
                tasks_.pop_front();
            }
            const auto task = job.task;
            cancel_operation_ = false;
            active_task_ = task;
            Completion result;
            result.task = task;
            result.generation = job.generation;
            result.epoch = job.epoch;
            const auto cancelled = [this] { return stopping_.load() || cancel_operation_.load(); };
            try
            {
                if (task == summary_task)
                {
                    summary.reset();
                    computed_hash.clear();
                    summary = read_summary(path_, cancelled);
                    result.summary = summary;
                }
                else if (task == hash_task && summary)
                {
                    computed_hash = sha256(path_, summary->identity, cancelled, [&](unsigned percent) {
                        Completion update;
                        update.task = 105;
                        update.epoch = job.epoch;
                        update.text = std::to_wstring(percent) + L"%";
                        {
                            std::scoped_lock lock(mutex_);
                            completed_.push_back(std::move(update));
                        }
                        PostMessageW(window_, ready_message, 0, 0);
                    });
                    result.text = computed_hash;
                }
                else if (task == 1 && summary)
                {
                    verify_identity(path_, summary->identity);
                    result.table = verify_signature(path_, cancelled);
                    verify_identity(path_, summary->identity);
                }
                else if (task < tables_.size() && summary)
                    result.table =
                        read_section(path_, summary->identity, static_cast<Section>(task), cancelled);
                else if (task == resource_task && summary)
                {
                    if (job.resource.type == 1 || job.resource.type == 2 || job.resource.type == 3 ||
                        job.resource.type == 12 || job.resource.type == 14)
                        try
                        {
                            result.image = resource_image(path_, summary->identity, job.resource);
                        }
                        catch (const std::exception&)
                        {
                        }
                    const auto data = read_resource(path_, summary->identity, job.resource, 65536);
                    result.text = job.resource.type == 24 ? decode_text(data) : std::wstring{};
                    if (job.resource.type == 6)
                    {
                        std::size_t cursor{};
                        for (unsigned i = 0; i < 16 && cursor + 2 <= data.size(); ++i)
                        {
                            WORD length{};
                            memcpy(&length, data.data() + cursor, 2);
                            cursor += 2;
                            if (length > (data.size() - cursor) / 2)
                                break;
                            if (length)
                                result.text +=
                                    std::to_wstring((job.resource.id - 1) * 16 + i) + L"\t" +
                                    std::wstring(reinterpret_cast<const wchar_t*>(data.data() + cursor),
                                                 length) +
                                    L"\r\n";
                            cursor += length * 2ULL;
                        }
                    }
                    if (result.text.empty())
                    {
                        std::wostringstream output;
                        for (std::size_t i = 0; i < std::min<std::size_t>(data.size(), 4096); ++i)
                        {
                            if (i % 16 == 0)
                                output << (i ? L"\r\n" : L"") << hex(i) << L"  ";
                            wchar_t byte[4]{};
                            swprintf_s(byte, L"%02X ", std::to_integer<unsigned>(data[i]));
                            output << byte;
                        }
                        result.text = output.str();
                    }
                    if (job.resource.length > 65536)
                        result.table.state = L"PreviewLimit";
                }
                else if (task == save_resource_task && summary)
                    atomic_output(job.destination, path_, [&](const std::wstring& temporary) {
                        const auto extension = std::filesystem::path(job.destination).extension().wstring();
                        if (_wcsicmp(extension.c_str(), L".png") == 0)
                            save_image(path_, summary->identity, job.resource, temporary);
                        else if (_wcsicmp(extension.c_str(), L".ico") == 0 ||
                                 _wcsicmp(extension.c_str(), L".cur") == 0)
                        {
                            const auto bytes = icon_file(path_, summary->identity, job.resource);
                            std::ofstream stream(std::filesystem::path(temporary), std::ios::binary);
                            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                            if (!stream)
                                throw std::runtime_error("SaveFailed");
                        }
                        else
                            export_resource(path_, summary->identity, job.resource, temporary, cancelled);
                    });
                else if (task == report_task && summary)
                {
                    std::array<Table, 7> tables;
                    tables[0] = summary->overview;
                    tables[0].rows.push_back(
                        {{L"SHA-256", computed_hash.empty() ? L"HashPending" : computed_hash}});
                    for (unsigned i = 1; i < tables.size(); ++i)
                    {
                        if (cancelled())
                            throw std::runtime_error("Cancelled");
                        tables[i] = i == 1 ? verify_signature(path_, cancelled)
                                           : read_section(path_, summary->identity, static_cast<Section>(i),
                                                          cancelled);
                    }
                    verify_identity(path_, summary->identity);
                    const auto text =
                        format_report(path_, tables, Resources(job.payload),
                                      !job.destination.empty() &&
                                          std::filesystem::path(job.destination).extension() != L".txt");
                    if (job.destination.empty())
                        result.text = text;
                    else
                        atomic_output(job.destination, path_,
                                      [&](const std::wstring& temporary) { write_utf8(temporary, text); });
                }
            }
            catch (const std::exception& error)
            {
                const std::string text(error.what());
                result.table.state.assign(text.begin(), text.end());
            }
            {
                std::scoped_lock lock(mutex_);
                completed_.push_back(std::move(result));
            }
            active_task_ = 999;
            if (!stopping_)
                PostMessageW(window_, ready_message, 0, 0);
        }
    }
    void Viewer::complete()
    {
        std::deque<Completion> results;
        bool rebuild{};
        {
            std::scoped_lock lock(mutex_);
            results.swap(completed_);
        }
        for (auto& result : results)
        {
            if (result.epoch != epoch_)
                continue;
            if (result.task == 105)
            {
                SetWindowTextW(status_, result.text.c_str());
                continue;
            }
            if (result.summary)
            {
                summary_ = std::move(result.summary);
                for (auto& table : tables_)
                {
                    table = {};
                    table.state = L"NotLoaded";
                }
                tables_[0] = summary_->overview;
                rebuild = true;
                if (icon_)
                    DestroyIcon(icon_);
                icon_ = nullptr;
                if (!summary_->icon.empty())
                    icon_ = CreateIconFromResourceEx(reinterpret_cast<PBYTE>(summary_->icon.data()),
                                                     static_cast<DWORD>(summary_->icon.size()), TRUE,
                                                     0x00030000, px(48), px(48), LR_DEFAULTCOLOR);
                language(language_);
                if (summary_->identity.size <= 64ULL * 1024 * 1024)
                    request(1);
            }
            else if (result.task == summary_task)
            {
                tables_[0] = std::move(result.table);
                rebuild = section_ == 0;
            }
            else if (result.task == hash_task)
            {
                hash_ = result.text;
                auto& rows = tables_[0].rows;
                std::erase_if(
                    rows, [](const Row& row) { return !row.cells.empty() && row.cells[0] == L"SHA-256"; });
                rows.push_back({{L"SHA-256", hash_.empty() ? strings_.text(result.table.state) : hash_}});
                rebuild = section_ == 0;
            }
            else if (result.task < tables_.size())
            {
                tables_[result.task] = std::move(result.table);
                rebuild = rebuild || result.task == section_;
            }
            else if (result.task == resource_task && result.generation == resource_generation_)
            {
                if (result.table.state != L"Complete")
                    result.text += L"\r\n" + strings_.text(result.table.state);
                SetWindowTextW(details_, result.text.c_str());
                resource_image_ = std::move(result.image);
                layout();
                InvalidateRect(picture_, nullptr, TRUE);
            }
            else if (result.task == report_task || result.task == save_resource_task)
            {
                if (result.task == report_task && !result.text.empty())
                    clipboard(window_, result.text);
                SetWindowTextW(status_, strings_.text(result.table.state).c_str());
            }
        }
        if (rebuild)
            refresh();
        InvalidateRect(window_, nullptr, TRUE);
    }
    void Viewer::copy()
    {
        std::wstring text;
        for (int item = ListView_GetNextItem(list_, -1, LVNI_SELECTED); item >= 0;
             item = ListView_GetNextItem(list_, item, LVNI_SELECTED))
        {
            if (static_cast<std::size_t>(item) >= filtered_.size())
                continue;
            const auto& row = tables_[section_].rows[filtered_[item]];
            for (std::size_t i = 0; i < row.cells.size(); ++i)
            {
                if (i)
                    text += L'\t';
                text += cell(row, i);
            }
            text += L"\r\n";
        }
        if (text.empty())
        {
            const auto& table = tables_[section_];
            text = strings_.text(section_keys[section_]) + L"\r\n";
            for (const auto& row : table.rows)
            {
                for (std::size_t i = 0; i < row.cells.size(); ++i)
                {
                    if (i)
                        text += L'\t';
                    text += cell(row, i);
                }
                text += L"\r\n";
            }
        }
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (!memory)
            return;
        auto* pointer = GlobalLock(memory);
        if (!pointer)
        {
            GlobalFree(memory);
            return;
        }
        memcpy(pointer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        if (OpenClipboard(window_))
        {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
            CloseClipboard();
        }
        else
            GlobalFree(memory);
    }
    void Viewer::reload()
    {
        ++epoch_;
        cancel_operation_ = true;
        {
            std::scoped_lock lock(mutex_);
            tasks_.clear();
            completed_.clear();
        }
        for (auto& table : tables_)
        {
            table = {};
            table.state = L"NotLoaded";
        }
        summary_.reset();
        hash_.clear();
        resource_image_ = {};
        for (auto& selected : selections_)
            selected.clear();
        top_rows_.fill(0);
        overview_scroll_ = 0;
        request(summary_task);
        tables_[0].state = L"Loading";
        section_ = 0;
        refresh();
    }
    void Viewer::save_report()
    {
        const auto path = save_path(window_, L"json", L"JSON (*.json)\0*.json\0Text (*.txt)\0*.txt\0\0");
        if (!path.empty())
        {
            SetWindowTextW(status_, strings_.text(L"Loading").c_str());
            enqueue({report_task, {}, path, language_});
        }
    }
    void Viewer::save_resource()
    {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (!summary_ || selected < 0 || static_cast<std::size_t>(selected) >= filtered_.size())
            return;
        const auto& row = tables_[4].rows[filtered_[selected]];
        const auto path =
            row.type == 14   ? save_path(window_, L"ico", L"ICO (*.ico)\0*.ico\0Binary (*.bin)\0*.bin\0\0")
            : row.type == 12 ? save_path(window_, L"cur", L"CUR (*.cur)\0*.cur\0Binary (*.bin)\0*.bin\0\0")
            : row.type == 2  ? save_path(window_, L"png", L"PNG (*.png)\0*.png\0Binary (*.bin)\0*.bin\0\0")
                             : save_path(window_, L"bin", L"Binary (*.bin)\0*.bin\0\0");
        if (path.empty())
            return;
        // CREATE_NEW prevents accidentally replacing the source file or a concurrent writer.
        enqueue({save_resource_task, tables_[4].rows[filtered_[selected]], path});
    }
    void Viewer::preview_resource()
    {
        if (updating_ || section_ != 4 || !summary_)
            return;
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= filtered_.size())
            return;
        const auto& row = tables_[4].rows[filtered_[selected]];
        SetWindowTextW(details_, strings_.text(L"Loading").c_str());
        resource_image_ = {};
        layout();
        enqueue({resource_task, row, {}, {}, ++resource_generation_});
    }
    void Viewer::paint()
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT rect{};
        GetClientRect(window_, &rect);
        FillRect(dc, &rect, brush_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground_);
        if (icon_)
            DrawIconEx(dc, px(24), px(25), icon_, px(48), px(48), 0, nullptr, DI_NORMAL);
        const int left = icon_ ? px(88) : px(24);
        SelectObject(dc, heading_);
        RECT title{left, px(20), rect.right - px(24), px(52)};
        const auto text = summary_ ? summary_->title : std::filesystem::path(path_).filename().wstring();
        DrawTextW(dc, text.c_str(), -1, &title, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, font_);
        RECT subtitle{left, px(58), rect.right - px(24), px(85)};
        const auto detail = summary_ ? (summary_->version.empty() ? L"" : summary_->version + L"   ·   ") +
                                           summary_->architecture + L"   ·   " + summary_->type +
                                           (summary_->managed ? L"   ·   .NET" : L"")
                                     : strings_.text(L"Loading");
        DrawTextW(dc, detail.c_str(), -1, &subtitle, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        EndPaint(window_, &paint);
    }
    bool Viewer::key(MSG& message)
    {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd)))
            return false;
        if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (message.wParam == 'F')
            {
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
                return true;
            }
            if (message.wParam == 'C' && GetFocus() == list_)
            {
                copy();
                return true;
            }
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }
    LRESULT CALLBACK Viewer::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<Viewer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        try
        {
            if (self)
                return self->message(message, wparam, lparam);
        }
        catch (...)
        {
            if (self && self->status_)
                SetWindowTextW(self->status_, self->strings_.text(L"Error").c_str());
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    LRESULT Viewer::message(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            layout();
            return 0;
        case ready_message:
            complete();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wparam), foreground_);
            SetBkColor(reinterpret_cast<HDC>(wparam), background_);
            return reinterpret_cast<LRESULT>(brush_);
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw->CtlID == 21)
            {
                FillRect(draw->hDC, &draw->rcItem, brush_);
                if (resource_image_.bitmap)
                {
                    const auto dc = CreateCompatibleDC(draw->hDC);
                    const auto previous = SelectObject(dc, resource_image_.bitmap.get());
                    const double scale = std::min({1.0, double(draw->rcItem.right) / resource_image_.width,
                                                   double(draw->rcItem.bottom) / resource_image_.height});
                    const auto width = static_cast<int>(resource_image_.width * scale),
                               height = static_cast<int>(resource_image_.height * scale);
                    SetStretchBltMode(draw->hDC, HALFTONE);
                    StretchBlt(draw->hDC, (draw->rcItem.right - width) / 2,
                               (draw->rcItem.bottom - height) / 2, width, height, dc, 0, 0,
                               resource_image_.width, resource_image_.height, SRCCOPY);
                    SelectObject(dc, previous);
                    DeleteDC(dc);
                }
                return TRUE;
            }
            if (draw->CtlID != 10 || draw->itemID >= navigation_.size())
                break;
            FillRect(draw->hDC, &draw->rcItem, brush_);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, foreground_);
            SelectObject(draw->hDC, font_);
            RECT rect = draw->rcItem;
            rect.left += px(14);
            const auto text = strings_.text(section_keys[navigation_[draw->itemID]]);
            DrawTextW(draw->hDC, text.c_str(), -1, &rect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (draw->itemState & ODS_SELECTED)
            {
                RECT bar = draw->rcItem;
                bar.right = bar.left + px(3);
                bar.top += px(10);
                bar.bottom -= px(10);
                FillRect(draw->hDC, &bar, GetSysColorBrush(COLOR_HIGHLIGHT));
            }
            if (draw->itemState & ODS_FOCUS)
                DrawFocusRect(draw->hDC, &draw->rcItem);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == 10 && HIWORD(wparam) == LBN_SELCHANGE)
            {
                const auto index = SendMessageW(nav_, LB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < navigation_.size())
                    select(navigation_[index]);
            }
            if (LOWORD(wparam) == 11 && HIWORD(wparam) == CBN_SELCHANGE)
            {
                const auto index = SendMessageW(compact_, CB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < navigation_.size())
                    select(navigation_[index]);
            }
            if (LOWORD(wparam) == 12 && HIWORD(wparam) == EN_CHANGE && !updating_)
            {
                if (section_ == 0)
                    refresh();
                else
                    filter();
            }
            if (LOWORD(wparam) == 19 && HIWORD(wparam) == LBN_SELCHANGE)
            {
                const auto index = SendMessageW(master_, LB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < masters_.size())
                {
                    master_filter_ = masters_[index];
                    filter();
                }
            }
            if (LOWORD(wparam) == 15)
                copy();
            if (LOWORD(wparam) == 16)
                save_report();
            if (LOWORD(wparam) == 17)
            {
                if (text_of(action_) == strings_.text(L"Cancel"))
                {
                    cancel_operation_ = true;
                    return 0;
                }
                if (section_ == 0 && summary_)
                {
                    SetWindowTextW(status_, strings_.text(L"Loading").c_str());
                    SetWindowTextW(action_, strings_.text(L"Cancel").c_str());
                    request(hash_task);
                }
                else if (section_ == 4)
                    save_resource();
                else if (summary_)
                {
                    if (section_ == 1)
                    {
                        request(1);
                        SetWindowTextW(action_, strings_.text(L"Cancel").c_str());
                    }
                    else
                        reload();
                }
            }
            return 0;
        case WM_CONTEXTMENU: {
            const auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, strings_.text(L"Copy").c_str());
            AppendMenuW(menu, MF_STRING, 2, strings_.text(L"CopyReport").c_str());
            AppendMenuW(menu, MF_STRING, 3, strings_.text(L"Report").c_str());
            AppendMenuW(menu, MF_STRING, 4, strings_.text(L"Reload").c_str());
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (point.x == -1 && point.y == -1)
                GetCursorPos(&point);
            const auto selected =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
            DestroyMenu(menu);
            if (selected == 1)
                copy();
            if (selected == 2)
            {
                SetWindowTextW(status_, strings_.text(L"Loading").c_str());
                enqueue({report_task, {}, {}, language_});
            }
            if (selected == 3)
                save_report();
            if (selected == 4)
                reload();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header->hwndFrom != list_)
                break;
            if (header->code == LVN_GETDISPINFOW)
            {
                auto* info = reinterpret_cast<NMLVDISPINFOW*>(lparam);
                if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 &&
                    static_cast<std::size_t>(info->item.iItem) < filtered_.size())
                {
                    const auto text = cell(tables_[section_].rows[filtered_[info->item.iItem]],
                                           static_cast<std::size_t>(info->item.iSubItem));
                    wcsncpy_s(info->item.pszText, static_cast<std::size_t>(info->item.cchTextMax),
                              text.c_str(), _TRUNCATE);
                }
            }
            if (header->code == LVN_COLUMNCLICK)
            {
                const auto* click = reinterpret_cast<NMLISTVIEW*>(lparam);
                descending_ = sort_column_ == click->iSubItem && !descending_;
                sort_column_ = click->iSubItem;
                filter();
            }
            if (header->code == LVN_ITEMCHANGED)
                preview_resource();
            return 0;
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
} // namespace glance::executable
