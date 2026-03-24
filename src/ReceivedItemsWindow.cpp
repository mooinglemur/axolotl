#include "ReceivedItemsWindow.h"
#include <algorithm>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <set>
#include <vector>

ReceivedItemsWindow::ReceivedItemsWindow(ArchipelagoNetwork &ap_network,
                                         const ConnectionSettings &settings,
                                         const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings) {
  collapse_ = settings_.collapse_received_items;
}

void ReceivedItemsWindow::Render(std::tm *current_tm, ImFont *custom_font,
                                 ImFont *preview_font,
                                 ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
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

    auto const &history = ap_network_.GetAggregatedReceivedItems();
    uint64_t current_version = ap_network_.GetDataVersion();
    bool filter_changed = (filter_text_ != last_filter_text_);
    if (history.size() != last_history_count_ || collapse_ != last_collapse_ ||
        current_version != last_data_version_ || filter_changed ||
        force_rebuild_) {
      display_rows_.clear();

      if (collapse_) {
        std::map<std::pair<std::string, std::string>, DisplayRow> groups;
        for (const auto &rm : history) {
          std::string text;
          for (const auto &p : rm.parts)
            text += p.text;

          auto key = std::make_pair(rm.source_slot, text);
          if (groups.find(key) == groups.end()) {
            groups[key] = {rm, 1, text, ""};
          } else {
            groups[key].count++;
            if (rm.timestamp > groups[key].rm.timestamp) {
              groups[key].rm = rm;
            }
          }
        }
        for (auto const &[key, dr] : groups)
          display_rows_.push_back(dr);
      } else {
        for (const auto &rm : history) {
          std::string text;
          for (const auto &p : rm.parts)
            text += p.text;
          display_rows_.push_back({rm, 1, text, ""});
        }
      }

      for (auto &row : display_rows_) {
        row.text_lower_cache = row.text_cache;
        std::transform(row.text_lower_cache.begin(), row.text_lower_cache.end(),
                       row.text_lower_cache.begin(), ::tolower);
      }
      force_rebuild_ = false;
    }

    if (ImGui::BeginTable(
            "ReceivedItemsTable", 3,
            ImGuiTableFlags_Borders |
                (settings_.shade_alternating_rows ? ImGuiTableFlags_RowBg : 0) |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_DefaultSort);
      ImGui::TableSetupColumn("Slot");
      ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty || history.size() != last_history_count_ ||
            collapse_ != last_collapse_ ||
            current_version != last_data_version_ || filter_changed ||
            force_rebuild_) {
          if (specs->SpecsCount > 0 && display_rows_.size() > 1) {
            // ... (rest of sorting logic)
            const auto *spec = &specs->Specs[0];
            std::stable_sort(
                display_rows_.begin(), display_rows_.end(),
                [&](const DisplayRow &a, const DisplayRow &b) {
                  if (spec->ColumnIndex == 0) {
                    if (a.rm.timestamp != b.rm.timestamp)
                      return (spec->SortDirection ==
                              ImGuiSortDirection_Ascending)
                                 ? (a.rm.timestamp < b.rm.timestamp)
                                 : (a.rm.timestamp > b.rm.timestamp);
                  } else if (spec->ColumnIndex == 1) {
                    int delta = a.rm.source_slot.compare(b.rm.source_slot);
                    if (delta != 0)
                      return (spec->SortDirection ==
                              ImGuiSortDirection_Ascending)
                                 ? (delta < 0)
                                 : (delta > 0);
                  } else if (spec->ColumnIndex == 2) {
                    int delta = a.text_cache.compare(b.text_cache);
                    if (delta != 0)
                      return (spec->SortDirection ==
                              ImGuiSortDirection_Ascending)
                                 ? (delta < 0)
                                 : (delta > 0);
                  }

                  // Consistent tie-breakers
                  if (a.rm.timestamp != b.rm.timestamp)
                    return a.rm.timestamp < b.rm.timestamp;
                  int s_delta = a.rm.source_slot.compare(b.rm.source_slot);
                  if (s_delta != 0)
                    return s_delta < 0;
                  return a.text_cache < b.text_cache;
                });
          }
          specs->SpecsDirty = false;
          last_history_count_ = history.size();
          last_collapse_ = collapse_;
          last_data_version_ = current_version;
          last_filter_text_ = filter_text_;
        }
      }

      if (custom_font)
        ImGui::PushFont(custom_font);

      int current_yday = current_tm->tm_yday;
      int current_year = current_tm->tm_year;

      const std::set<int> &my_slots = ap_network_.GetConnectedSlots();
      if (selection_anchor_ >= (int)display_rows_.size())
        selection_anchor_ =
            display_rows_.empty() ? -1 : (int)display_rows_.size() - 1;
      if (selection_active_ >= (int)display_rows_.size())
        selection_active_ =
            display_rows_.empty() ? -1 : (int)display_rows_.size() - 1;

      bool timestamp_sorted = false;
      bool sort_descending = false;
      if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0 && specs->Specs[0].ColumnIndex == 0) {
          timestamp_sorted = true;
          sort_descending =
              (specs->Specs[0].SortDirection == ImGuiSortDirection_Descending);
        }
      }

      bool was_at_bottom =
          (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f);
      bool history_grew = ((int)display_rows_.size() > last_display_row_count_);
      float current_scroll_max_y = ImGui::GetScrollMaxY();
      float current_window_width = ImGui::GetWindowWidth();

      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      show_long_dates_ = false;
      if (!l_filter.empty()) {
        int current_yday = current_tm->tm_yday;
        int current_year = current_tm->tm_year;
        for (const auto &row_check : display_rows_) {
          if (row_check.text_lower_cache.find(l_filter) != std::string::npos) {
            if (row_check.rm.local_time.tm_yday != current_yday ||
                row_check.rm.local_time.tm_year != current_year) {
              show_long_dates_ = true;
              break;
            }
          }
        }
      }

      for (int i = 0; i < (int)display_rows_.size(); ++i) {
        const auto &row = display_rows_[i];
        const auto &rm = row.rm;

        if (!l_filter.empty()) {
          if (row.text_lower_cache.find(l_filter) == std::string::npos)
            continue;
        }

        ImGui::TableNextRow();
        bool is_selected = false;
        if (selection_anchor_ != -1 && selection_active_ != -1) {
          int start = std::min(selection_anchor_, selection_active_);
          int end = std::max(selection_anchor_, selection_active_);
          is_selected = (i >= start && i <= end);
        }

        ImGui::PushID(i);
        // Find first visible column for the selectable
        bool selectable_rendered = false;
        for (int col = 0; col < 3; col++) {
          if (ImGui::TableSetColumnIndex(col)) {
            char label[32];
            snprintf(label, sizeof(label), "##row_%d", i);
            if (ImGui::Selectable(label, is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_SelectOnClick)) {
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

            ImGuiTable *table = ImGui::GetCurrentTable();
            if (ImGui::BeginPopupContextItem(
                    "ReceivedLineCtx", ImGuiPopupFlags_MouseButtonRight)) {
              if (selection_anchor_ == -1) {
                selection_anchor_ = i;
                selection_active_ = i;
              }
              if (ImGui::MenuItem("Copy Selection")) {
                std::string selected_text;
                int start_sel =
                    std::max(0, std::min(selection_anchor_, selection_active_));
                int end_sel =
                    std::min((int)display_rows_.size() - 1,
                             std::max(selection_anchor_, selection_active_));
                if (start_sel != -1 && start_sel < (int)display_rows_.size()) {
                  if (table) {
                    struct ColumnOrder {
                      int index;
                      int order;
                    };
                    std::vector<ColumnOrder> visible_cols;
                    for (int c = 0; c < 3; c++) {
                      if (c < table->Columns.size() &&
                          table->Columns[c].IsEnabled) {
                        visible_cols.push_back(
                            {c, table->Columns[c].DisplayOrder});
                      }
                    }
                    std::sort(visible_cols.begin(), visible_cols.end(),
                              [](const auto &a, const auto &b) {
                                return a.order < b.order;
                              });

                    for (int j = start_sel; j <= end_sel; ++j) {
                      const auto &row_j = display_rows_[j];
                      for (size_t c_idx = 0; c_idx < visible_cols.size();
                           ++c_idx) {
                        int c_num = visible_cols[c_idx].index;
                        if (c_num == 0) {
                          // Timestamp column
                          const std::tm *tm_ptr_j = &row_j.rm.local_time;
                          char t_buf[64];
                          if (show_long_dates_) {
                            std::strftime(
                                t_buf, sizeof(t_buf),
                                settings_.timestamp_format_long.c_str(),
                                tm_ptr_j);
                          } else {
                            std::strftime(
                                t_buf, sizeof(t_buf),
                                settings_.timestamp_format_short.c_str(),
                                tm_ptr_j);
                          }
                          selected_text += t_buf;
                        } else if (c_num == 1) {
                          // Slot column
                          selected_text += row_j.rm.source_slot;
                        } else if (c_num == 2) {
                          // Item column
                          selected_text += row_j.text_cache;
                          if (row_j.count > 1) {
                            selected_text +=
                                " (x" + std::to_string(row_j.count) + ")";
                          }
                        }
                        if (c_idx < visible_cols.size() - 1)
                          selected_text += "\t";
                      }
                      if (j < end_sel)
                        selected_text += "\n";
                    }
                    ImGui::SetClipboardText(selected_text.c_str());
                  }
                }
              }
              if (ImGui::MenuItem("Clear Selection")) {
                selection_anchor_ = -1;
                selection_active_ = -1;
              }
              ImGui::EndPopup();
            }
            selectable_rendered = true;
            break;
          }
        }
        ImGui::PopID();

        // Restore column 0 if we were just using it for selectable,
        // or just proceed to actual data rendering
        ImGui::TableSetColumnIndex(0);
        if (selectable_rendered) {
          ImGui::SameLine(0, 0);
        }

        const std::tm *tm_ptr = &row.rm.local_time;
        char time_buf[64];
        if (show_long_dates_) {
          std::strftime(time_buf, sizeof(time_buf),
                        settings_.timestamp_format_long.c_str(), tm_ptr);
        } else {
          std::strftime(time_buf, sizeof(time_buf),
                        settings_.timestamp_format_short.c_str(), tm_ptr);
        }
        ImGui::TextDisabled("%s", time_buf);

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", rm.source_slot.c_str());
        if (ImGui::IsItemHovered()) {
          std::string game = ap_network_.ResolvePlayerGame(rm.receiver_slot);
          if (!game.empty()) {
            ImGui::SetTooltip("Game: %s", game.c_str());
          }
        }

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
            if (p.player_id != -1 && ImGui::IsItemHovered()) {
              std::string game = ap_network_.ResolvePlayerGame(p.player_id);
              if (!game.empty()) {
                ImGui::SetTooltip("Game: %s", game.c_str());
              }
            }
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

      if (timestamp_sorted) {
        if (sort_descending) {
          if (history_grew || filter_changed) {
            ImGui::SetScrollY(0.0f);
          }
        } else {
          if ((was_at_bottom &&
               (history_grew || current_scroll_max_y != last_scroll_max_y_ ||
                current_window_width != last_window_width_)) ||
              filter_changed) {
            ImGui::SetScrollHereY(1.0f);
          }
        }
      }

      last_display_row_count_ = (int)display_rows_.size();
      last_scroll_max_y_ = ImGui::GetScrollMaxY();
      last_window_width_ = current_window_width;
      last_filter_text_ = filter_text_;

      ImGui::EndTable();
    }
  }
  ImGui::End();
}

void ReceivedItemsWindow::SaveState(ConnectionSettings &settings) {
  settings.collapse_received_items = collapse_;
}
