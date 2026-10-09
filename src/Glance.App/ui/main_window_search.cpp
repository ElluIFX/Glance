#include "pch.h"
#include "MainWindow.xaml.h"
#include "localization.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <chrono>

namespace winrt::Glance::App::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;

    namespace
    {
        bool checked(Controls::Primitives::ToggleButton const& control)
        {
            const auto value = control.IsChecked();
            return value && value.Value();
        }

    }

    void MainWindow::initialize_text_search()
    {
        const auto weak = get_weak();
        text_search_timer_ = DispatcherTimer();
        text_search_timer_.Interval(std::chrono::milliseconds(180));
        text_search_timer_.Tick([weak](auto&&, auto&&) {
            if (const auto self = weak.get()) { self->text_search_timer_.Stop(); self->run_text_search_async(); }
        });
        TextSearchInput().TextChanged([weak](auto&&, auto&&) { if (auto self = weak.get()) self->schedule_text_search(); });
        for (auto control : {SearchCaseToggle(), SearchWordToggle(), SearchRegexToggle()})
            control.Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->schedule_text_search(); });
        JsonSearchScope().SelectedIndex(0);
        JsonSearchScope().SelectionChanged([weak](auto&&, auto&&) { if (auto self = weak.get()) self->schedule_text_search(); });
        SearchPreviousButton().Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->navigate_text_search(-1); });
        SearchNextButton().Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->navigate_text_search(1); });
        SearchCloseButton().Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->close_text_search(); });
        TextFindButton().Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->open_text_search(); });
        TextSearchPanel().SizeChanged([weak](auto&&, auto&&) { if (auto self = weak.get()) self->queue_native_surface_occlusion_update(); });
        update_text_search_labels();
    }

    void MainWindow::update_text_search_labels()
    {
        TextSearchInput().PlaceholderText(glance::app::localize(L"SearchPlaceholder"));
        const auto tooltip = [](auto const& control, const wchar_t* key) {
            const auto text = glance::app::localize(key);
            ToolTipService::SetToolTip(control, box_value(text));
            Automation::AutomationProperties::SetName(control, text);
        };
        tooltip(TextFindButton(), L"SearchOpen");
        tooltip(SearchCaseToggle(), L"SearchMatchCase");
        tooltip(SearchWordToggle(), L"SearchWholeWord");
        tooltip(SearchRegexToggle(), L"SearchRegex");
        tooltip(SearchPreviousButton(), L"SearchPrevious");
        tooltip(SearchNextButton(), L"SearchNext");
        tooltip(SearchCloseButton(), L"SearchClose");
        Automation::AutomationProperties::SetName(TextSearchInput(), glance::app::localize(L"SearchPlaceholder"));
        Automation::AutomationProperties::SetName(JsonSearchScope(), glance::app::localize(L"JsonSearchScope"));
        JsonSearchBoth().Content(box_value(glance::app::localize(L"JsonSearchBoth")));
        JsonSearchKeys().Content(box_value(glance::app::localize(L"JsonSearchKeys")));
        JsonSearchValues().Content(box_value(glance::app::localize(L"JsonSearchValues")));
        update_text_search_status();
    }

    bool MainWindow::handle_text_search_key(WPARAM key)
    {
        if (xaml_modal_overlay_active_ || password_prompt_target_ != PasswordPromptTarget::none) return false;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (TextSearchPanel().Visibility() != Visibility::Visible) return false;
        if (key == VK_ESCAPE) { close_text_search(); return true; }
        if (key == VK_F3 || key == VK_RETURN) { navigate_text_search(shift ? -1 : 1); return true; }
        if (GetKeyState(VK_MENU) & 0x8000)
        {
            Controls::Primitives::ToggleButton toggle{nullptr};
            if (key == 'C') toggle = SearchCaseToggle();
            if (key == 'W') toggle = SearchWordToggle();
            if (key == 'R') toggle = SearchRegexToggle();
            if (toggle) { toggle.IsChecked(!checked(toggle)); schedule_text_search(); return true; }
        }
        return false;
    }

    void MainWindow::open_text_search()
    {
        if (TextStatusControls().Visibility() != Visibility::Visible) return;
        if (markdown_preview_ && (current_text_markdown_ || current_text_web_)) set_markdown_preview_mode(false);
        TextSearchPanel().Visibility(Visibility::Visible);
        JsonSearchScope().Visibility(current_text_json_ && json_tree_mode_ ? Visibility::Visible : Visibility::Collapsed);
        update_input_activation();
        SetForegroundWindow(window_);
        TextSearchInput().Focus(FocusState::Programmatic);
        TextSearchInput().SelectAll();
        update_state();
        queue_native_surface_occlusion_update();
        schedule_text_search();
    }

    bool MainWindow::TryCloseSearch()
    {
        if (TextSearchPanel().Visibility() != Visibility::Visible) return false;
        close_text_search();
        return true;
    }

    void MainWindow::close_text_search()
    {
        ++text_search_request_;
        if (text_search_cancellation_) text_search_cancellation_->store(true);
        text_search_cancellation_.reset();
        if (text_search_timer_) text_search_timer_.Stop();
        TextSearchPanel().Visibility(Visibility::Collapsed);
        text_search_hits_.clear();
        text_search_json_fields_.clear();
        if (text_editor_) text_editor_->set_search_matches({});
        for (const auto node : search_expanded_json_nodes_) json_expanded_nodes_.erase(node);
        if (!search_expanded_json_nodes_.empty()) sync_json_projection(false);
        search_expanded_json_nodes_.clear();
        text_search_loading_ = false;
        text_search_error_ = glance::app::TextSearchError::none;
        text_search_query_validated_ = false;
        text_search_incomplete_ = false;
        update_input_activation();
        update_state();
        queue_native_surface_occlusion_update();
        refresh_realized_json_rows();
    }

    void MainWindow::schedule_text_search(bool content_changed)
    {
        if (TextSearchPanel().Visibility() != Visibility::Visible) return;
        if (!content_changed) text_search_query_validated_ = false;
        if (content_changed && text_search_query_validated_ && current_text_has_more_ &&
            !(current_text_json_ && json_tree_mode_) && !TextSearchInput().Text().empty())
        {
            update_text_search_status();
            load_next_text_chunk_async(content_generation_);
            return;
        }
        if (text_search_cancellation_) text_search_cancellation_->store(true);
        ++text_search_request_;
        text_search_error_ = glance::app::TextSearchError::none;
        if (!content_changed || !(current_text_json_ && json_tree_mode_ && text_search_query_validated_))
        {
            text_search_hits_.clear();
            text_search_index_ = 0;
            text_search_json_fields_.clear();
        }
        if (text_editor_) text_editor_->set_search_matches({});
        text_search_loading_ = !TextSearchInput().Text().empty();
        update_text_search_status();
        if (!content_changed || !text_search_timer_.IsEnabled())
        {
            text_search_timer_.Stop();
            text_search_timer_.Start();
        }
    }

    fire_and_forget MainWindow::run_text_search_async()
    {
        const auto weak = get_weak();
        const auto dispatcher = DispatcherQueue();
        const auto generation = content_generation_;
        const auto request = text_search_request_;
        const std::wstring query = TextSearchInput().Text().c_str();
        const bool tree = current_text_json_ && json_tree_mode_;
        const int scope = JsonSearchScope().SelectedIndex();
        const glance::app::TextSearchOptions options{checked(SearchCaseToggle()), checked(SearchWordToggle()), checked(SearchRegexToggle())};
        text_search_incomplete_ = tree ? !json_parse_complete_ || json_index_truncated_ : current_text_has_more_;
        if (query.empty()) { text_search_loading_ = false; update_text_search_status(); co_return; }
        if (!tree && text_loading_) co_return;
        const bool read_more = !tree && current_text_has_more_;
        auto cancellation = std::make_shared<std::atomic_bool>();
        text_search_cancellation_ = cancellation;
        glance::app::ScintillaTextView::SearchSnapshot snapshot;
        std::vector<glance::app::JsonNode> nodes;
        try
        {
            if (tree) nodes = json_nodes_;
            else if (text_editor_ && !read_more) snapshot = text_editor_->search_snapshot();
        }
        catch (...)
        {
            text_search_loading_ = false;
            text_search_error_ = glance::app::TextSearchError::failed;
            update_text_search_status();
            co_return;
        }
        co_await resume_background();
        std::vector<SearchHit> hits;
        auto error = glance::app::TextSearchError::none;
        try
        {
            if (!tree)
            {
                glance::app::TextSearchMatcher matcher(query, options);
                auto result = matcher.find_all(snapshot.text, *cancellation);
                error = result.error;
                for (const auto& match : result.matches)
                    hits.push_back({{snapshot.display_position(match.start), snapshot.display_position(match.end)}, glance::app::json_no_parent, false});
            }
            else
            {
                auto result = glance::app::search_json_nodes(nodes, query, options,
                    static_cast<glance::app::JsonSearchScope>(scope), *cancellation);
                error = result.error;
                for (const auto& match : result.matches) hits.push_back({match.match, match.node_id, match.key});
            }
        }
        catch (...) { error = glance::app::TextSearchError::failed; }
        try { dispatcher.TryEnqueue([weak, generation, request, tree, read_more, cancellation, hits = std::move(hits), error]() mutable {
            const auto self = weak.get();
            if (!self || generation != self->content_generation_ || request != self->text_search_request_ || cancellation->load()) return;
            self->text_search_error_ = error;
            if (error != glance::app::TextSearchError::none)
            {
                self->text_search_loading_ = false;
                self->update_text_search_status();
                return;
            }
            self->text_search_query_validated_ = true;
            if (read_more)
            {
                self->load_next_text_chunk_async(generation);
                return;
            }
            self->text_search_loading_ = false;
            const auto selected_hit = tree && self->text_search_index_ < self->text_search_hits_.size()
                ? std::optional<SearchHit>(self->text_search_hits_[self->text_search_index_]) : std::nullopt;
            self->text_search_hits_ = std::move(hits);
            self->text_search_index_ = 0;
            bool retained_selection = false;
            if (selected_hit)
            {
                const auto found = std::find_if(self->text_search_hits_.begin(), self->text_search_hits_.end(), [&](const auto& hit) {
                    return hit.node_id == selected_hit->node_id && hit.key == selected_hit->key &&
                        hit.match.start == selected_hit->match.start && hit.match.end == selected_hit->match.end;
                });
                if (found != self->text_search_hits_.end())
                {
                    self->text_search_index_ = static_cast<std::size_t>(found - self->text_search_hits_.begin());
                    retained_selection = true;
                }
            }
            if (tree)
            {
                self->text_search_json_fields_.clear();
                for (const auto& hit : self->text_search_hits_)
                    self->text_search_json_fields_[hit.node_id] |= hit.key ? 1 : 2;
                self->refresh_realized_json_rows();
            }
            else if (self->text_editor_)
            {
                std::vector<glance::app::TextSearchMatch> matches;
                matches.reserve(self->text_search_hits_.size());
                for (const auto& hit : self->text_search_hits_) matches.push_back(hit.match);
                self->text_editor_->set_search_matches(std::move(matches));
            }
            self->update_text_search_status();
            if (!self->text_search_hits_.empty() && !retained_selection) self->navigate_text_search(0);
        }); } catch (...) {}
    }

    void MainWindow::navigate_text_search(int direction)
    {
        if (text_search_hits_.empty()) return;
        if (direction > 0) text_search_index_ = (text_search_index_ + 1) % text_search_hits_.size();
        else if (direction < 0) text_search_index_ = text_search_index_ ? text_search_index_ - 1 : text_search_hits_.size() - 1;
        const auto& hit = text_search_hits_[text_search_index_];
        if (hit.node_id == glance::app::json_no_parent)
        {
            if (text_editor_) text_editor_->reveal_search_match(text_search_index_);
        }
        else
        {
            auto child = hit.node_id;
            while (child < json_nodes_.size())
            {
                const auto parent = json_nodes_[child].parent_id;
                if (parent >= json_nodes_.size()) break;
                if (json_expanded_nodes_.insert(parent).second) search_expanded_json_nodes_.insert(parent);
                const auto& siblings = json_children_[parent];
                const auto found = std::find(siblings.begin(), siblings.end(), child);
                if (found != siblings.end())
                    json_visible_child_counts_[parent] = std::max(json_visible_child_counts_[parent], static_cast<std::size_t>(found - siblings.begin()) + 1);
                child = parent;
            }
            sync_json_projection(false);
            refresh_realized_json_rows();
            const auto row = std::find(json_visible_rows_.begin(), json_visible_rows_.end(), hit.node_id);
            if (row != json_visible_rows_.end()) JsonTreeList().ScrollIntoView(JsonTreeList().Items().GetAt(static_cast<std::uint32_t>(row - json_visible_rows_.begin())));
        }
        update_text_search_status();
    }

    void MainWindow::update_text_search_status()
    {
        SearchPreviousButton().IsEnabled(!text_search_hits_.empty());
        SearchNextButton().IsEnabled(!text_search_hits_.empty());
        if (text_search_error_ != glance::app::TextSearchError::none)
        {
            const wchar_t* key = text_search_error_ == glance::app::TextSearchError::invalid_expression ? L"SearchInvalidRegex" :
                text_search_error_ == glance::app::TextSearchError::timed_out ? L"SearchTimedOut" : L"SearchFailed";
            TextSearchStatus().Text(glance::app::localize(key));
            return;
        }
        if (text_search_loading_)
        {
            TextSearchStatus().Text(current_text_has_more_ && !(current_text_json_ && json_tree_mode_)
                ? glance::app::localize_format(L"SearchReading", {std::to_wstring(text_source_bytes_read_ / 1024)})
                : glance::app::localize(L"SearchSearching"));
            return;
        }
        TextSearchStatus().Text(glance::app::localize_format(text_search_incomplete_ ? L"SearchPartialCount" : L"SearchCount",
            {std::to_wstring(text_search_hits_.empty() ? 0 : text_search_index_ + 1), std::to_wstring(text_search_hits_.size())}));
    }
}
