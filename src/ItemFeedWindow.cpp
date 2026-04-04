#include "ItemFeedWindow.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <imgui.h>
#include <iostream>
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

    std::lock_guard<std::recursive_mutex> lock(ap_network_.GetStateMutex());
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

      double threshold = 2.0 * (double)ImGui::GetTextLineHeightWithSpacing();
      bool was_at_bottom =
          (last_scroll_max_y_ <= 0.0 ||
           (double)ImGui::GetScrollY() >= last_scroll_max_y_ - threshold);

      bool interacting = (ImGui::IsWindowHovered(
                              ImGuiHoveredFlags_RootAndChildWindows |
                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                          (ImGui::GetIO().MouseWheel != 0.0f ||
                           ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1)));

      // Re-lock ONLY if near bottom and NOT interacting
      if (was_at_bottom && !interacting) {
        locked_to_bottom_ = true;
      }

      // Unlock if user scrolls away manually while interacting
      if (interacting && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 5.0f) {
        locked_to_bottom_ = false;
      }

      int current_display_size = (int)display_indices_.size();

      if (selection_anchor_idx_ >= current_display_size)
        selection_anchor_idx_ =
            display_indices_.empty() ? -1 : current_display_size - 1;
      if (selection_active_idx_ >= current_display_size)
        selection_active_idx_ =
            display_indices_.empty() ? -1 : current_display_size - 1;

      bool in_bottom_zone =
          (ImGui::GetScrollY() > ImGui::GetScrollMaxY() - 128.0f);

      double min_h = (double)ImGui::GetTextLineHeightWithSpacing();
      double avg_h = (measured_rows_count_ > 0)
                         ? (double)(measured_height_sum_ / measured_rows_count_)
                         : min_h;
      if (avg_h > 10.0 * min_h)
        avg_h = 10.0 * min_h;

      // Part 28: Million-Pixel Absolute Alignment (Prefix-Sum Cache)
      cumulative_heights_.resize(current_display_size + 1);
      double current_y_sum = 0;
      for (int i = 0; i < current_display_size; ++i) {
        cumulative_heights_[i] = current_y_sum;
        int hist_idx = display_indices_[i];
        double h = (row_height_cache_[hist_idx] > 0) ? row_height_cache_[hist_idx] : avg_h;
        current_y_sum += h;
      }
      cumulative_heights_[current_display_size] = current_y_sum;
      double total_content_height = current_y_sum;

      ImGuiListClipper clipper;
      bool use_clipper = (current_display_size > 100);

      bool force_bottom_render =
          (use_clipper && (locked_to_bottom_ || in_bottom_zone) &&
           !display_indices_.empty());

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

        double row_h = row_height_cache_[i];
        if (row_h < 0)
          row_h = (double)ImGui::GetTextLineHeightWithSpacing();

        char label[32];
        snprintf(label, sizeof(label), "##row_%d", i);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, (float)row_h))) {
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
        double h =
            (double)item_size.y + (double)ImGui::GetStyle().ItemSpacing.y;
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

      bool is_any_interaction_val =
          (ImGui::IsWindowHovered() &&
           (ImGui::GetIO().MouseWheel != 0.0f || ImGui::IsMouseDown(0) ||
            ImGui::IsMouseDown(1))) ||
          (ImGui::IsWindowFocused() &&
           (ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
            ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
            ImGui::IsKeyDown(ImGuiKey_PageUp) ||
            ImGui::IsKeyDown(ImGuiKey_PageDown) ||
            ImGui::IsKeyDown(ImGuiKey_Home) || ImGui::IsKeyDown(ImGuiKey_End)));

      int rendered_count = 0;
      double current_window_width = (double)ImGui::GetWindowWidth();
      double current_scroll_max_y = (double)ImGui::GetScrollMaxY();

      if (use_clipper) {
        int count = current_display_size;

        int vis_start = 0;
        int vis_end = 0;
        double scroll_y = (double)ImGui::GetScrollY();
        double window_h = (double)ImGui::GetWindowHeight();

        // Optimized binary viewport search (O(log N))
        auto it_start = std::lower_bound(cumulative_heights_.begin(),
                                         cumulative_heights_.end(), scroll_y);
        vis_start = std::clamp(
            (int)std::distance(cumulative_heights_.begin(), it_start) - 1, 0,
            count - 1);

        auto it_end =
            std::lower_bound(cumulative_heights_.begin() + vis_start,
                             cumulative_heights_.end(), scroll_y + window_h);
        vis_end = std::clamp(
            (int)std::distance(cumulative_heights_.begin(), it_end), 0, count);

        clipper.Begin(count, (float)avg_h);
        clipper.IncludeItemsByIndex(std::clamp(vis_start - 30, 0, count),
                                    std::clamp(vis_end + 50, 0, count));
        clipper.IncludeItemsByIndex(0, std::clamp(20, 0, count));
        if (force_bottom_render) {
          clipper.IncludeItemsByIndex(std::clamp(count - 128, 0, count), count);
        }

        int lowest_rendered_idx = -1;
        double lowest_rendered_y = -1.0;

        while (clipper.Step()) {
          // Force alignment for this range (Overrides clipper's internal inaccurate skip)
          ImGui::SetCursorPosY((float)cumulative_heights_[clipper.DisplayStart]);

          for (int row_idx = clipper.DisplayStart; row_idx < clipper.DisplayEnd;
               ++row_idx) {
            render_row(row_idx);
            if (row_idx > lowest_rendered_idx) {
              lowest_rendered_idx = row_idx;
              lowest_rendered_y = (double)ImGui::GetCursorPosY();
            }
            rendered_count++;
          }
        }

        // Part 23/27/28/29: Deterministic LACH anchored to prefix sum
        if (lowest_rendered_idx >= 0) {
          double convergent_total = lowest_rendered_y;
          convergent_total +=
              (cumulative_heights_[current_display_size] -
               cumulative_heights_[lowest_rendered_idx + 1]);

          if (std::abs(convergent_total - total_content_height) > 0.001) {
            total_content_height = convergent_total;
          }
        }
      } else {
        for (int row_idx = 0; row_idx < (int)display_indices_.size();
             ++row_idx) {
          render_row(row_idx);
          rendered_count++;
        }
      }

      // Part 24/27/30/31: Threshold-based Stability (Hysteresis)
      // Only apply stabilization if NOT locked to bottom.
      // At the bottom, we want the layout to converge immediately to the true MaxY.
      double stability_threshold = 2.0 * avg_h;
      if (!locked_to_bottom_ &&
          (int)display_indices_.size() == last_display_indices_size_ &&
          current_window_width == last_window_width_ &&
          std::abs(total_content_height - last_stable_height_) <
              stability_threshold) {
        total_content_height = last_stable_height_;
      }
      last_stable_height_ = total_content_height;

      ImGui::SetCursorPosY((float)total_content_height);
      ImGui::Dummy(ImVec2(0.0f, 0.0f));

      // Part 25: Absolute Bottom Alignment
      if (locked_to_bottom_ && !is_any_interaction_val) {
        ImGui::SetScrollHereY(1.0f);
      }

      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_active_idx_ = -1;
        selection_anchor_idx_ = -1;
      }

      current_scroll_max_y = (double)ImGui::GetScrollMaxY();
      if (is_any_interaction_val) {
        locked_to_bottom_ =
            (ImGui::GetScrollY() >= (float)current_scroll_max_y - 20.0f) ||
            ImGui::IsKeyDown(ImGuiKey_End);
      }
      if (locked_to_bottom_ && !is_any_interaction_val) {
        // Ensure MaxY is up to date for stats
        current_scroll_max_y = (double)ImGui::GetScrollMaxY();
      }

      if (current_window_width != last_window_width_) {
        std::fill(row_height_cache_.begin(), row_height_cache_.end(), -1.0);
        last_avg_height_ = -1.0;
        measured_height_sum_ = 0;
        measured_rows_count_ = 0;
      }

      last_display_indices_size_ = (int)display_indices_.size();
      last_scroll_max_y_ = current_scroll_max_y;
      last_window_width_ = current_window_width;
      last_filter_text_ = filter_text_;

      if (ap_network_.IsScrollStatsEnabled()) {
        double cur_y = (double)ImGui::GetScrollY();
        double cur_h = (double)ImGui::GetWindowHeight();
        if (cur_y != (double)last_reported_scroll_y_ ||
            current_scroll_max_y != last_reported_scroll_max_y_ ||
            cur_h != (double)last_reported_window_h_ ||
            locked_to_bottom_ != last_reported_locked_) {
          char msg[512];
          snprintf(msg, sizeof(msg),
                   "[Feed Scroll] Y=%.1f MaxY=%.1f H=%.1f EstH=%.1f "
                   "Items=%d/%d Locked=%d Int=%d",
                   cur_y, current_scroll_max_y, cur_h, total_content_height,
                   rendered_count, (int)display_indices_.size(),
                   (int)locked_to_bottom_, (int)is_any_interaction_val);
          std::cout << msg << std::endl;

          last_reported_scroll_y_ = (float)cur_y;
          last_reported_scroll_max_y_ = (float)current_scroll_max_y;
          last_reported_window_h_ = (float)cur_h;
          last_reported_locked_ = locked_to_bottom_;
        }
      }

      ImGui::GetWindowDrawList()->ChannelsMerge();
      if (custom_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
  }
  ImGui::End();
}
