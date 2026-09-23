#include "pch.h"
#include "preview_provider.h"
#include "scintilla_text_view.h"
#include "component_loader.h"
#include "localization.h"
#include "third_party/scintilla/include/Scintilla.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

// Only application services unrelated to text decoding are substituted.
namespace glance::app
{
    std::wstring localize(std::wstring_view key) { return std::wstring(key); }
    bool component_has_extension(std::wstring_view) noexcept { return false; }
    contracts::components::GalleryMediaKind component_gallery_media_kind(std::wstring_view) noexcept
    {
        return contracts::components::GalleryMediaKind::none;
    }
    std::vector<std::wstring> component_gallery_extensions(contracts::components::GalleryMediaKind) noexcept
    {
        return {};
    }
}

namespace
{
    using namespace glance::app;
    constexpr std::size_t chunk_size = 256 * 1024;
    void require(bool condition, const char* message)
    {
        if (!condition) { throw std::runtime_error(message); }
    }

    void write_file(const std::filesystem::path& path, std::string_view bytes, bool append = false)
    {
        winrt::handle file(CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(static_cast<bool>(file), "Open test log for writing");
        if (append)
        {
            LARGE_INTEGER zero{};
            require(SetFilePointerEx(file.get(), zero, nullptr, FILE_END) != FALSE, "Seek test log");
        }
        DWORD written{};
        require(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size(), "Write test log");
        static std::atomic<std::uint64_t> sequence{ 133000000000000000ULL };
        const auto tick = sequence.fetch_add(10000);
        const FILETIME time{ static_cast<DWORD>(tick), static_cast<DWORD>(tick >> 32) };
        require(SetFileTime(file.get(), nullptr, nullptr, &time) != FALSE, "Stamp test log");
    }

    struct Preview
    {
        std::shared_ptr<IncrementalTextReader> reader;
        std::wstring text;
        std::uint64_t bytes{};
        bool apply(TextPreview update)
        {
            require(update.error.empty(), "Text decoding succeeded");
            require(update.bytes_read <= chunk_size + 32768, "Bounded read size");
            bytes += update.bytes_read;
            if (update.retry_later) { return false; }
            if (update.replace_content) { text.clear(); }
            text += update.content;
            reader = std::move(update.reader);
            return update.has_more;
        }
        void poll()
        {
            for (int count = 0; count < 1000; ++count)
            {
                if (!apply(load_next_text_preview_chunk(reader, chunk_size))) { return; }
            }
            require(false, "Finite backlog drains");
        }
    };

    void pump()
    {
        MSG message{};
        const auto deadline = GetTickCount64() + 100;
        while (GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void test_scintilla()
    {
        const HighlightRule sample_rules[] = {
            { HighlightMatch::word, "READY", HighlightStyle::string, true },
            { HighlightMatch::digit_pattern, "v##.##", HighlightStyle::number },
            { HighlightMatch::identifier_before, ":", HighlightStyle::attribute },
        };
        const HighlightRuleSet sample{ {}, sample_rules };
        const std::string sample_text = "ready v12.34 name: READY_more";
        std::vector<char> sample_styles(sample_text.size());
        highlight_text(sample, sample_text, 0, sample_styles);
        require(sample_styles[0] == static_cast<char>(HighlightStyle::string) &&
            sample_styles[6] == static_cast<char>(HighlightStyle::number) &&
            sample_styles[13] == static_cast<char>(HighlightStyle::attribute) && sample_styles[19] == 0,
            "Independent declarative rule set uses the shared engine");
        const HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Glance text regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(owner != nullptr, "Create regression window");
        {
            ScintillaTextView view(owner, [] {}, [](int) {}, [] { return false; });
            require(view.available(), "Load actual Scintilla runtime");
            view.set_bounds(0, 0, 620, 440);
            ShowWindow(owner, SW_SHOWNOACTIVATE);
            view.set_visible(true);
            view.set_word_wrap(false);
            struct Search { HWND owner; HWND editor{}; } search{ owner };
            EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
                auto& state = *reinterpret_cast<Search*>(data);
                if (GetWindow(window, GW_OWNER) == state.owner)
                {
                    state.editor = FindWindowExW(window, nullptr, L"Scintilla", nullptr);
                    if (state.editor) { return FALSE; }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            require(search.editor != nullptr, "Find own Scintilla editor");
            const auto call = [&](UINT message, WPARAM w = 0, LPARAM l = 0) {
                return SendMessageW(search.editor, message, w, l);
            };
            std::wstring text;
            for (int i = 0; i < 2000; ++i) { text += L"log line " + std::to_wstring(i) + L"\n"; }
            view.append_text(text);
            pump();
            call(SCI_SETSEL, 23, 42);
            call(SCI_SETFIRSTVISIBLELINE, 300);
            const auto position = call(SCI_GETFIRSTVISIBLELINE);
            const auto document = call(SCI_GETDOCPOINTER);
            for (int i = 0; i < 100; ++i)
            {
                view.refresh_text(L"appended log\n", false, false, false);
            }
            pump();
            require(call(SCI_GETFIRSTVISIBLELINE) == position, "Append preserves viewing position");
            require(call(SCI_GETANCHOR) == 23 && call(SCI_GETCURRENTPOS) == 42, "Append preserves selection");
            require(call(SCI_GETDOCPOINTER) == document, "Append retains the native document");
            view.refresh_text(text, true, false, false);
            pump();
            require(call(SCI_GETFIRSTVISIBLELINE) == position, "Replacement preserves viewing position");
            require(call(SCI_GETANCHOR) == 23 && call(SCI_GETCURRENTPOS) == 42, "Replacement preserves selection");
            call(SCI_SETFIRSTVISIBLELINE, std::numeric_limits<int>::max());
            const auto anchor_before_follow = call(SCI_GETANCHOR);
            view.refresh_text(L"new tail\n", false, true, false);
            pump();
            const auto at_bottom = [&] {
                const auto last = call(SCI_GETLINECOUNT) - 1;
                RECT client{};
                GetClientRect(search.editor, &client);
                const auto end_y = call(SCI_POINTYFROMPOSITION, 0, call(SCI_GETLENGTH));
                return call(SCI_GETFIRSTVISIBLELINE) + call(SCI_LINESONSCREEN) >=
                    call(SCI_VISIBLEFROMDOCLINE, last) + call(SCI_WRAPCOUNT, last) &&
                    end_y >= 0 && end_y + call(SCI_TEXTHEIGHT, last) <= client.bottom;
            };
            require(at_bottom(), "Follow reaches the actual scrollbar bottom");
            require(call(SCI_GETANCHOR) == anchor_before_follow, "Follow preserves selection");
            call(WM_VSCROLL, SB_TOP);
            view.refresh_text(L"read without following\n", false, true, false);
            pump();
            require(call(SCI_GETFIRSTVISIBLELINE) == 0, "Manual upward scrolling disengages follow");
            call(WM_VSCROLL, SB_BOTTOM);
            view.refresh_text(L"resume following\n", false, true, false);
            pump();
            require(at_bottom(), "Returning to bottom resumes follow");
            view.refresh_text(L"", true, false, false);
            require(call(SCI_GETLENGTH) == 0, "Truncation clears the existing document");
            require(call(SCI_GETREADONLY) != 0, "Refresh keeps the document read-only");
            view.set_word_wrap(true);
            const std::wstring long_line(400, L'x');
            std::wstring wrapped;
            for (int i = 0; i < 800; ++i) { wrapped += long_line + L"\n"; }
            view.refresh_text(wrapped, true, false, false);
            pump();
            call(SCI_SCROLLVERTICAL, 150, 1);
            pump();
            const auto wrapped_first = call(SCI_GETFIRSTVISIBLELINE);
            const auto wrapped_document_line = call(SCI_DOCLINEFROMVISIBLE, wrapped_first);
            view.refresh_text(long_line, false, false, false);
            pump();
            require(call(SCI_DOCLINEFROMVISIBLE, call(SCI_GETFIRSTVISIBLELINE)) == wrapped_document_line,
                "Wrapped append preserves the first document line");
            view.refresh_text(wrapped.substr(0, 401), true, false, true);
            view.refresh_text(wrapped.substr(401), false, false, false);
            pump();
            require(call(SCI_DOCLINEFROMVISIBLE, call(SCI_GETFIRSTVISIBLELINE)) == wrapped_document_line,
                "Multi-chunk replacement restores a wrapped viewing position");
            call(WM_VSCROLL, SB_BOTTOM);
            view.refresh_text(long_line, false, true, false);
            for (int frame = 0; frame < 10; ++frame) { pump(); }
            require(at_bottom(), "Wrapped append remains at the actual bottom after layout");
            const auto before_rewrite = call(SCI_GETFIRSTVISIBLELINE);
            view.refresh_text(wrapped.substr(0, 401), true, true, true);
            require(call(SCI_GETFIRSTVISIBLELINE) == before_rewrite,
                "First replacement chunk retains the existing viewport");
            view.refresh_text(wrapped.substr(401), false, true, false);
            for (int frame = 0; frame < 10; ++frame) { pump(); }
            require(at_bottom(), "Replacement completes at the actual bottom after layout");
            view.set_file_path(L"example.LOG");
            view.refresh_text(L"2026-09-14 12:34:56.123 [INFO] key=value DEBUG warning ERROR informationX\n", true, false, false);
            call(SCI_COLOURISE, 0, -1);
            require(call(SCI_GETSTYLEAT, 0) == 1 && call(SCI_GETSTYLEAT, 11) == 1, "Log timestamps");
            require(call(SCI_GETSTYLEAT, 25) == 3 && call(SCI_GETSTYLEAT, 31) == 6, "Log level and field key");
            require(call(SCI_GETSTYLEAT, 41) == 2 && call(SCI_GETSTYLEAT, 47) == 4 &&
                call(SCI_GETSTYLEAT, 55) == 5 && call(SCI_GETSTYLEAT, 61) == 0, "Log levels and word boundaries");
            view.refresh_text(L"[ER", true, false, false);
            call(SCI_COLOURISE, 0, -1);
            view.refresh_text(L"ROR] 中文\n", false, false, false);
            call(SCI_COLOURISE, 0, -1);
            require(call(SCI_GETSTYLEAT, 1) == 5 && call(SCI_GETSTYLEAT, 8) == 0, "Split level and UTF-8 text");
            view.set_syntax_highlighting(false);
            require(call(SCI_GETSTYLEAT, 1) == 0, "Disable log highlighting");
            view.set_syntax_highlighting(true);
            pump();
            require(call(SCI_GETSTYLEAT, 1) == 5, "Re-enable log highlighting");
            TextPreferences themed;
            view.set_preferences(themed, true, true);
            call(SCI_COLOURISE, 0, -1);
            const auto dark_color = call(SCI_STYLEGETFORE, 5);
            view.set_preferences(themed, true, false);
            call(SCI_COLOURISE, 0, -1);
            require(call(SCI_GETSTYLEAT, 1) == 5 && call(SCI_STYLEGETFORE, 5) != dark_color,
                "Theme change preserves tokens and updates colors");
            view.refresh_text(L"2026-09-14T12:34:56.123Z INFO", true, false, false);
            call(SCI_COLOURISE, 0, -1);
            require(call(SCI_GETSTYLEAT, 11) == 1 && call(SCI_GETSTYLEAT, 22) == 1,
                "ISO timestamp with fractional seconds");
            std::wstring boundary(32765, L' ');
            boundary += L"ERROR ";
            boundary += std::wstring(256 * 1024, L'x');
            boundary += L" WARN\n";
            view.refresh_text(boundary, true, false, false);
            call(SCI_COLOURISE, 0, -1);
            require(call(SCI_GETSTYLEAT, 32765) == 5 && call(SCI_GETSTYLEAT, 32769) == 5,
                "Level spanning styling blocks");
            require(call(SCI_GETSTYLEAT, boundary.size() - 3) == 4, "Long line retains trailing level");
            view.set_file_path(L"example.txt");
            require(call(SCI_GETSTYLEAT, 32765) == 0, "Switch to ordinary text clears log styles");
            const std::array<UndecodableByte, 1> invalid{ UndecodableByte{ 1, 0xFF } };
            view.refresh_text(L"A\uFFFD中文 FF", true, false, false, invalid);
            require(call(SCI_INDICATORVALUEAT, 20, 1) == 256 &&
                call(SCI_INDICATORVALUEAT, 20, 2) == 256 &&
                call(SCI_INDICATORVALUEAT, 20, 3) == 0, "Invalid byte has an isolated numeric box");
            require(call(SCI_INDICGETSTYLE, 20) == INDIC_ROUNDBOX &&
                call(SCI_INDICGETSTYLE, 21) == INDIC_TEXTFORE, "Numeric box and foreground styling are independent");
            std::string copied;
            SetWindowSubclass(search.editor, [](HWND window, UINT message, WPARAM w, LPARAM l,
                UINT_PTR, DWORD_PTR data) -> LRESULT {
                if (message == SCI_COPYTEXT)
                {
                    *reinterpret_cast<std::string*>(data) =
                        std::string(reinterpret_cast<const char*>(l), static_cast<std::size_t>(w));
                    return 0;
                }
                return DefSubclassProc(window, message, w, l);
            }, 100, reinterpret_cast<DWORD_PTR>(&copied));
            call(SCI_SETSEL, 0, call(SCI_GETLENGTH));
            call(SCI_COPY);
            require(copied == "A\xEF\xBF\xBD中文 FF", "Copy preserves decoded text without injecting display hex digits");
            view.set_syntax_highlighting(false);
            require(call(SCI_INDICATORVALUEAT, 20, 1) == 256, "Invalid byte markers survive highlighting changes");
            view.refresh_text(L"AFF中文 FF", true, false, false);
            require(call(SCI_INDICATORVALUEAT, 20, 1) == 0, "Identical display text clears obsolete invalid-byte metadata");
        }
        DestroyWindow(owner);
        pump();
        std::cout << "Scintilla: append, replacement, selection, viewport, follow, empty file passed\n";
    }
}

int run_text_monitor_tests()
{
    const auto directory = std::filesystem::current_path() / L".tmp" /
        (L"text-monitor-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    const auto path = directory / L"live.log";
    const auto replacement = directory / L"replacement.log";
    try
    {
        write_file(path, "start\n");
        Preview preview;
        preview.apply(load_text_preview(path, chunk_size, TextEncoding::utf8, true));
        require(preview.text == L"start\n", "Initial preview");
        const auto initial_bytes = preview.bytes;
        for (int i = 0; i < 1000; ++i) { preview.poll(); }
        require(preview.bytes == initial_bytes, "Unchanged polls perform zero content reads");
        for (int i = 0; i < 1000; ++i)
        {
            const auto noise = directory / (L"unrelated-" + std::to_wstring(i) + L".log");
            write_file(noise, "unrelated log\n");
            preview.poll();
            require(DeleteFileW(noise.c_str()) != FALSE, "Delete unrelated test log");
            preview.poll();
        }
        require(preview.bytes == initial_bytes, "Unrelated file creation/deletion triggers zero content reads");
        write_file(path, "\xe4", true);
        preview.poll();
        require(preview.text == L"start\n", "Hold incomplete UTF-8 at EOF");
        write_file(path, "\xb8\xad\xf0\x9f", true);
        preview.poll();
        require(preview.text == L"start\n中", "Finish split UTF-8 and retain next partial character");
        write_file(path, "\x98\x80\n", true);
        preview.poll();
        require(preview.text == L"start\n中😀\n", "Finish split supplementary character");

        std::string burst;
        for (int i = 0; i < 150000; ++i) { burst += "burst log line " + std::to_string(i) + "\n"; }
        const auto before_burst = preview.bytes;
        const auto before_burst_characters = preview.text.size();
        write_file(path, burst, true);
        preview.poll();
        require(preview.text.size() == before_burst_characters + burst.size(), "Multi-chunk burst has no duplicates or omissions");
        require(preview.text.substr(before_burst_characters) == std::wstring(burst.begin(), burst.end()),
            "Every burst line is in source order");
        require(preview.bytes - before_burst <= burst.size() + 8192, "Append reads only new data and bounded guards");
        write_file(path, "");
        preview.poll();
        require(preview.text.empty(), "Truncate to empty");
        write_file(path, "regrown\n", true);
        preview.poll();
        require(preview.text == L"regrown\n", "Append after empty truncation");
        write_file(path, "changed\n");
        preview.poll();
        require(preview.text == L"changed\n", "Same-size rewrite");
        write_file(path, "truncate and regrow beyond old size\n");
        preview.poll();
        require(preview.text == L"truncate and regrow beyond old size\n", "Truncate and regrow between polls");

        require(DeleteFileW(path.c_str()) != FALSE, "No retained source handle prevents deletion");
        const auto retained = preview.text;
        for (int i = 0; i < 100; ++i) { preview.poll(); }
        require(preview.text == retained, "Missing file preserves last available content");
        write_file(path, "recreated\n");
        preview.poll();
        require(preview.text == L"recreated\n", "Recreated file resumes monitoring");
        for (int i = 0; i < 1000; ++i)
        {
            const auto line = "replacement " + std::to_string(i) + "\n";
            write_file(replacement, line);
            DWORD rotation_error{};
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                if (MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                { rotation_error = 0; break; }
                rotation_error = GetLastError();
                if (rotation_error != ERROR_ACCESS_DENIED && rotation_error != ERROR_SHARING_VIOLATION) break;
                Sleep(2);
            }
            if (rotation_error) throw std::runtime_error("Atomic log rotation: " + std::to_string(rotation_error));
            preview.poll();
            require(preview.text == std::wstring(line.begin(), line.end()), "Rotation follows path identity");
        }
        DWORD handles_before{};
        GetProcessHandleCount(GetCurrentProcess(), &handles_before);
        std::atomic_bool done{};
        std::atomic_bool writer_failed{};
        std::jthread writer([&] {
            try
            {
                for (int i = 0; i < 2000; ++i)
                {
                    DeleteFileW(path.c_str());
                    write_file(path, "rotating burst " + std::to_string(i) + "\n");
                    write_file(path, "tail\n", true);
                }
            }
            catch (...) { writer_failed = true; }
            done = true;
        });
        std::size_t polls{};
        while (!done)
        {
            auto update = load_next_text_preview_chunk(preview.reader, chunk_size);
            if (update.error.empty()) { preview.apply(std::move(update)); }
            ++polls;
        }
        writer.join();
        require(!writer_failed, "Concurrent writer completed");
        write_file(path, "final stable log after churn\n");
        preview.poll();
        require(preview.text == L"final stable log after churn\n", "Converges after concurrent delete/create/append burst");
        {
            winrt::handle locked(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(static_cast<bool>(locked), "Writer can acquire exclusive access between polls");
            const auto retained_while_locked = preview.text;
            preview.poll();
            require(preview.text == retained_while_locked, "Sharing violation retains visible content");
        }
        preview.poll();
        require(preview.text == L"final stable log after churn\n", "Recover after sharing violation");
        {
            winrt::handle writer_open(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(static_cast<bool>(writer_open), "Open persistent log writer");
            DWORD written{};
            require(WriteFile(writer_open.get(), "open writer\n", 12, &written, nullptr) && written == 12,
                "Append without closing writer");
            preview.poll();
            require(preview.text.ends_with(L"open writer\n"), "Detect growth while writer keeps its handle open");
        }
        DWORD handles_after{};
        GetProcessHandleCount(GetCurrentProcess(), &handles_after);
        require(handles_after <= handles_before + 4, "No handle leak during file churn");
        cancel_text_preview_read(preview.reader);
        const auto cancelled = load_next_text_preview_chunk(preview.reader, chunk_size);
        require(cancelled.content.empty() && cancelled.bytes_read == 0, "Cancelled preview performs no file I/O");

        write_file(path, std::string("\xff\xfe\x41\x00", 4));
        Preview utf16;
        utf16.apply(load_text_preview(path, chunk_size, TextEncoding::automatic, true));
        write_file(path, std::string("\x2d", 1), true);
        utf16.poll();
        require(utf16.text == L"A", "Hold incomplete UTF-16 code unit");
        write_file(path, std::string("\x4e", 1), true);
        utf16.poll();
        require(utf16.text == L"A中", "Finish split UTF-16 code unit");
        write_file(path, std::string("\x3d\xd8", 2), true);
        utf16.poll();
        require(utf16.text == L"A中", "Hold incomplete UTF-16 surrogate pair");
        write_file(path, std::string("\x00\xde", 2), true);
        utf16.poll();
        require(utf16.text == L"A中😀", "Finish split UTF-16 surrogate pair");
        write_file(path, "");
        Preview empty;
        empty.apply(load_text_preview(path, chunk_size, TextEncoding::automatic, true));
        write_file(path, std::string("\xff\xfe\x41\x00", 4), true);
        empty.poll();
        require(empty.text == L"A", "Detect encoding when an initially empty log receives content");
        write_file(path, "GBK:");
        Preview gbk;
        gbk.apply(load_text_preview(path, chunk_size, TextEncoding::gbk, true));
        write_file(path, "\xd6", true);
        gbk.poll();
        require(gbk.text == L"GBK:", "Hold incomplete GBK character");
        write_file(path, "\xd0", true);
        gbk.poll();
        require(gbk.text == L"GBK:中", "Finish split GBK character");
        std::cout << "Reader: idle_reads=0, burst_bytes=" << burst.size()
            << ", rotations=1000, concurrent_cycles=2000, concurrent_polls=" << polls << '\n';
        write_file(path, std::string("A\xFF") + "中文" + std::string("\0Z", 2));
        auto damaged = load_text_preview(path, chunk_size, TextEncoding::utf8);
        require(damaged.error.empty() && damaged.content == std::wstring(L"A\uFFFD中文\0Z", 6) &&
            damaged.undecodable_bytes.size() == 1 && damaged.undecodable_bytes[0].offset == 1 &&
            damaged.undecodable_bytes[0].value == 255, "Recover invalid UTF-8 and preserve surrounding text and NUL");
        std::string mostly_utf8;
        for (int i = 0; i < 100; ++i) { mostly_utf8 += "正常 UTF-8 text\n"; }
        write_file(path, mostly_utf8 + "\xFF" + "正常 tail");
        auto automatic = load_text_preview(path);
        require(automatic.error.empty() && automatic.encoding == L"UTF-8" &&
            automatic.undecodable_bytes.size() == 1 && automatic.content.ends_with(L"正常 tail"),
            "Automatic encoding tolerates isolated corrupt UTF-8");
        write_file(path, std::string("\xff\xfe\x00\xd8\x58\x00", 6));
        auto broken_utf16 = load_text_preview(path);
        require(broken_utf16.error.empty() && broken_utf16.content == L"\uFFFD\uFFFDX" &&
            broken_utf16.undecodable_bytes.size() == 2 && broken_utf16.undecodable_bytes[1].value == 0xD8,
            "UTF-16 recovery keeps each original invalid byte");
        write_file(path, "GBK:\xff tail");
        auto broken_gbk = load_text_preview(path, chunk_size, TextEncoding::gbk);
        require(broken_gbk.error.empty() && broken_gbk.undecodable_bytes.empty() &&
            broken_gbk.content == L"GBK:\uF8F5 tail", "Valid GBK private-use mapping is preserved");
        write_file(path, "GBK:\x81 tail");
        broken_gbk = load_text_preview(path, chunk_size, TextEncoding::gbk);
        require(broken_gbk.error.empty() && broken_gbk.undecodable_bytes.size() == 1 &&
            broken_gbk.content.ends_with(L" tail"), "Recover invalid GBK");
        write_file(path, std::string(1000, '\xff'));
        require(!load_text_preview(path, chunk_size, TextEncoding::utf8).error.empty(),
            "Reject predominantly undecodable data");
        write_file(path, "valid \xef\xbf\xbd text");
        auto valid_replacement = load_text_preview(path, chunk_size, TextEncoding::utf8);
        require(valid_replacement.error.empty() && valid_replacement.undecodable_bytes.empty(),
            "Literal replacement character is not an invalid-byte marker");
        write_file(path, std::string(chunk_size - 1, 'a') + "\xe4");
        auto split_bad = load_text_preview(path, chunk_size, TextEncoding::utf8, true);
        require(split_bad.undecodable_bytes.empty(), "Incomplete monitored UTF-8 waits for the next write");
        write_file(path, "X tail", true);
        auto completed_bad = load_next_text_preview_chunk(split_bad.reader, chunk_size);
        require(completed_bad.error.empty() && completed_bad.content == L"\uFFFDX tail" &&
            completed_bad.undecodable_bytes.size() == 1 && completed_bad.undecodable_bytes[0].value == 0xE4,
            "Invalid split sequence reports buffered bytes and resumes decoding");
        std::cout << "Damaged text: UTF-8, UTF-16, GBK, automatic encoding, NUL, density, split writes passed\n";
        test_scintilla();
        std::filesystem::remove(path);
        std::filesystem::remove(directory);
        std::cout << "All text monitor tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\nFixtures retained at " << directory << '\n';
        return 1;
    }
}
