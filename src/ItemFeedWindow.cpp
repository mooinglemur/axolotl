#include "ItemFeedWindow.h"
#include <algorithm>
#include <cstring>
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

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("Filter:");
    ImGui::SameLine();
    char filter_buf[256];
    strncpy(filter_buf, filter_text_.c_str(), sizeof(filter_buf));
    filter_buf[sizeof(filter_buf) - 1] = '\0';

    ImGui::PushItemWidth(-1.0f);
    if (ImGui::InputText("##Filter", filter_buf, sizeof(filter_buf))) {
      filter_text_ = filter_buf;
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    auto const &history = ap_network_.GetItemHistory();
    const std::set<int> &my_slots = ap_network_.GetConnectedSlots();

    if (history.size() != last_history_size_ ||
        filter_text_ != last_filter_text_) {
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
      last_history_size_ = history.size();
      last_filter_text_ = filter_text_;
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

      bool was_at_bottom =
          (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f);
      bool history_grew = (display_indices_.size() > last_history_size_);

      if (selection_anchor_ >= (int)history.size())
        selection_anchor_ = history.empty() ? -1 : (int)history.size() - 1;
      if (selection_active_ >= (int)history.size())
        selection_active_ = history.empty() ? -1 : (int)history.size() - 1;

      float avg_height = ImGui::GetTextLineHeightWithSpacing();

      ImGuiListClipper clipper;
      bool use_clipper = (display_indices_.size() > 100);
      if (use_clipper) {
        clipper.Begin((int)display_indices_.size(), avg_height);
      }

      int pass = 0;
      while ((use_clipper && clipper.Step()) || (!use_clipper && pass == 0)) {
        int start = use_clipper ? clipper.DisplayStart : 0;
        int end =
            use_clipper ? clipper.DisplayEnd : (int)display_indices_.size();
        pass++;

        for (int row_idx = start; row_idx < end; ++row_idx) {
          int i = display_indices_[row_idx];
          const auto &rm = history[i];
          ImGui::PushID(i);

          ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

          ImVec2 pos_start = ImGui::GetCursorScreenPos();
          ImGui::BeginGroup();

          // Timestamp
          const std::tm *tm_ptr = &rm.local_time;
          char time_buf[64];
          if (show_long_dates_) {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_long.c_str(), tm_ptr);
          } else {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_short.c_str(), tm_ptr);
          }

          RenderRichMessageWrapped(time_buf, rm.parts, &my_slots);
          ImGui::EndGroup();
          ImVec2 item_size = ImGui::GetItemRectSize();
          ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
          ImGui::SetCursorScreenPos(pos_start);

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
          row_height_cache_[i] = item_size.y;

          bool is_selected = false;
          if (selection_anchor_ != -1 && selection_active_ != -1) {
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            is_selected = (i >= start && i <= end);
          }

          char label[32];
          snprintf(label, sizeof(label), "##row_%d", i);
          if (ImGui::Selectable(label, is_selected,
                                ImGuiSelectableFlags_SpanAllColumns |
                                    ImGuiSelectableFlags_AllowOverlap,
                                ImVec2(0, item_size.y))) {
          }
          if (ImGui::IsItemClicked(0)) {
            if (ImGui::GetIO().KeyShift && selection_anchor_ != -1)
              selection_active_ = i;
            else {
              if (selection_anchor_ == i && selection_active_ == i) {
                selection_anchor_ = -1;
                selection_active_ = -1;
              } else {
                selection_anchor_ = i;
                selection_active_ = i;
              }
            }
          }
          if (ImGui::IsItemHovered(
                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
              ImGui::IsMouseDown(0))
            selection_active_ = i;

          if (ImGui::BeginPopupContextItem("FeedLineCtx",
                                           ImGuiPopupFlags_MouseButtonRight)) {
            if (selection_anchor_ == -1) {
              selection_anchor_ = i;
              selection_active_ = i;
            }
            if (ImGui::MenuItem("Copy selection")) {
              std::string selected_text;
              int start =
                  std::max(0, std::min(selection_anchor_, selection_active_));
              int end =
                  std::min((int)history.size() - 1,
                           std::max(selection_anchor_, selection_active_));
              for (int j = start; j <= end && j < (int)history.size(); ++j) {
                const auto &rm_j = history[j];
                for (const auto &p : rm_j.parts)
                  selected_text += p.text;
                if (j < end)
                  selected_text += "\n";
              }
              ImGui::SetClipboardText(selected_text.c_str());
            }
            if (ImGui::MenuItem("Copy selection (with timestamps)")) {
              std::string selected_text;
              int start =
                  std::max(0, std::min(selection_anchor_, selection_active_));
              int end =
                  std::min((int)history.size() - 1,
                           std::max(selection_anchor_, selection_active_));
              for (int j = start; j <= end && j < (int)history.size(); ++j) {
                const auto &rm_j = history[j];
                const std::tm *tm_ptr = &rm_j.local_time;
                char time_buf[64];
                if (tm_ptr->tm_yday != current_yday ||
                    tm_ptr->tm_year != current_year) {
                  std::strftime(time_buf, sizeof(time_buf),
                                settings_.timestamp_format_long.c_str(),
                                tm_ptr);
                } else {
                  std::strftime(time_buf, sizeof(time_buf),
                                settings_.timestamp_format_short.c_str(),
                                tm_ptr);
                }
                selected_text += time_buf;
                selected_text += " ";
                for (const auto &p : rm_j.parts)
                  selected_text += p.text;
                if (j < end)
                  selected_text += "\n";
              }
              ImGui::SetClipboardText(selected_text.c_str());
            }
            if (ImGui::MenuItem("Clear Selection")) {
              selection_anchor_ = -1;
              selection_active_ = -1;
            }
            ImGui::EndPopup();
          }

          ImGui::PopID();
        }
        if (!use_clipper)
          break;
      }
      ImGui::GetWindowDrawList()->ChannelsMerge();

      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_active_ = -1;
      }

      float current_scroll_max_y = ImGui::GetScrollMaxY();
      float current_window_width = ImGui::GetWindowWidth();
      if (was_at_bottom &&
          (history_grew || current_scroll_max_y != last_scroll_max_y_ ||
           current_window_width != last_window_width_)) {
        ImGui::SetScrollY(current_scroll_max_y + 10000.0f);
      }

      last_history_size_ = (int)display_indices_.size();
      last_scroll_max_y_ = current_scroll_max_y;
      last_window_width_ = current_window_width;

      if (custom_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
  }
  ImGui::End();
}
