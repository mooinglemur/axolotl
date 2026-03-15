#include "ItemFeedWindow.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <imgui.h>

ItemFeedWindow::ItemFeedWindow(const std::vector<RichMessage> &history,
                               std::function<int()> get_global_slot,
                               bool personal_only, const std::string &name)
    : Window(name), history_(history), get_global_slot_(get_global_slot),
      personal_only_(personal_only) {}

void ItemFeedWindow::Render(ImFont *custom_font, ImFont *preview_font,
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

    ImGui::Separator();

    bool show_date = false;
    if (!filter_text_.empty()) {
      std::time_t now = std::time(nullptr);
      std::tm *now_tm = std::localtime(&now);
      int current_yday = now_tm->tm_yday;
      int current_year = now_tm->tm_year;

      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      int global_slot = get_global_slot_ ? get_global_slot_() : -1;

      for (const auto &rm : history_) {
        bool passes_personal = true;
        if (personal_only_) {
          passes_personal = (rm.sender_slot == global_slot ||
                             rm.receiver_slot == global_slot);
        }
        if (!passes_personal)
          continue;

        std::string full_text;
        for (const auto &p : rm.parts)
          full_text += p.text;

        std::string l_text = full_text;
        std::transform(l_text.begin(), l_text.end(), l_text.begin(), ::tolower);
        if (l_text.find(l_filter) != std::string::npos) {
          std::time_t t = (std::time_t)rm.timestamp;
          std::tm *rm_tm = std::localtime(&t);
          if (rm_tm->tm_yday != current_yday ||
              rm_tm->tm_year != current_year) {
            show_date = true;
            break;
          }
        }
      }
    }

    if (ImGui::BeginChild("FeedScrollingRegion")) {
      if (custom_font)
        ImGui::PushFont(custom_font);
      for (int i = 0; i < (int)history_.size(); ++i) {
        const auto &rm = history_[i];

        bool passes_personal = true;
        if (personal_only_) {
          int global_slot = get_global_slot_ ? get_global_slot_() : -1;
          passes_personal = (rm.sender_slot == global_slot ||
                             rm.receiver_slot == global_slot);
        }

        if (!passes_personal)
          continue;

        std::string full_text;
        for (const auto &p : rm.parts)
          full_text += p.text;

        bool show = false;
        if (filter_text_.empty()) {
          show = true;
        } else {
          std::string l_text = full_text;
          std::string l_filter = filter_text_;
          std::transform(l_text.begin(), l_text.end(), l_text.begin(),
                         ::tolower);
          std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                         ::tolower);
          show = (l_text.find(l_filter) != std::string::npos);
        }

        if (show) {
          ImGui::PushID(i);

          ImGui::GetWindowDrawList()->ChannelsSplit(2);
          ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

          ImVec2 pos_start = ImGui::GetCursorScreenPos();
          ImGui::BeginGroup();

          // Timestamp
          std::time_t t = (std::time_t)rm.timestamp;
          std::tm *tm_ptr = std::localtime(&t);
          char time_buf[32];
          if (show_date) {
            std::strftime(time_buf, sizeof(time_buf), "[%Y-%m-%d %H:%M:%S]",
                          tm_ptr);
          } else {
            std::strftime(time_buf, sizeof(time_buf), "[%H:%M:%S]", tm_ptr);
          }

          RenderRichMessageWrapped(time_buf, rm.parts);

          ImGui::EndGroup();
          ImVec2 item_size = ImGui::GetItemRectSize();
          ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
          ImGui::SetCursorScreenPos(pos_start);

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
            // Updated via mouse state
          }
          if (ImGui::IsItemClicked(0)) {
            if (ImGui::GetIO().KeyShift && selection_anchor_ != -1) {
              selection_active_ = i;
            } else {
              if (selection_anchor_ == i && selection_active_ == i) {
                // Clicked the only selected item, so toggle off
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
              ImGui::IsMouseDown(0)) {
            selection_active_ = i;
          }

          ImGui::GetWindowDrawList()->ChannelsMerge();
          ImGui::PopID();
        }
      }

      if (ImGui::BeginPopupContextWindow("ItemFeedCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Copy Selected")) {
          if (selection_anchor_ != -1 && selection_active_ != -1) {
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            std::string export_text;
            for (int sel = start; sel <= end; ++sel) {
              const auto &m = history_[sel];
              std::time_t mt = (std::time_t)m.timestamp;
              std::tm *mtm = std::localtime(&mt);
              char mt_buf[32];
              if (show_date) {
                std::strftime(mt_buf, sizeof(mt_buf), "[%Y-%m-%d %H:%M:%S] ",
                              mtm);
              } else {
                std::strftime(mt_buf, sizeof(mt_buf), "[%H:%M:%S] ", mtm);
              }
              export_text += mt_buf;
              for (const auto &p : m.parts)
                export_text += p.text;
              export_text += "\n";
            }
            ImGui::SetClipboardText(export_text.c_str());
          }
        }
        if (ImGui::MenuItem("Clear Selection")) {
          selection_anchor_ = -1;
          selection_active_ = -1;
        }
        ImGui::EndPopup();
      }

      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_anchor_ = -1;
        selection_active_ = -1;
      }

      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
      if (custom_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
  }
  ImGui::End();
}
