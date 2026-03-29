#include "HintWindow.h"
#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <set>
#include <string>

HintWindow::HintWindow(ArchipelagoNetwork &ap_network,
                       const ConnectionSettings &settings,
                       const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings) {}

void HintWindow::Render(std::tm *current_tm, ImFont *custom_font,
                        ImFont *preview_font, ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  const auto &hints = ap_network_.GetAggregatedHints();
  uint64_t current_version = ap_network_.GetDataVersion();


  ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1.0f);
    RenderFilterInput("##Filter", filter_text_, focus_filter_);
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (hints.size() != last_hint_count_ ||
        current_version != last_data_version_ || force_rebuild_) {
      resolved_hints_.clear();
      for (const auto &h : hints) {
        auto session = ap_network_.GetSession(h.source_slot);
        if (!session)
          continue;

        ResolvedHint rh;
        rh.hint = h;
        rh.item_name = session->ResolveItemName(h.item_id, h.receiver_slot);
        rh.receiver_name = session->ResolvePlayerName(h.receiver_slot);
        rh.location_name =
            session->ResolveLocationName(h.location_id, h.finder_slot);
        rh.entrance_name = h.entrance_name;
        rh.finder_name = session->ResolvePlayerName(h.finder_slot);
        if (h.status >= 40 || h.found)
          rh.status_name = "Found";
        else if (h.status >= 30)
          rh.status_name = "Priority";
        else if (h.status >= 20)
          rh.status_name = "Avoid";
        else if (h.status >= 10)
          rh.status_name = "Not Needed";
        else
          rh.status_name = "Unspecified";

        rh.lower_combined = rh.item_name + " " + rh.receiver_name + " " +
                            rh.location_name + " " + rh.entrance_name + " " +
                            rh.finder_name + " " +
                            rh.status_name; // Include status in combined string
        std::transform(rh.lower_combined.begin(), rh.lower_combined.end(),
                       rh.lower_combined.begin(), ::tolower);

        resolved_hints_.push_back(rh);
      }
      force_rebuild_ = false;
    }

    if (ImGui::BeginTable(
            "HintTable", 6,
            ImGuiTableFlags_Borders |
                (settings_.shade_alternating_rows ? ImGuiTableFlags_RowBg : 0) |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch |
                                          ImGuiTableColumnFlags_DefaultSort);
      ImGui::TableSetupColumn("Receiver", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Entrance", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Finder", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                              80.0f);
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
        if (sorts_specs->SpecsDirty || hints.size() != last_hint_count_ ||
            current_version != last_data_version_ || force_rebuild_) {
          if (sorts_specs->SpecsCount > 0 && resolved_hints_.size() > 1) {
            const auto *spec = &sorts_specs->Specs[0];
            std::stable_sort(
                resolved_hints_.begin(), resolved_hints_.end(),
                [&](const ResolvedHint &hA, const ResolvedHint &hB) {
                  int delta = 0;
                  if (spec->ColumnIndex == 0)
                    delta = NaturalCompare(hA.item_name, hB.item_name);
                  else if (spec->ColumnIndex == 1)
                    delta = NaturalCompare(hA.receiver_name, hB.receiver_name);
                  else if (spec->ColumnIndex == 2)
                    delta = NaturalCompare(hA.location_name, hB.location_name);
                  else if (spec->ColumnIndex == 3)
                    delta = NaturalCompare(hA.entrance_name, hB.entrance_name);
                  else if (spec->ColumnIndex == 4)
                    delta = NaturalCompare(hA.finder_name, hB.finder_name);
                  else if (spec->ColumnIndex == 5)
                    delta = hA.hint.status - hB.hint.status;

                  if (delta != 0)
                    return (spec->SortDirection ==
                            ImGuiSortDirection_Ascending)
                               ? (delta < 0)
                               : (delta > 0);

                  // Tie-breakers
                  int t1 = NaturalCompare(hA.item_name, hB.item_name);
                  if (t1 != 0)
                    return t1 < 0;
                  int t2 = NaturalCompare(hA.receiver_name, hB.receiver_name);
                  if (t2 != 0)
                    return t2 < 0;
                  return NaturalCompare(hA.location_name, hB.location_name) < 0;
                });
          }
          sorts_specs->SpecsDirty = false;
          last_hint_count_ = hints.size();
          last_data_version_ = current_version;
        }
      }

      if (custom_font)
        ImGui::PushFont(custom_font);

      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      const std::set<int> &my_slots = ap_network_.GetConnectedSlots();

      if (selection_anchor_ >= (int)hints.size())
        selection_anchor_ = hints.empty() ? -1 : (int)hints.size() - 1;
      if (selection_active_ >= (int)hints.size())
        selection_active_ = hints.empty() ? -1 : (int)hints.size() - 1;

      for (int i = 0; i < (int)resolved_hints_.size(); ++i) {
        const auto &rh = resolved_hints_[i];
        const auto &h = rh.hint;

        if (!l_filter.empty()) {
          if (rh.lower_combined.find(l_filter) == std::string::npos)
            continue;
        }

        std::string item = rh.item_name;
        std::string receiver = rh.receiver_name;
        std::string location = rh.location_name;
        std::string entrance =
            h.entrance_name.empty() ? "Vanilla" : h.entrance_name;
        std::string finder = rh.finder_name;
        std::string status = rh.status_name;

        ImGui::TableNextRow();

        bool is_selected = false;
        if (selection_anchor_ != -1 && selection_active_ != -1) {
          int start = std::min(selection_anchor_, selection_active_);
          int end = std::max(selection_anchor_, selection_active_);
          is_selected = (i >= start && i <= end);
        }

        ImGui::PushID(i);
        bool selectable_rendered = false;
        ImGui::TableSetColumnIndex(0);
        char label[32];
        snprintf(label, sizeof(label), "##hintrow_%d", i);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
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

        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::TableGetHoveredColumn() == 5) {
          // Only show status menu if we are the receiver (we have a session for
          // it)
          if (ap_network_.GetSessionByGlobalSlot(h.receiver_slot)) {
            ImGui::OpenPopup("StatusCtx");
          } else {
            ImGui::OpenPopup("RowCtx");
          }
        } else if (ImGui::IsItemHovered() &&
                   ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
          ImGui::OpenPopup("RowCtx");
        }

        if (ImGui::BeginPopup("StatusCtx")) {
          if (selection_anchor_ == -1) {
            selection_anchor_ = i;
            selection_active_ = i;
          }
          auto session_h = ap_network_.GetSessionByGlobalSlot(h.receiver_slot);
          if (ImGui::MenuItem("Set Priority", NULL, h.status == 30)) {
            if (session_h) {
              if (ap_network_.IsDebugMode())
                fprintf(stderr,
                        "[UI] Requesting UpdateHint (Priority) for loc %lld, "
                        "player %d\n",
                        (long long)h.location_id, h.receiver_slot);
              session_h->SendUpdateHint(h.location_id, h.finder_slot, 30);
            }
          }
          if (ImGui::MenuItem("Set Avoid", NULL, h.status == 20)) {
            if (session_h) {
              if (ap_network_.IsDebugMode())
                fprintf(stderr,
                        "[UI] Requesting UpdateHint (Avoid) for loc %lld, "
                        "player %d\n",
                        (long long)h.location_id, h.receiver_slot);
              session_h->SendUpdateHint(h.location_id, h.finder_slot, 20);
            }
          }
          if (ImGui::MenuItem("Set Not Needed", NULL, h.status == 10)) {
            if (session_h) {
              if (ap_network_.IsDebugMode())
                fprintf(stderr,
                        "[UI] Requesting UpdateHint (Not Needed) for loc "
                        "%lld, player %d\n",
                        (long long)h.location_id, h.receiver_slot);
              session_h->SendUpdateHint(h.location_id, h.finder_slot, 10);
            }
          }
          if (ImGui::MenuItem("Set Unspecified", NULL, h.status == 0)) {
            if (session_h) {
              if (ap_network_.IsDebugMode())
                fprintf(stderr,
                        "[UI] Requesting UpdateHint (Unspecified) for loc "
                        "%lld, player %d\n",
                        (long long)h.location_id, h.receiver_slot);
              session_h->SendUpdateHint(h.location_id, h.finder_slot, 0);
            }
          }
          ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("RowCtx")) {
          if (selection_anchor_ == -1) {
            selection_anchor_ = i;
            selection_active_ = i;
          }
          if (ImGui::MenuItem("Copy selection (with markdown)")) {
            std::string selected_text;
            int start =
                std::max(0, std::min(selection_anchor_, selection_active_));
            int end =
                std::min((int)resolved_hints_.size() - 1,
                         std::max(selection_anchor_, selection_active_));
            std::string l_filter_copy = filter_text_;
            std::transform(l_filter_copy.begin(), l_filter_copy.end(),
                           l_filter_copy.begin(), ::tolower);

            for (int j = start; j <= end && j < (int)resolved_hints_.size();
                 ++j) {
              const auto &rh_j = resolved_hints_[j];
              const auto &h_j = rh_j.hint;

              if (!l_filter_copy.empty()) {
                if (rh_j.lower_combined.find(l_filter_copy) ==
                    std::string::npos)
                  continue;
              }

              std::string cb = "**[Hint]:** " + rh_j.receiver_name + "'s *" +
                               rh_j.item_name + "* is at **" +
                               rh_j.location_name + "**";
              if (!h_j.entrance_name.empty())
                cb += " (via **" + h_j.entrance_name + "**)";
              cb += " in **" + rh_j.finder_name + "**'s World. (" +
                    rh_j.status_name + ")";
              selected_text += cb;
              if (j < end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Copy selection (tab-delimited)")) {
            std::string selected_text;
            int start_c =
                std::max(0, std::min(selection_anchor_, selection_active_));
            int end_c =
                std::min((int)resolved_hints_.size() - 1,
                         std::max(selection_anchor_, selection_active_));

            std::string l_filter_copy = filter_text_;
            std::transform(l_filter_copy.begin(), l_filter_copy.end(),
                           l_filter_copy.begin(), ::tolower);

            ImGuiTable *table = ImGui::GetCurrentTable();
            struct ColOrd {
              int index;
              int order;
            };
            std::vector<ColOrd> v_cols;
            if (table) {
              for (int c = 0; c < 6; c++) {
                if (c < table->Columns.size() && table->Columns[c].IsEnabled) {
                  v_cols.push_back({c, table->Columns[c].DisplayOrder});
                }
              }
              std::sort(v_cols.begin(), v_cols.end(),
                        [](const auto &a, const auto &b) {
                          return a.order < b.order;
                        });
            }

            for (int j = start_c; j <= end_c && j < (int)resolved_hints_.size();
                 ++j) {
              const auto &rh_j = resolved_hints_[j];

              if (!l_filter_copy.empty()) {
                if (rh_j.lower_combined.find(l_filter_copy) ==
                    std::string::npos)
                  continue;
              }

              for (size_t c_idx = 0; c_idx < v_cols.size(); ++c_idx) {
                int c_num = v_cols[c_idx].index;
                if (c_num == 0)
                  selected_text += rh_j.item_name;
                else if (c_num == 1)
                  selected_text += rh_j.receiver_name;
                else if (c_num == 2)
                  selected_text += rh_j.location_name;
                else if (c_num == 3)
                  selected_text += rh_j.entrance_name.empty()
                                       ? "Vanilla"
                                       : rh_j.entrance_name;
                else if (c_num == 4)
                  selected_text += rh_j.finder_name;
                else if (c_num == 5)
                  selected_text += rh_j.status_name;

                if (c_idx < v_cols.size() - 1)
                  selected_text += "\t";
              }
              if (j < end_c)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Clear selection")) {
            selection_anchor_ = -1;
            selection_active_ = -1;
          }
          ImGui::EndPopup();
        }

        ImGui::PopID();

        ImGui::TableSetColumnIndex(0);
        ImGui::SameLine(0, 0);

        uint32_t color = 0xFFFFFF00;
        if (h.item_flags & 0x01)
          color = 0xFFFF5FAF;
        else if (h.item_flags & 0x02)
          color = 0xFFED9564;
        else if (h.item_flags & 0x04)
          color = 0xFF0045FF;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s",
                           item.c_str());

        // Receiver column: Magenta if it's us
        ImGui::TableSetColumnIndex(1);
        if (my_slots.count(h.receiver_slot))
          ImGui::TextColored(ImColor(255, 0, 255), "%s", receiver.c_str());
        else
          ImGui::Text("%s", receiver.c_str());

        if (ImGui::IsItemHovered()) {
          std::string game = ap_network_.ResolvePlayerGame(h.receiver_slot);
          if (!game.empty()) {
            ImGui::SetTooltip("Game: %s", game.c_str());
          }
        }

        // Location column: Green
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImColor(0, 255, 0), "%s", location.c_str());

        // Entrance column
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%s", entrance.c_str());

        // Finder column
        ImGui::TableSetColumnIndex(4);
        if (my_slots.count(h.finder_slot))
          ImGui::TextColored(ImColor(255, 0, 255), "%s", finder.c_str());
        else
          ImGui::Text("%s", finder.c_str());

        if (ImGui::IsItemHovered()) {
          std::string game = ap_network_.ResolvePlayerGame(h.finder_slot);
          if (!game.empty()) {
            ImGui::SetTooltip("Game: %s", game.c_str());
          }
        }

        // Status column
        ImGui::TableSetColumnIndex(5);
        uint32_t s_color = 0xFFAAAAAA; // Gray (Unspecified)
        if (h.status >= 40 || h.found)
          s_color = 0xFF00FF00; // Green (Found)
        else if (h.status >= 30)
          s_color = 0xFF00D7FF; // Gold (Priority)
        else if (h.status >= 20)
          s_color = 0xFF5050FF; // Red/Orange (Avoid)
        else if (h.status >= 10)
          s_color = 0xFF777777; // Dark Gray (Not Needed)

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s_color), "%s",
                           status.c_str());
      }
      if (custom_font)
        ImGui::PopFont();
      ImGui::EndTable();
    }
  }
  ImGui::End();
}
