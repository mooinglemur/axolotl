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

    auto const &history = ap_network_.GetItemHistory();
    const std::set<int> &my_slots = ap_network_.GetConnectedSlots();

    if (ImGui::BeginChild("FeedScrollingRegion")) {
      if (custom_font)
        ImGui::PushFont(custom_font);

      std::time_t now = std::time(nullptr);
      std::tm *now_tm = std::localtime(&now);
      int current_yday = now_tm->tm_yday;
      int current_year = now_tm->tm_year;

      int visible_row_idx = 0;
      for (int i = 0; i < (int)history.size(); ++i) {
        const auto &rm = history[i];
        bool is_selected = false;

        if (personal_only_) {
          bool matches = (my_slots.count(rm.sender_slot) ||
                          my_slots.count(rm.receiver_slot));
          if (!matches)
            continue;
        }

        if (!filter_text_.empty()) {
          std::string full_text;
          for (const auto &p : rm.parts)
            full_text += p.text;
          std::string l_text = full_text;
          std::string l_filter = filter_text_;
          std::transform(l_text.begin(), l_text.end(), l_text.begin(),
                         ::tolower);
          std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                         ::tolower);
          if (l_text.find(l_filter) == std::string::npos)
            continue;
        }

        ImGui::PushID(i);

        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

        ImVec2 pos_start = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();

        // Timestamp
        std::time_t t = (std::time_t)rm.timestamp;
        std::tm *tm_ptr = std::localtime(&t);
        char time_buf[64];
        if (tm_ptr->tm_yday != current_yday ||
            tm_ptr->tm_year != current_year) {
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

        if (visible_row_idx % 2 == 1) {
          float x_min =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
          float x_max =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(x_min, pos_start.y),
              ImVec2(x_max, pos_start.y + item_size.y),
              ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
        }
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
            selection_anchor_ = i;
            selection_active_ = i;
          }
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(0))
          selection_active_ = i;

        if (ImGui::BeginPopupContextItem("FeedLineCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (ImGui::MenuItem("Copy Selection")) {
            std::string selected_text;
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            for (int j = start; j <= end; ++j) {
              const auto &rm_j = history[j];
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

        ImGui::GetWindowDrawList()->ChannelsMerge();
        ImGui::PopID();
        visible_row_idx++;
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
