#include "pch.h"
#include "scintilla_text_view.h"

#include "syntax_theme.h"

#include "third_party/scintilla/include/ILexer.h"
#include "third_party/scintilla/include/Lexilla.h"
#include "third_party/scintilla/include/SciLexer.h"
#include "third_party/scintilla/include/Scintilla.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <uxtheme.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
    constexpr wchar_t host_window_class[] = L"Glance.ScintillaHost";
    constexpr UINT_PTR editor_subclass_id = 1;

    struct LexerDefinition
    {
        std::string_view name;
        std::array<std::string_view, 3> keywords;
    };

    HMODULE scintilla_module{};
    HMODULE lexilla_module{};
    Lexilla::CreateLexerFn create_lexer{};
    std::once_flag host_class_once;

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            path.data(),
            static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    bool load_scintilla()
    {
        if (scintilla_module != nullptr)
        {
            return true;
        }
        const auto path = executable_directory() / L"Scintilla.dll";
        scintilla_module = LoadLibraryExW(
            path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        return scintilla_module != nullptr;
    }

    bool load_lexilla()
    {
        if (create_lexer != nullptr)
        {
            return true;
        }
        const auto path = executable_directory() / L"Lexilla.dll";
        lexilla_module = LoadLibraryExW(
            path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (lexilla_module == nullptr)
        {
            return false;
        }
        create_lexer = reinterpret_cast<Lexilla::CreateLexerFn>(
            GetProcAddress(lexilla_module, LEXILLA_CREATELEXER));
        return create_lexer != nullptr;
    }

    std::string utf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            length,
            nullptr,
            nullptr);
        return result;
    }

    COLORREF color_ref(std::uint32_t color) noexcept
    {
        return RGB(
            (color >> 16U) & 0xFFU,
            (color >> 8U) & 0xFFU,
            color & 0xFFU);
    }

    std::wstring lower_extension(std::wstring_view path)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        return extension;
    }

    LexerDefinition lexer_for_path(std::wstring_view path)
    {
        static constexpr std::string_view cpp_keywords =
            "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto "
            "bitand bitor bool break case catch char char8_t char16_t char32_t class compl "
            "concept const consteval constexpr constinit const_cast continue co_await co_return "
            "co_yield decltype default delete do double dynamic_cast else enum explicit export "
            "extern false float for friend goto if inline int long mutable namespace new noexcept "
            "not not_eq nullptr operator or or_eq private protected public register reinterpret_cast "
            "requires return short signed sizeof static static_assert static_cast struct switch "
            "synchronized template this thread_local throw true try typedef typeid typename union "
            "unsigned using virtual void volatile wchar_t while xor xor_eq";
        static constexpr std::string_view csharp_keywords =
            "abstract as base bool break byte case catch char checked class const continue decimal "
            "default delegate do double else enum event explicit extern false finally fixed float "
            "for foreach goto if implicit in int interface internal is lock long namespace new null "
            "object operator out override params private protected public readonly ref return sbyte "
            "sealed short sizeof stackalloc static string struct switch this throw true try typeof "
            "uint ulong unchecked unsafe ushort using virtual void volatile while async await record";
        static constexpr std::string_view java_keywords =
            "abstract assert boolean break byte case catch char class const continue default do "
            "double else enum extends false final finally float for goto if implements import "
            "instanceof int interface long native new null package private protected public return "
            "short static strictfp super switch synchronized this throw throws transient true try "
            "void volatile while";
        static constexpr std::string_view javascript_keywords =
            "async await break case catch class const continue debugger default delete do else "
            "export extends false finally for from function get if implements import in instanceof "
            "interface let new null of package private protected public return set static super "
            "switch this throw true try typeof undefined var void while with yield";
        static constexpr std::string_view go_keywords =
            "break case chan const continue default defer else fallthrough for func go goto if "
            "import interface map package range return select struct switch type var";
        static constexpr std::string_view python_keywords =
            "and as assert async await break class continue def del elif else except False finally "
            "for from global if import in is lambda None nonlocal not or pass raise return True try "
            "while with yield";
        static constexpr std::string_view rust_keywords =
            "as async await break const continue crate dyn else enum extern false fn for if impl in "
            "let loop match mod move mut pub ref return self Self static struct super trait true try "
            "type unsafe use where while";
        static constexpr std::string_view sql_keywords =
            "add all alter and any as asc authorization backup begin between break browse bulk by "
            "cascade case check checkpoint close clustered coalesce collate column commit compute "
            "constraint contains continue convert create cross current cursor database dbcc "
            "deallocate declare default delete deny desc distinct distributed double drop dump else "
            "end errlvl escape except exec execute exists exit external fetch file fillfactor for "
            "foreign freetext from full function goto grant group having holdlock identity if in "
            "index inner insert intersect into is join key kill left like lineno load merge national "
            "nocheck nonclustered not null nullif of off offsets on open opendatasource openquery "
            "openrowset openxml option or order outer over percent pivot plan precision primary "
            "print proc procedure public raiserror read readtext reconfigure references replication "
            "restore restrict return revert revoke right rollback rowcount rule save schema select "
            "set shutdown some statistics table tablesample textsize then to top tran transaction "
            "trigger truncate try_convert tsequal union unique unpivot update updatetext use user "
            "values varying view waitfor when where while with writetext";
        static constexpr std::string_view shell_keywords =
            "case do done elif else esac fi for function if in select then time until while";
        static constexpr std::string_view powershell_keywords =
            "begin break catch class continue data define do dynamicparam else elseif end enum exit "
            "filter finally for foreach from function if in inlinescript parallel param process "
            "return sequence switch throw trap try until using var while workflow";
        static constexpr std::string_view batch_keywords =
            "assoc break call cd chdir cls color copy date del dir echo endlocal erase exit for ftype "
            "goto if md mkdir mklink move path pause popd prompt pushd rd rem ren rename rmdir set "
            "setlocal shift start time title type ver verify vol";

        const auto extension = lower_extension(path);
        if (extension == L".py" || extension == L".pyw")
        {
            return { "python", { python_keywords, {}, {} } };
        }
        if (extension == L".rs")
        {
            return { "rust", { rust_keywords, {}, {} } };
        }
        if (extension == L".json" || extension == L".jsonc")
        {
            return { "json", { "false null true", {}, {} } };
        }
        if (extension == L".yaml" || extension == L".yml")
        {
            return { "yaml", { "false null true yes no on off", {}, {} } };
        }
        if (extension == L".toml")
        {
            return { "toml", { "false true", {}, {} } };
        }
        if (extension == L".xml" || extension == L".xaml" ||
            extension == L".vcxproj" || extension == L".props" ||
            extension == L".targets" || extension == L".svg")
        {
            return { "xml", {} };
        }
        if (extension == L".html" || extension == L".htm" ||
            extension == L".xhtml" || extension == L".shtml")
        {
            return { "hypertext", {} };
        }
        if (extension == L".css" || extension == L".scss" || extension == L".less")
        {
            return { "css", {} };
        }
        if (extension == L".sql")
        {
            return { "sql", { sql_keywords, {}, {} } };
        }
        if (extension == L".sh" || extension == L".bash" ||
            extension == L".zsh")
        {
            return { "bash", { shell_keywords, {}, {} } };
        }
        if (extension == L".ps1" || extension == L".psm1" || extension == L".psd1")
        {
            return { "powershell", { powershell_keywords, {}, {} } };
        }
        if (extension == L".bat" || extension == L".cmd")
        {
            return { "batch", { batch_keywords, {}, {} } };
        }
        if (extension == L".cmake" ||
            std::filesystem::path(path).filename().wstring() == L"CMakeLists.txt")
        {
            return { "cmake", {} };
        }
        if (extension == L".ini" || extension == L".cfg" ||
            extension == L".conf" || extension == L".sln")
        {
            return { "props", {} };
        }
        if (extension == L".md" || extension == L".markdown")
        {
            return { "markdown", {} };
        }
        if (extension == L".diff" || extension == L".patch")
        {
            return { "diff", {} };
        }
        if (extension == L".cs")
        {
            return { "cpp", { csharp_keywords, {}, {} } };
        }
        if (extension == L".java")
        {
            return { "cpp", { java_keywords, {}, {} } };
        }
        if (extension == L".js" || extension == L".jsx" ||
            extension == L".ts" || extension == L".tsx")
        {
            return { "cpp", { javascript_keywords, {}, {} } };
        }
        if (extension == L".go")
        {
            return { "cpp", { go_keywords, {}, {} } };
        }
        if (extension == L".c" || extension == L".h" ||
            extension == L".cc" || extension == L".cpp" ||
            extension == L".cxx" || extension == L".hpp" ||
            extension == L".hxx")
        {
            return { "cpp", { cpp_keywords, {}, {} } };
        }
        return { "null", {} };
    }

    void register_host_window_class(HINSTANCE instance)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = glance::app::ScintillaTextView::host_window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        window_class.lpszClassName = host_window_class;
        RegisterClassExW(&window_class);
    }

}

