#include "ItemFeedWindow.h"
#include <algorithm>
#include <ctime>
#include <imgui.h>
#include <set>

ItemFeedWindow::ItemFeedWindow(ArchipelagoNetwork &ap_network,
                               const ConnectionSettings &settings,
                               bool personal_only, const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings),
      personal_only_(personal_only) {}

void ItemFeedWindow::Render(std::tm *current_tm, ImFont *custom_font,
                            ImFont *preview_font,
                            ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1.0f);
    RenderFilterInput("##Filter", filter_text_, focus_filter_);
    ImGui::PopItemWidth();

    ImGui::Separator();

    auto const &history = ap_network_.GetItemHistory();
    const std::set<int> &my_slots = ap_network_.GetConnectedSlots();

    bool filter_changed = (filter_text_ != last_filter_text_);
    if (history.size() != last_history_data_size_ || filter_changed) {
      display_indices_.clear();
      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      show_long_dates_ = false;
      int current_yday = current_tm->tm_yday;
      int current_year = current_tm->tm_year;

      for (int i = 0; i < (int)history.size(); ++i) {
        const auto &rm = history[i];
        if (personal_only_) {
          if (!my_slots.count(rm.sender_slot) &&
              !my_slots.count(rm.receiver_slot))
            continue;
        }

        if (!l_filter.empty()) {
          std::string full_text;
          for (const auto &p : rm.parts)
            full_text += p.text;
          std::transform(full_text.begin(), full_text.end(), full_text.begin(),
                         ::tolower);
          if (full_text.find(l_filter) == std::string::npos)
            continue;

          // Only check for long dates if filter is NOT empty
          if (rm.local_time.tm_yday != current_yday ||
              rm.local_time.tm_year != current_year) {
            show_long_dates_ = true;
          }
        }
        display_indices_.push_back(i);
      }
      row_height_cache_.resize(history.size(), -1.0f);
      last_history_data_size_ = history.size();
    }

    if (ImGui::BeginChild("FeedScrollingRegion", ImVec2(0, 0),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      if (custom_font)
        ImGui::PushFont(custom_font);

      ImGui::GetWindowDrawList()->ChannelsSplit(2);

      int current_yday = current_tm->tm_yday;
      int current_year = current_tm->tm_year;

      float threshold = 4.0f * ImGui::GetTextLineHeightWithSpacing();
      bool was_at_bottom =
          (last_scroll_max_y_ <= 0.0f ||
           ImGui::GetScrollY() >= last_scroll_max_y_ - threshold ||
           ImGui::GetScrollY() >= ImGui::GetScrollMaxY());
      bool history_grew =
          ((int)display_indices_.size() > last_display_indices_size_);

      if (selection_anchor_idx_ >= (int)display_indices_.size())
        selection_anchor_idx_ =
            display_indices_.empty() ? -1 : (int)display_indices_.size() - 1;
      if (selection_active_idx_ >= (int)display_indices_.size())
        selection_active_idx_ =
            display_indices_.empty() ? -1 : (int)display_indices_.size() - 1;

      float measured_avg =
          (measured_rows_count_ > 0)
              ? (float)(measured_height_sum_ / measured_rows_count_)
              : ImGui::GetTextLineHeightWithSpacing();
      float clipper_height =
          std::min(measured_avg, ImGui::GetTextLineHeightWithSpacing() * 1.5f);
      ImGuiListClipper clipper;
      bool use_clipper = (display_indices_.size() > 100);
      if (use_clipper) {
        clipper.Begin((int)display_indices_.size(), clipper_height);
      }

      bool in_bottom_zone =
          (ImGui::GetScrollY() > ImGui::GetScrollMaxY() - 500.0f);
      bool force_bottom_render =
          (use_clipper && (was_at_bottom || history_grew || in_bottom_zone) &&
           !display_indices_.empty());
      int manual_tail_start =
          force_bottom_render ? std::max(0, (int)display_indices_.size() - 200)
                              : (int)display_indices_.size();

      auto render_row = [&](int row_idx) {
        int i = display_indices_[row_idx];
        const auto &rm = history[i];
        ImGui::PushID(i);

        ImVec2 pos_start = ImGui::GetCursorScreenPos();

        bool is_selected = false;
        if (selection_anchor_idx_ != -1 && selection_active_idx_ != -1) {
          int s_start = std::min(selection_anchor_idx_, selection_active_idx_);
          int s_end = std::max(selection_anchor_idx_, selection_active_idx_);
          is_selected = (row_idx >= s_start && row_idx <= s_end);
        }

        float row_h = row_height_cache_[i];
        if (row_h < 0)
          row_h = ImGui::GetTextLineHeightWithSpacing();

        char label[32];
        snprintf(label, sizeof(label), "##row_%d", i);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, row_h))) {
        }
        if (ImGui::IsItemClicked(0)) {
          if (ImGui::GetIO().KeyShift && selection_anchor_idx_ != -1)
            selection_active_idx_ = row_idx;
          else {
            if (selection_anchor_idx_ == row_idx &&
                selection_active_idx_ == row_idx) {
              selection_anchor_idx_ = -1;
              selection_active_idx_ = -1;
            } else {
              selection_anchor_idx_ = row_idx;
              selection_active_idx_ = row_idx;
            }
          }
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(0))
          selection_active_idx_ = row_idx;

        ImGui::SetCursorScreenPos(pos_start);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        ImGui::BeginGroup();

        // Timestamp
        const std::tm *tm_ptr = &rm.local_time;
        char time_buf[64];
        char *time_ptr = nullptr;
        if (settings_.show_feed_timestamps) {
          if (show_long_dates_) {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_long.c_str(), tm_ptr);
          } else {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_short.c_str(), tm_ptr);
          }
          time_ptr = time_buf;
        }

        RenderRichMessageWrapped(time_ptr, rm.parts, &ap_network_, &my_slots);
        ImGui::EndGroup();
        ImVec2 item_size = ImGui::GetItemRectSize();
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);

        if (settings_.shade_alternating_rows && row_idx % 2 == 1) {
          float x_min =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
          float x_max =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(x_min, pos_start.y),
              ImVec2(x_max, pos_start.y + item_size.y),
              ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
        }
        float h = item_size.y + ImGui::GetStyle().ItemSpacing.y;
        if (row_height_cache_[i] < 0) {
          measured_height_sum_ += h;
          measured_rows_count_++;
        }
        row_height_cache_[i] = h;

        if (ImGui::BeginPopupContextItem("FeedLineCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (selection_anchor_idx_ == -1) {
            selection_anchor_idx_ = row_idx;
            selection_active_idx_ = row_idx;
          }
          if (ImGui::MenuItem("Copy selection")) {
            std::string selected_text;
            int s_start = std::max(
                0, std::min(selection_anchor_idx_, selection_active_idx_));
            int s_end = std::min(
                (int)display_indices_.size() - 1,
                std::max(selection_anchor_idx_, selection_active_idx_));
            for (int k = s_start; k <= s_end; ++k) {
              int hist_idx = display_indices_[k];
              const auto &rm_k = history[hist_idx];
              for (const auto &p : rm_k.parts)
                selected_text += p.text;
              if (k < s_end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Copy selection (with timestamps)")) {
            std::string selected_text;
            int s_start = std::max(
                0, std::min(selection_anchor_idx_, selection_active_idx_));
            int s_end = std::min(
                (int)display_indices_.size() - 1,
                std::max(selection_anchor_idx_, selection_active_idx_));
            for (int k = s_start; k <= s_end; ++k) {
              int hist_idx = display_indices_[k];
              const auto &rm_k = history[hist_idx];
              const std::tm *tm_ptr_k = &rm_k.local_time;
              char t_buf[64];
              if (tm_ptr_k->tm_yday != current_yday ||
                  tm_ptr_k->tm_year != current_year) {
                std::strftime(t_buf, sizeof(t_buf),
                              settings_.timestamp_format_long.c_str(),
                              tm_ptr_k);
              } else {
                std::strftime(t_buf, sizeof(t_buf),
                              settings_.timestamp_format_short.c_str(),
                              tm_ptr_k);
              }
              selected_text += t_buf;
              selected_text += " ";
              for (const auto &p : rm_k.parts)
                selected_text += p.text;
              if (k < s_end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Clear Selection")) {
            selection_anchor_idx_ = -1;
            selection_active_idx_ = -1;
          }
          ImGui::EndPopup();
        }

        ImGui::PopID();
      };

      int pass = 0;
      float max_y = ImGui::GetCursorPosY();
      while ((use_clipper && clipper.Step()) || (!use_clipper && pass == 0)) {
        int start = use_clipper ? clipper.DisplayStart : 0;
        int end =
            use_clipper ? clipper.DisplayEnd : (int)display_indices_.size();
        pass++;

        for (int row_idx = start; row_idx < end; ++row_idx) {
          if (force_bottom_render && row_idx >= manual_tail_start)
            break; // Handled by manual tail

          render_row(row_idx);
        }
        if (!use_clipper)
          break;
      }
      if (use_clipper) {
        ImGui::SetCursorPosY(max_y);
        ImGui::Dummy(ImVec2(0.0f, 1e-6f)); // satisfy ImGui assertion about
                                           // growing window via SetCursorPos
      }

      // Safe Bottom: Manually render the tail
      // if needed to eliminate ghost space
      if (force_bottom_render) {
        ImGui::SetCursorPosY((float)manual_tail_start * clipper_height);
        for (int row_idx = manual_tail_start;
             row_idx < (int)display_indices_.size(); ++row_idx) {
          render_row(row_idx);
        }
      }
      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_active_idx_ = -1;
        selection_anchor_idx_ = -1;
      }

      float current_scroll_max_y = ImGui::GetScrollMaxY();
      float current_window_width = ImGui::GetWindowWidth();
      if ((was_at_bottom &&
           (history_grew || current_scroll_max_y != last_scroll_max_y_ ||
            current_window_width != last_window_width_)) ||
          filter_changed) {
        // Idempotency check: only snap if we are NOT already at the bottom
        float gap = current_scroll_max_y - ImGui::GetScrollY();
        if (gap > 1.0f || filter_changed) {
          ImGui::SetScrollY(current_scroll_max_y);
        }
      }

      if (current_window_width != last_window_width_) {
        std::fill(row_height_cache_.begin(), row_height_cache_.end(), -1.0f);
        last_avg_height_ = -1.0f;
        measured_height_sum_ = 0;
        measured_rows_count_ = 0;
      }

      last_display_indices_size_ = (int)display_indices_.size();
      last_scroll_max_y_ = current_scroll_max_y;
      last_window_width_ = current_window_width;
      last_filter_text_ = filter_text_;

      ImGui::GetWindowDrawList()->ChannelsMerge();
      if (custom_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
  }
  ImGui::End();
}
