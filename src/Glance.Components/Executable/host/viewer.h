#pragma once
#include "pe_reader.h"
#include "resources.h"
#include "resource_image.h"
#include "glance/contracts/native_preview_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace glance::executable
{
    class Viewer final
    {
      public:
        Viewer();
        ~Viewer();
        bool open(HWND parent, const std::wstring& path, const RECT& bounds, unsigned dpi,
                  const contracts::native_preview::PreviewVisuals& visuals);
        void resize(const RECT& bounds, unsigned dpi);
        void visuals(const contracts::native_preview::PreviewVisuals& value);
        void language(const std::wstring& value);
        void close();
        bool key(MSG& message);
        HWND window() const
        {
            return window_;
        }

      private:
        static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;
        LRESULT message(UINT, WPARAM, LPARAM);
        void layout();
        void refresh();
        void select(unsigned section);
        void filter();
        void copy();
        void save_report();
        void save_resource();
        void request(unsigned task);
        void reload();
        struct Work
        {
            unsigned task{};
            Row resource;
            std::wstring destination;
            std::wstring payload;
            std::uint64_t generation{};
            std::uint64_t epoch{};
        };
        void enqueue(Work task);
        void work();
        void complete();
        void preview_resource();
        void paint();
        void overview_layout();
        void rebuild_master();
        int px(int value) const;
        std::wstring cell(const Row& row, std::size_t column) const;
        HWND window_{}, nav_{}, compact_{}, search_{}, list_{}, details_{}, copy_{}, report_{}, action_{},
            status_{}, master_{}, overview_{}, picture_{};
        ResourceImage resource_image_;
        struct FieldControl
        {
            HWND label{}, value{};
            std::size_t row{};
        };
        std::vector<FieldControl> fields_;
        std::vector<HWND> groups_;
        std::vector<std::wstring> masters_;
        std::wstring master_filter_;
        int overview_scroll_{}, overview_height_{};
        HFONT font_{}, heading_{}, section_font_{}, code_font_{};
        HBRUSH brush_{};
        HICON icon_{};
        COLORREF background_{RGB(250, 250, 250)}, foreground_{RGB(25, 25, 25)};
        unsigned dpi_{96};
        unsigned section_{};
        int sort_column_{-1};
        bool descending_{};
        bool initialized_{};
        Resources strings_;
        std::wstring path_;
        std::wstring language_{L"en-US"};
        std::optional<Summary> summary_;
        std::array<Table, static_cast<unsigned>(Section::count)> tables_;
        std::array<std::wstring, static_cast<unsigned>(Section::count)> searches_;
        std::array<int, static_cast<unsigned>(Section::count)> top_rows_{};
        std::array<int, static_cast<unsigned>(Section::count)> sort_columns_{-1, -1, -1, -1, -1, -1, -1};
        std::array<bool, static_cast<unsigned>(Section::count)> sort_directions_{};
        std::array<std::vector<std::size_t>, static_cast<unsigned>(Section::count)> selections_;
        bool updating_{};
        std::uint64_t resource_generation_{};
        std::uint64_t epoch_{};
        std::vector<std::size_t> filtered_;
        std::vector<unsigned> navigation_;
        std::wstring hash_;
        std::mutex mutex_;
        std::condition_variable condition_;
        std::deque<Work> tasks_;
        struct Completion
        {
            unsigned task{};
            std::optional<Summary> summary;
            Table table;
            std::wstring text;
            std::uint64_t generation{};
            ResourceImage image;
            std::uint64_t epoch{};
        };
        std::deque<Completion> completed_;
        std::thread worker_;
        std::atomic_bool stopping_{};
        std::atomic_bool cancel_operation_{};
        std::atomic_uint active_task_{999};
    };
} // namespace glance::executable