namespace glance::app
{
    ScintillaTextView::ScintillaTextView(
        HWND parent,
        NearEndCallback near_end_callback,
        FontZoomCallback font_zoom_callback)
        : parent_(parent),
          near_end_callback_(std::move(near_end_callback)),
          font_zoom_callback_(std::move(font_zoom_callback))
    {
        if (!load_scintilla())
        {
            error_ = L"Scintilla.dll is unavailable.";
            return;
        }

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        std::call_once(host_class_once, register_host_window_class, instance);
        host_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            host_window_class,
            nullptr,
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            0,
            0,
            parent_,
            nullptr,
            instance,
            this);
        if (host_ == nullptr)
        {
            error_ = L"Unable to create the text preview host.";
            return;
        }
        editor_ = CreateWindowExW(
            0,
            L"Scintilla",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
            0,
            0,
            0,
            0,
            host_,
            nullptr,
            instance,
            nullptr);
        if (editor_ == nullptr)
        {
            error_ = L"Unable to create the Scintilla text control.";
            DestroyWindow(host_);
            host_ = nullptr;
            return;
        }
        SetWindowSubclass(
            editor_,
            editor_subclass,
            editor_subclass_id,
            reinterpret_cast<DWORD_PTR>(this));
        configure();
    }

    ScintillaTextView::~ScintillaTextView()
    {
        if (editor_ != nullptr && IsWindow(editor_))
        {
            RemoveWindowSubclass(editor_, editor_subclass, editor_subclass_id);
        }
        if (host_ != nullptr && IsWindow(host_))
        {
            DestroyWindow(host_);
        }
    }

    bool ScintillaTextView::available() const noexcept
    {
        return editor_ != nullptr;
    }

    std::wstring ScintillaTextView::error() const
    {
        return error_;
    }

    bool ScintillaTextView::should_load_more() const noexcept
    {
        if (editor_ == nullptr)
        {
            return false;
        }
        const auto line_count = std::max<LRESULT>(1, call(SCI_GETLINECOUNT));
        const auto first_visible = std::max<LRESULT>(0, call(SCI_GETFIRSTVISIBLELINE));
        const auto visible_lines = std::max<LRESULT>(1, call(SCI_LINESONSCREEN));
        return first_visible + visible_lines + 4 >= line_count ||
            (first_visible + visible_lines) * 4 >= line_count * 3;
    }

    void ScintillaTextView::set_bounds(
        int x,
        int y,
        int width,
        int height) noexcept
    {
        if (host_ == nullptr)
        {
            return;
        }
        POINT origin{ x, y };
        ClientToScreen(parent_, &origin);
        SetWindowPos(
            host_,
            HWND_TOP,
            origin.x,
            origin.y,
            std::max(0, width),
            std::max(0, height),
            SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
    }

    void ScintillaTextView::set_visible(bool visible) noexcept
    {
        visible_ = visible;
        if (host_ != nullptr)
        {
            ShowWindow(host_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
    }

    void ScintillaTextView::set_occlusions(
        std::span<const RECT> rectangles) noexcept
    {
        if (host_ == nullptr)
        {
            return;
        }
        if (rectangles.empty())
        {
            SetWindowRgn(host_, nullptr, TRUE);
            return;
        }

        RECT bounds{};
        if (!GetClientRect(host_, &bounds))
        {
            return;
        }
        const HRGN visible_region = CreateRectRgnIndirect(&bounds);
        if (visible_region == nullptr)
        {
            return;
        }
        for (const auto& rectangle : rectangles)
        {
            RECT clipped{
                std::clamp(rectangle.left, bounds.left, bounds.right),
                std::clamp(rectangle.top, bounds.top, bounds.bottom),
                std::clamp(rectangle.right, bounds.left, bounds.right),
                std::clamp(rectangle.bottom, bounds.top, bounds.bottom),
            };
            if (clipped.left >= clipped.right || clipped.top >= clipped.bottom)
            {
                continue;
            }
            const HRGN excluded_region = CreateRectRgnIndirect(&clipped);
            if (excluded_region != nullptr)
            {
                CombineRgn(
                    visible_region,
                    visible_region,
                    excluded_region,
                    RGN_DIFF);
                DeleteObject(excluded_region);
            }
        }
        if (SetWindowRgn(host_, visible_region, TRUE) == 0)
        {
            DeleteObject(visible_region);
        }
    }

    void ScintillaTextView::clear() noexcept
    {
        if (editor_ == nullptr)
        {
            return;
        }
        call(SCI_SETREADONLY, FALSE);
        call(SCI_CLEARALL);
        call(SCI_EMPTYUNDOBUFFER);
        call(SCI_SETREADONLY, TRUE);
        update_line_number_width();
    }

    void ScintillaTextView::append_text(std::wstring_view text)
    {
        if (editor_ == nullptr || text.empty())
        {
            return;
        }
        const auto encoded = utf8(text);
        if (encoded.empty())
        {
            return;
        }
        call(SCI_SETREADONLY, FALSE);
        call(
            SCI_APPENDTEXT,
            static_cast<WPARAM>(encoded.size()),
            reinterpret_cast<LPARAM>(encoded.data()));
        call(SCI_SETREADONLY, TRUE);
        update_line_number_width();
    }

    void ScintillaTextView::set_file_path(std::wstring_view path)
    {
        path_.assign(path);
        const auto lexer = lexer_for_path(path);
        lexer_name_.assign(lexer.name);
        for (std::size_t index = 0; index < lexer_keywords_.size(); ++index)
        {
            lexer_keywords_[index].assign(lexer.keywords[index]);
        }
        if (editor_ != nullptr)
        {
            update_lexer();
        }
    }

    void ScintillaTextView::set_preferences(
        const TextPreferences& preferences,
        bool syntax_highlighting,
        bool dark)
    {
        preferences_ = preferences;
        syntax_highlighting_ = syntax_highlighting;
        dark_ = dark;
        if (editor_ == nullptr)
        {
            return;
        }
        const auto font = utf8(preferences.font_family);
        call(
            SCI_STYLESETFONT,
            STYLE_DEFAULT,
            reinterpret_cast<LPARAM>(font.c_str()));
        call(
            SCI_STYLESETSIZEFRACTIONAL,
            STYLE_DEFAULT,
            static_cast<LPARAM>(std::clamp(preferences.font_size, 7.0, 32.0) * SC_FONT_SIZE_MULTIPLIER));
        call(SCI_STYLECLEARALL);
        set_word_wrap(preferences.word_wrap);
        set_line_numbers(preferences.line_numbers);
        apply_theme(dark);
        update_lexer();
    }

    void ScintillaTextView::set_word_wrap(bool enabled) noexcept
    {
        preferences_.word_wrap = enabled;
        if (editor_ == nullptr)
        {
            return;
        }
        call(SCI_SETWRAPMODE, enabled ? SC_WRAP_WORD : SC_WRAP_NONE);
        call(SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_NONE);
        call(SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_FIXED);
    }

    void ScintillaTextView::set_line_numbers(bool enabled) noexcept
    {
        preferences_.line_numbers = enabled;
        if (editor_ == nullptr)
        {
            return;
        }
        call(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
        if (enabled)
        {
            update_line_number_width();
        }
        else
        {
            call(SCI_SETMARGINWIDTHN, 0, 0);
        }
    }

    void ScintillaTextView::set_syntax_highlighting(bool enabled)
    {
        syntax_highlighting_ = enabled;
        update_lexer();
    }

    LRESULT CALLBACK ScintillaTextView::host_window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<ScintillaTextView*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ScintillaTextView*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr)
        {
            if (message == WM_MOUSEACTIVATE)
            {
                return MA_NOACTIVATE;
            }
            if (message == WM_SIZE && self->editor_ != nullptr)
            {
                MoveWindow(
                    self->editor_,
                    0,
                    0,
                    LOWORD(lparam),
                    HIWORD(lparam),
                    TRUE);
                if (self->near_end_callback_ && self->should_load_more())
                {
                    self->near_end_callback_();
                }
                return 0;
            }
            if (message == WM_NOTIFY)
            {
                self->handle_notification(*reinterpret_cast<NMHDR*>(lparam));
                return 0;
            }
            if (message == WM_ERASEBKGND)
            {
                return 1;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT CALLBACK ScintillaTextView::editor_subclass(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR,
        DWORD_PTR reference_data) noexcept
    {
        auto* self = reinterpret_cast<ScintillaTextView*>(reference_data);
        if (message == WM_MOUSEWHEEL && self != nullptr &&
            (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) != 0)
        {
            self->wheel_delta_ += GET_WHEEL_DELTA_WPARAM(wparam);
            while (std::abs(self->wheel_delta_) >= WHEEL_DELTA)
            {
                const int direction = self->wheel_delta_ > 0 ? 1 : -1;
                self->wheel_delta_ -= direction * WHEEL_DELTA;
                if (self->font_zoom_callback_)
                {
                    self->font_zoom_callback_(direction);
                }
            }
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(window, editor_subclass, editor_subclass_id);
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    LRESULT ScintillaTextView::call(
        UINT message,
        WPARAM wparam,
        LPARAM lparam) const noexcept
    {
        return editor_ == nullptr
            ? 0
            : SendMessageW(editor_, message, wparam, lparam);
    }

    void ScintillaTextView::configure()
    {
        call(SCI_SETCODEPAGE, SC_CP_UTF8);
        call(SCI_SETREADONLY, TRUE);
        call(SCI_SETUNDOCOLLECTION, FALSE);
        call(SCI_SETMODEVENTMASK, 0);
        call(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
        call(SCI_SETBUFFEREDDRAW, TRUE);
        call(SCI_SETPHASESDRAW, SC_PHASES_MULTIPLE);
        call(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
        call(
            SCI_SETLAYOUTTHREADS,
            std::max(1U, std::thread::hardware_concurrency()));
        call(SCI_SETIDLESTYLING, SC_IDLESTYLING_AFTERVISIBLE);
        call(SCI_SETSCROLLWIDTH, 1);
        call(SCI_SETSCROLLWIDTHTRACKING, TRUE);
        call(SCI_SETMARGINLEFT, 0, 12);
        call(SCI_SETMARGINRIGHT, 0, 12);
        call(SCI_SETEXTRAASCENT, 2);
        call(SCI_SETEXTRADESCENT, 2);
        call(SCI_SETCARETWIDTH, 0);
        call(SCI_SETMOUSEDWELLTIME, SC_TIME_FOREVER);
        call(SCI_SETMARGINS, 1);
        call(SCI_SETMARGINSENSITIVEN, 0, FALSE);
        call(SCI_SETMARGINCURSORN, 0, SC_CURSORARROW);
    }

    void ScintillaTextView::update_lexer()
    {
        if (editor_ == nullptr)
        {
            return;
        }
        if (!syntax_highlighting_ || lexer_name_.empty() || lexer_name_ == "null")
        {
            call(SCI_SETILEXER, 0, 0);
            call(SCI_CLEARDOCUMENTSTYLE);
            apply_theme(dark_);
            return;
        }
        if (!load_lexilla())
        {
            call(SCI_SETILEXER, 0, 0);
            return;
        }
        auto* lexer = create_lexer(lexer_name_.c_str());
        if (lexer == nullptr)
        {
            call(SCI_SETILEXER, 0, 0);
            return;
        }
        call(SCI_SETILEXER, 0, reinterpret_cast<LPARAM>(lexer));
        for (std::size_t index = 0; index < lexer_keywords_.size(); ++index)
        {
            call(
                SCI_SETKEYWORDS,
                static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(lexer_keywords_[index].c_str()));
        }
        apply_lexer_styles();
        call(SCI_COLOURISE, 0, -1);
    }

    void ScintillaTextView::apply_theme(bool dark)
    {
        dark_ = dark;
        if (editor_ == nullptr)
        {
            return;
        }
        SetWindowTheme(host_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(editor_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(host_, WM_THEMECHANGED, 0, 0);
        SendMessageW(editor_, WM_THEMECHANGED, 0, 0);
        const auto& palette = syntax_theme_palette(preferences_.syntax_theme, dark);
        call(SCI_STYLESETFORE, STYLE_DEFAULT, color_ref(palette.foreground));
        call(SCI_STYLESETBACK, STYLE_DEFAULT, color_ref(palette.background));
        call(SCI_STYLECLEARALL);
        call(SCI_STYLESETFORE, STYLE_LINENUMBER, color_ref(palette.line_number));
        call(SCI_STYLESETBACK, STYLE_LINENUMBER, color_ref(palette.line_number_background));
        call(SCI_SETSELBACK, TRUE, color_ref(palette.selection));
        call(SCI_SETCARETFORE, color_ref(palette.foreground));
        apply_lexer_styles();
        update_line_number_width();
    }

    void ScintillaTextView::apply_lexer_styles()
    {
        if (editor_ == nullptr)
        {
            return;
        }
        const auto& palette = syntax_theme_palette(preferences_.syntax_theme, dark_);
        const auto style = [this](std::initializer_list<int> styles, std::uint32_t color) {
            for (const int value : styles)
            {
                call(SCI_STYLESETFORE, value, color_ref(color));
            }
        };
        const auto bold = [this](std::initializer_list<int> styles) {
            for (const int value : styles)
            {
                call(SCI_STYLESETBOLD, value, TRUE);
            }
        };

        if (lexer_name_ == "cpp")
        {
            style({ SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC,
                    SCE_C_COMMENTLINEDOC, SCE_C_PREPROCESSORCOMMENT,
                    SCE_C_PREPROCESSORCOMMENTDOC, SCE_C_TASKMARKER }, palette.comment);
            style({ SCE_C_WORD }, palette.keyword);
            style({ SCE_C_WORD2, SCE_C_GLOBALCLASS }, palette.type);
            style({ SCE_C_STRING, SCE_C_CHARACTER, SCE_C_STRINGEOL, SCE_C_VERBATIM,
                    SCE_C_REGEX, SCE_C_STRINGRAW, SCE_C_TRIPLEVERBATIM,
                    SCE_C_HASHQUOTEDSTRING, SCE_C_USERLITERAL }, palette.string);
            style({ SCE_C_NUMBER }, palette.number);
            style({ SCE_C_PREPROCESSOR }, palette.preprocessor);
            style({ SCE_C_ESCAPESEQUENCE }, palette.escape);
            style({ SCE_C_COMMENTDOCKEYWORDERROR }, palette.error);
            bold({ SCE_C_WORD, SCE_C_PREPROCESSOR });
        }
        else if (lexer_name_ == "python")
        {
            style({ SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK }, palette.comment);
            style({ SCE_P_WORD, SCE_P_WORD2 }, palette.keyword);
            style({ SCE_P_CLASSNAME }, palette.type);
            style({ SCE_P_DEFNAME, SCE_P_DECORATOR, SCE_P_ATTRIBUTE }, palette.function);
            style({ SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE,
                    SCE_P_STRINGEOL, SCE_P_FSTRING, SCE_P_FCHARACTER, SCE_P_FTRIPLE,
                    SCE_P_FTRIPLEDOUBLE }, palette.string);
            style({ SCE_P_NUMBER }, palette.number);
            bold({ SCE_P_WORD, SCE_P_CLASSNAME, SCE_P_DEFNAME });
        }
        else if (lexer_name_ == "rust")
        {
            style({ SCE_RUST_COMMENTBLOCK, SCE_RUST_COMMENTLINE,
                    SCE_RUST_COMMENTBLOCKDOC, SCE_RUST_COMMENTLINEDOC }, palette.comment);
            style({ SCE_RUST_WORD, SCE_RUST_WORD2, SCE_RUST_WORD3 }, palette.keyword);
            style({ SCE_RUST_WORD4, SCE_RUST_WORD5, SCE_RUST_WORD6,
                    SCE_RUST_WORD7 }, palette.type);
            style({ SCE_RUST_STRING, SCE_RUST_STRINGR, SCE_RUST_CHARACTER,
                    SCE_RUST_BYTESTRING, SCE_RUST_BYTESTRINGR,
                    SCE_RUST_BYTECHARACTER, SCE_RUST_CSTRING, SCE_RUST_CSTRINGR }, palette.string);
            style({ SCE_RUST_NUMBER }, palette.number);
            style({ SCE_RUST_LIFETIME, SCE_RUST_MACRO }, palette.preprocessor);
            style({ SCE_RUST_LEXERROR }, palette.error);
            bold({ SCE_RUST_WORD, SCE_RUST_MACRO });
        }
        else if (lexer_name_ == "json")
        {
            style({ SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT }, palette.comment);
            style({ SCE_JSON_KEYWORD, SCE_JSON_LDKEYWORD }, palette.keyword);
            style({ SCE_JSON_STRING, SCE_JSON_STRINGEOL }, palette.string);
            style({ SCE_JSON_PROPERTYNAME }, palette.attribute);
            style({ SCE_JSON_NUMBER }, palette.number);
            style({ SCE_JSON_ESCAPESEQUENCE }, palette.escape);
            style({ SCE_JSON_URI, SCE_JSON_COMPACTIRI }, palette.function);
            style({ SCE_JSON_ERROR }, palette.error);
        }
        else if (lexer_name_ == "yaml")
        {
            style({ SCE_YAML_COMMENT }, palette.comment);
            style({ SCE_YAML_KEYWORD }, palette.keyword);
            style({ SCE_YAML_IDENTIFIER, SCE_YAML_DOCUMENT }, palette.attribute);
            style({ SCE_YAML_TEXT, SCE_YAML_REFERENCE }, palette.string);
            style({ SCE_YAML_NUMBER }, palette.number);
            style({ SCE_YAML_ERROR }, palette.error);
        }
        else if (lexer_name_ == "toml")
        {
            style({ SCE_TOML_COMMENT }, palette.comment);
            style({ SCE_TOML_KEYWORD }, palette.keyword);
            style({ SCE_TOML_TABLE }, palette.type);
            style({ SCE_TOML_KEY, SCE_TOML_IDENTIFIER }, palette.attribute);
            style({ SCE_TOML_STRING_SQ, SCE_TOML_STRING_DQ,
                    SCE_TOML_TRIPLE_STRING_SQ, SCE_TOML_TRIPLE_STRING_DQ,
                    SCE_TOML_STRINGEOL }, palette.string);
            style({ SCE_TOML_NUMBER, SCE_TOML_DATETIME }, palette.number);
            style({ SCE_TOML_ESCAPECHAR }, palette.escape);
            style({ SCE_TOML_ERROR }, palette.error);
        }
        else if (lexer_name_ == "hypertext" || lexer_name_ == "xml")
        {
            style({ SCE_H_COMMENT, SCE_H_XCCOMMENT, SCE_H_SGML_COMMENT,
                    SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE, SCE_HJ_COMMENTDOC,
                    SCE_HJA_COMMENT, SCE_HJA_COMMENTLINE, SCE_HJA_COMMENTDOC,
                    SCE_HP_COMMENTLINE, SCE_HPHP_COMMENT, SCE_HPHP_COMMENTLINE }, palette.comment);
            style({ SCE_H_TAG, SCE_H_TAGEND, SCE_H_XMLSTART, SCE_H_XMLEND,
                    SCE_H_SCRIPT, SCE_H_ASP, SCE_H_ASPAT }, palette.tag);
            style({ SCE_H_ATTRIBUTE, SCE_H_VALUE }, palette.attribute);
            style({ SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_CDATA,
                    SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING, SCE_HJ_REGEX,
                    SCE_HJ_TEMPLATELITERAL, SCE_HJA_DOUBLESTRING,
                    SCE_HJA_SINGLESTRING, SCE_HJA_REGEX, SCE_HJA_TEMPLATELITERAL,
                    SCE_HP_STRING, SCE_HP_CHARACTER, SCE_HP_TRIPLE,
                    SCE_HP_TRIPLEDOUBLE, SCE_HPHP_HSTRING,
                    SCE_HPHP_SIMPLESTRING }, palette.string);
            style({ SCE_HJ_WORD, SCE_HJ_KEYWORD, SCE_HJA_WORD, SCE_HJA_KEYWORD,
                    SCE_HP_WORD, SCE_HPHP_WORD }, palette.keyword);
            style({ SCE_H_NUMBER, SCE_HJ_NUMBER, SCE_HJA_NUMBER,
                    SCE_HP_NUMBER, SCE_HPHP_NUMBER }, palette.number);
            style({ SCE_H_ENTITY, SCE_H_SGML_ENTITY }, palette.escape);
            style({ SCE_H_TAGUNKNOWN, SCE_H_ATTRIBUTEUNKNOWN,
                    SCE_H_SGML_ERROR }, palette.error);
            bold({ SCE_H_TAG, SCE_H_TAGEND, SCE_HJ_WORD, SCE_HJA_WORD,
                   SCE_HP_WORD, SCE_HPHP_WORD });
        }
        else if (lexer_name_ == "css")
        {
            style({ SCE_CSS_COMMENT }, palette.comment);
            style({ SCE_CSS_TAG, SCE_CSS_CLASS, SCE_CSS_ID }, palette.tag);
            style({ SCE_CSS_ATTRIBUTE, SCE_CSS_IDENTIFIER, SCE_CSS_IDENTIFIER2,
                    SCE_CSS_IDENTIFIER3, SCE_CSS_EXTENDED_IDENTIFIER,
                    SCE_CSS_VARIABLE }, palette.attribute);
            style({ SCE_CSS_PSEUDOCLASS, SCE_CSS_PSEUDOELEMENT,
                    SCE_CSS_EXTENDED_PSEUDOCLASS,
                    SCE_CSS_EXTENDED_PSEUDOELEMENT }, palette.function);
            style({ SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING }, palette.string);
            style({ SCE_CSS_VALUE }, palette.number);
            style({ SCE_CSS_DIRECTIVE, SCE_CSS_IMPORTANT,
                    SCE_CSS_GROUP_RULE }, palette.preprocessor);
            style({ SCE_CSS_UNKNOWN_IDENTIFIER,
                    SCE_CSS_UNKNOWN_PSEUDOCLASS }, palette.error);
        }
        else if (lexer_name_ == "sql")
        {
            style({ SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC,
                    SCE_SQL_SQLPLUS_COMMENT, SCE_SQL_COMMENTLINEDOC }, palette.comment);
            style({ SCE_SQL_WORD, SCE_SQL_WORD2 }, palette.keyword);
            style({ SCE_SQL_STRING, SCE_SQL_CHARACTER,
                    SCE_SQL_QUOTEDIDENTIFIER }, palette.string);
            style({ SCE_SQL_NUMBER }, palette.number);
            style({ SCE_SQL_SQLPLUS, SCE_SQL_SQLPLUS_PROMPT }, palette.preprocessor);
            style({ SCE_SQL_COMMENTDOCKEYWORDERROR }, palette.error);
            bold({ SCE_SQL_WORD });
        }
        else if (lexer_name_ == "bash")
        {
            style({ SCE_SH_COMMENTLINE }, palette.comment);
            style({ SCE_SH_WORD }, palette.keyword);
            style({ SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_BACKTICKS,
                    SCE_SH_HERE_DELIM, SCE_SH_HERE_Q }, palette.string);
            style({ SCE_SH_NUMBER }, palette.number);
            style({ SCE_SH_SCALAR, SCE_SH_PARAM }, palette.attribute);
            style({ SCE_SH_ERROR }, palette.error);
            bold({ SCE_SH_WORD });
        }
        else if (lexer_name_ == "powershell")
        {
            style({ SCE_POWERSHELL_COMMENT, SCE_POWERSHELL_COMMENTSTREAM,
                    SCE_POWERSHELL_COMMENTDOCKEYWORD }, palette.comment);
            style({ SCE_POWERSHELL_KEYWORD }, palette.keyword);
            style({ SCE_POWERSHELL_CMDLET, SCE_POWERSHELL_ALIAS,
                    SCE_POWERSHELL_FUNCTION }, palette.function);
            style({ SCE_POWERSHELL_STRING, SCE_POWERSHELL_CHARACTER,
                    SCE_POWERSHELL_HERE_STRING,
                    SCE_POWERSHELL_HERE_CHARACTER }, palette.string);
            style({ SCE_POWERSHELL_NUMBER }, palette.number);
            style({ SCE_POWERSHELL_VARIABLE }, palette.attribute);
            bold({ SCE_POWERSHELL_KEYWORD, SCE_POWERSHELL_CMDLET });
        }
        else if (lexer_name_ == "batch")
        {
            style({ SCE_BAT_COMMENT }, palette.comment);
            style({ SCE_BAT_WORD, SCE_BAT_COMMAND }, palette.keyword);
            style({ SCE_BAT_LABEL, SCE_BAT_AFTER_LABEL }, palette.function);
            style({ SCE_BAT_IDENTIFIER }, palette.attribute);
            bold({ SCE_BAT_WORD, SCE_BAT_COMMAND });
        }
        else if (lexer_name_ == "cmake")
        {
            style({ SCE_CMAKE_COMMENT }, palette.comment);
            style({ SCE_CMAKE_COMMANDS, SCE_CMAKE_USERDEFINED }, palette.keyword);
            style({ SCE_CMAKE_PARAMETERS, SCE_CMAKE_VARIABLE,
                    SCE_CMAKE_STRINGVAR }, palette.attribute);
            style({ SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGLQ,
                    SCE_CMAKE_STRINGRQ }, palette.string);
            style({ SCE_CMAKE_NUMBER }, palette.number);
            style({ SCE_CMAKE_MACRODEF }, palette.function);
            bold({ SCE_CMAKE_COMMANDS, SCE_CMAKE_MACRODEF });
        }
        else if (lexer_name_ == "props")
        {
            style({ SCE_PROPS_COMMENT }, palette.comment);
            style({ SCE_PROPS_SECTION }, palette.type);
            style({ SCE_PROPS_KEY, SCE_PROPS_ASSIGNMENT }, palette.attribute);
            style({ SCE_PROPS_DEFVAL }, palette.string);
            bold({ SCE_PROPS_SECTION });
        }
        else if (lexer_name_ == "markdown")
        {
            style({ SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
                    SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5,
                    SCE_MARKDOWN_HEADER6 }, palette.keyword);
            style({ SCE_MARKDOWN_STRONG1, SCE_MARKDOWN_STRONG2 }, palette.type);
            style({ SCE_MARKDOWN_EM1, SCE_MARKDOWN_EM2 }, palette.function);
            style({ SCE_MARKDOWN_LINK }, palette.attribute);
            style({ SCE_MARKDOWN_CODE, SCE_MARKDOWN_CODE2,
                    SCE_MARKDOWN_CODEBK }, palette.string);
            style({ SCE_MARKDOWN_BLOCKQUOTE }, palette.comment);
            bold({ SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2,
                   SCE_MARKDOWN_HEADER3, SCE_MARKDOWN_STRONG1,
                   SCE_MARKDOWN_STRONG2 });
        }
        else if (lexer_name_ == "diff")
        {
            style({ SCE_DIFF_COMMENT }, palette.comment);
            style({ SCE_DIFF_COMMAND, SCE_DIFF_HEADER,
                    SCE_DIFF_POSITION }, palette.preprocessor);
            style({ SCE_DIFF_ADDED, SCE_DIFF_PATCH_ADD,
                    SCE_DIFF_REMOVED_PATCH_ADD }, palette.string);
            style({ SCE_DIFF_DELETED, SCE_DIFF_PATCH_DELETE,
                    SCE_DIFF_REMOVED_PATCH_DELETE }, palette.error);
            style({ SCE_DIFF_CHANGED }, palette.number);
        }
    }

    void ScintillaTextView::update_line_number_width() noexcept
    {
        if (editor_ == nullptr || !preferences_.line_numbers)
        {
            return;
        }
        const auto line_count = std::max<LRESULT>(1, call(SCI_GETLINECOUNT));
        std::size_t digits = 1;
        for (auto value = line_count; value >= 10; value /= 10)
        {
            ++digits;
        }
        const std::string sample(std::max<std::size_t>(3, digits), '9');
        const auto width = call(
            SCI_TEXTWIDTH,
            STYLE_LINENUMBER,
            reinterpret_cast<LPARAM>(sample.c_str()));
        call(SCI_SETMARGINWIDTHN, 0, std::max<LRESULT>(36, width + 16));
    }

    void ScintillaTextView::handle_notification(const NMHDR& header) noexcept
    {
        if (header.hwndFrom != editor_ || header.code != SCN_UPDATEUI)
        {
            return;
        }
        if (near_end_callback_ && should_load_more())
        {
            near_end_callback_();
        }
    }
}
