#include "ReceivedItemsWindow.h"
#include <algorithm>
#include <ctime>
#include <imgui.h>
#include <map>
#include <set>
#include <vector>

ReceivedItemsWindow::ReceivedItemsWindow(ArchipelagoNetwork &ap_network,
                                         const ConnectionSettings &settings,
                                         const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings) {
  collapse_ = settings_.collapse_received_items;
}

void ReceivedItemsWindow::Render(ImFont *custom_font, ImFont *preview_font,
                                 ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("Filter:");
    ImGui::SameLine();
    char filter_buf[256];
    strncpy(filter_buf, filter_text_.c_str(), sizeof(filter_buf) - 1);
    filter_buf[sizeof(filter_buf) - 1] = '\0';

    float collapse_width = ImGui::CalcTextSize("Collapse").x +
                           ImGui::GetStyle().ItemInnerSpacing.x +
                           ImGui::GetFrameHeight();
    ImGui::PushItemWidth(-(collapse_width + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputText("##Filter", filter_buf, sizeof(filter_buf))) {
      filter_text_ = filter_buf;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Checkbox("Collapse", &collapse_);

    ImGui::Separator();

    auto history = ap_network_.GetAggregatedReceivedItems();

    struct DisplayRow {
      RichMessage rm;
      int count = 1;
      std::string text_cache;
    };
    std::vector<DisplayRow> display_rows;

    if (collapse_) {
      std::map<std::pair<std::string, std::string>, DisplayRow> groups;
      for (const auto &rm : history) {
        std::string text;
        for (const auto &p : rm.parts)
          text += p.text;

        auto key = std::make_pair(rm.source_slot, text);
        if (groups.find(key) == groups.end()) {
          groups[key] = {rm, 1, text};
        } else {
          groups[key].count++;
          if (rm.timestamp > groups[key].rm.timestamp) {
            groups[key].rm.timestamp = rm.timestamp;
          }
        }
      }
      for (auto const &[key, dr] : groups)
        display_rows.push_back(dr);
    } else {
      for (const auto &rm : history) {
        std::string text;
        for (const auto &p : rm.parts)
          text += p.text;
        display_rows.push_back({rm, 1, text});
      }
    }

    if (ImGui::BeginTable(
            "ReceivedItemsTable", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_DefaultSort);
      ImGui::TableSetupColumn("Slot");
      ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0 && display_rows.size() > 1) {
          const auto *spec = &specs->Specs[0];
          std::sort(
              display_rows.begin(), display_rows.end(),
              [&](const DisplayRow &a, const DisplayRow &b) {
                if (spec->ColumnIndex == 0) {
                  if (a.rm.timestamp != b.rm.timestamp)
                    return (spec->SortDirection == ImGuiSortDirection_Ascending)
                               ? (a.rm.timestamp < b.rm.timestamp)
                               : (a.rm.timestamp > b.rm.timestamp);
                } else if (spec->ColumnIndex == 1) {
                  int delta = a.rm.source_slot.compare(b.rm.source_slot);
                  if (delta != 0)
                    return (spec->SortDirection == ImGuiSortDirection_Ascending)
                               ? (delta < 0)
                               : (delta > 0);
                } else if (spec->ColumnIndex == 2) {
                  int delta = a.text_cache.compare(b.text_cache);
                  if (delta != 0)
                    return (spec->SortDirection == ImGuiSortDirection_Ascending)
                               ? (delta < 0)
                               : (delta > 0);
                }
                return false;
              });
        }
      }

      if (custom_font)
        ImGui::PushFont(custom_font);

      std::time_t now = std::time(nullptr);
      std::tm *now_tm = std::localtime(&now);
      int current_yday = now_tm->tm_yday;
      int current_year = now_tm->tm_year;

      const std::set<int> &my_slots = ap_network_.GetConnectedSlots();
      for (int i = 0; i < (int)display_rows.size(); ++i) {
        const auto &row = display_rows[i];
        const auto &rm = row.rm;

        if (!filter_text_.empty()) {
          std::string l_text = row.text_cache;
          std::string l_filter = filter_text_;
          std::transform(l_text.begin(), l_text.end(), l_text.begin(),
                         ::tolower);
          std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                         ::tolower);
          if (l_text.find(l_filter) == std::string::npos)
            continue;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        // Timestamp
        bool is_selected = false;
        if (selection_anchor_ != -1 && selection_active_ != -1) {
          int start = std::min(selection_anchor_, selection_active_);
          int end = std::max(selection_anchor_, selection_active_);
          is_selected = (i >= start && i <= end);
        }

        ImGui::PushID(i);
        char label[32];
        snprintf(label, sizeof(label), "##row_%d", i);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
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

        if (ImGui::BeginPopupContextItem("ReceivedLineCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (ImGui::MenuItem("Copy Selection")) {
            std::string selected_text;
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            for (int j = start; j <= end; ++j) {
              const auto &rm_j = display_rows[j].rm;
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
        ImGui::SameLine(0, 0);

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
        ImGui::Text("%s", time_buf);

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", rm.source_slot.c_str());

        ImGui::TableSetColumnIndex(2);
        if (rm.parts.empty()) {
          ImGui::TextDisabled("Unknown Item");
        } else {
          for (size_t p_idx = 0; p_idx < rm.parts.size(); ++p_idx) {
            const auto &p = rm.parts[p_idx];
            uint32_t use_color = p.color;
            if (p.player_id != -1) {
              use_color = my_slots.count(p.player_id) ? 0xFFFF00FF : 0xFFCCCCCC;
            }
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(use_color), "%s",
                               p.text.c_str());
            if (p_idx < rm.parts.size() - 1)
              ImGui::SameLine(0, 0);
          }
        }
        if (row.count > 1) {
          ImGui::SameLine(0, 4.0f);
          ImGui::TextDisabled("(x%d)", row.count);
        }
      }

      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_anchor_ = -1;
        selection_active_ = -1;
      }

      if (custom_font)
        ImGui::PopFont();

      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

      ImGui::EndTable();
    }
  }
  ImGui::End();
}

void ReceivedItemsWindow::SaveState(ConnectionSettings &settings) {
  settings.collapse_received_items = collapse_;
}
