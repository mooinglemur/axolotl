#include "OverviewWindow.h"
#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <string>
#include <vector>

namespace {
ImVec4 GetProgressColor(float progress) {
  if (progress >= 1.0f) {
    return ImVec4(0.2f, 0.6f, 0.2f, 1.0f);
  }
  return ImVec4(0.6f - 0.4f * progress, 0.2f + 0.3f * progress, 0.2f, 1.0f);
}
} // namespace

OverviewWindow::OverviewWindow(ArchipelagoNetwork &ap_network,
                               ConnectionSettings &settings)
    : Window("Overview"), ap_network_(ap_network), settings_(settings) {
  std::memset(tracker_url_buf_, 0, sizeof(tracker_url_buf_));
  const std::string &url = ap_network_.GetTrackerUrl();
  if (!url.empty()) {
    std::strncpy(tracker_url_buf_, url.c_str(), sizeof(tracker_url_buf_) - 1);
  }
}

void OverviewWindow::Render(std::tm *current_tm, ImFont *custom_font,
                            ImFont *preview_font,
                            ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  // Tracker URL might have been cleared externally (e.g., server change)
  const std::string &live_url = ap_network_.GetTrackerUrl();
  if (live_url != last_settings_tracker_url_) {
    std::strncpy(tracker_url_buf_, live_url.c_str(),
                 sizeof(tracker_url_buf_) - 1);
    last_settings_tracker_url_ = live_url;
  }

  ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(name_.c_str(), &is_open_,
                   ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    const auto &stats = ap_network_.GetGlobalStats();

    int fully_completed = 0;
    int complete_without_goal = 0;
    int goal_without_complete = 0;

    for (const auto &[slot_id, s_stats] : stats->slot_info) {
      bool is_goal = stats->completed_slots.count(slot_id) > 0;
      bool is_100_percent =
          (s_stats.total_locations > 0 &&
           s_stats.checked_locations >= s_stats.total_locations);

      if (is_goal && is_100_percent) {
        fully_completed++;
      } else if (!is_goal && is_100_percent) {
        complete_without_goal++;
      } else if (is_goal && !is_100_percent) {
        goal_without_complete++;
      }
    }

    if (ImGui::CollapsingHeader("Multiworld Status###GlobalProgress",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      if (custom_font)
        ImGui::PushFont(custom_font);

      // Locations Checked
      float progress =
          (stats->total_locations > 0)
              ? (float)stats->checked_locations / stats->total_locations
              : 0.0f;

      char overlay[64];
      snprintf(overlay, sizeof(overlay), "%.1f%% (%d/%d)", progress * 100.0f,
               stats->checked_locations, stats->total_locations);

      ImVec2 pos = ImGui::GetCursorScreenPos();
      ImVec2 bar_size =
          ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());

      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, GetProgressColor(progress));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

      ImGui::ProgressBar(progress, ImVec2(-1, 0), "");

      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);

      ImVec2 text_size = ImGui::CalcTextSize(overlay);
      ImVec2 text_pos = ImVec2(pos.x + (bar_size.x - text_size.x) * 0.5f,
                               pos.y + (bar_size.y - text_size.y) * 0.5f);
      ImGui::GetWindowDrawList()->AddText(text_pos, IM_COL32_WHITE, overlay);

      ImGui::Spacing();
      ImGui::BulletText("Completion: %d/%d", fully_completed,
                        stats->total_games);
      if (complete_without_goal > 0) {
        ImGui::BulletText("Complete without goal: %d", complete_without_goal);
      }
      if (goal_without_complete > 0) {
        ImGui::BulletText("At goal but incomplete: %d", goal_without_complete);
      }

      if (custom_font)
        ImGui::PopFont();
    }

    double current_sync_time = ap_network_.GetLastTrackerSyncTime();
    if (current_sync_time > last_seen_sync_time_) {
      sync_triggered_ = false;
      last_seen_sync_time_ = current_sync_time;
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Player Progression",
                                ImGuiTreeNodeFlags_DefaultOpen)) {

      ImGui::Text("Filter:");
      ImGui::SameLine();

      float checkbox_w = ImGui::GetFrameHeight() +
                         ImGui::GetStyle().ItemInnerSpacing.x +
                         ImGui::CalcTextSize("Exclude Goal+Complete").x +
                         ImGui::GetStyle().ItemSpacing.x * 2.0f;
      ImGui::PushItemWidth(-checkbox_w);
      RenderFilterInput("##Filter", filter_text_, focus_filter_);
      ImGui::PopItemWidth();

      ImGui::SameLine();
      ImGui::Checkbox("Exclude Goal+Complete", &exclude_goal_complete_);

      int row_bg_flag =
          settings_.shade_alternating_rows ? ImGuiTableFlags_RowBg : 0;

      if (ImGui::BeginTable(
              "PlayerStats", 3,
              ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                  ImGuiTableFlags_BordersOuter | row_bg_flag)) {
        ImGui::TableSetupColumn("Player/Slot",
                                ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed,
                                150.0f);
        ImGui::TableSetupColumn("Last Activity",
                                ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        struct SortEntry {
          int slot_id;
          std::string name;
          float progress;
          double last_activity;
          int checked;
          int total;
          bool is_goal;
        };
        std::string l_filter = filter_text_;
        std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                       ::tolower);

        std::vector<SortEntry> entries;
        for (const auto &[slot_id, s_stats] : stats->slot_info) {
          std::string name = ap_network_.ResolvePlayerName(slot_id);

          if (!l_filter.empty()) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(),
                           name_lower.begin(), ::tolower);
            if (name_lower.find(l_filter) == std::string::npos)
              continue;
          }

          bool is_goal = stats->completed_slots.count(slot_id) > 0;
          bool is_100_percent =
              (s_stats.total_locations > 0 &&
               s_stats.checked_locations == s_stats.total_locations);

          if (exclude_goal_complete_ && is_goal && is_100_percent) {
            continue;
          }

          SortEntry e;
          e.slot_id = slot_id;
          e.name = name;
          e.checked = s_stats.checked_locations;
          e.total = s_stats.total_locations;
          e.progress = (e.total > 0) ? (float)e.checked / e.total : 0.0f;
          e.last_activity = s_stats.last_activity_time;
          e.is_goal = is_goal;
          entries.push_back(e);
        }
        if (ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs()) {
          if (sort_specs->SpecsCount > 0) {
            std::sort(entries.begin(), entries.end(),
                      [&](const SortEntry &a, const SortEntry &b) {
                        for (int i = 0; i < sort_specs->SpecsCount; i++) {
                          const ImGuiTableColumnSortSpecs *spec =
                              &sort_specs->Specs[i];
                          int delta = 0;
                          if (spec->ColumnIndex == 0)
                            delta = a.name.compare(b.name);
                          else if (spec->ColumnIndex == 1)
                            delta = (a.progress < b.progress)   ? -1
                                    : (a.progress > b.progress) ? 1
                                                                : 0;
                          else if (spec->ColumnIndex == 2)
                            delta = (a.last_activity > b.last_activity)   ? -1
                                    : (a.last_activity < b.last_activity) ? 1
                                                                          : 0;

                          if (delta != 0) {
                            return (spec->SortDirection ==
                                    ImGuiSortDirection_Ascending)
                                       ? (delta < 0)
                                       : (delta > 0);
                          }
                        }
                        return a.slot_id < b.slot_id;
                      });
          }
        }

        const std::set<int> &my_slots = ap_network_.GetConnectedSlots();

        if (custom_font)
          ImGui::PushFont(custom_font);
        for (const auto &e : entries) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (e.is_goal) {
            ImGui::TextColored(ImColor(0, 255, 0), "%s", e.name.c_str());
          } else if (my_slots.count(e.slot_id) > 0) {
            ImGui::TextColored(ImColor(255, 0, 255), "%s", e.name.c_str());
          } else {
            ImGui::Text("%s", e.name.c_str());
          }

          if (ImGui::IsItemHovered()) {
            std::string game = ap_network_.ResolvePlayerGame(e.slot_id);
            if (!game.empty()) {
              ImGui::SetTooltip("Game: %s", game.c_str());
            }
          }

          ImGui::TableSetColumnIndex(1);
          char overlay[64];
          snprintf(overlay, sizeof(overlay), "%.1f%% (%d/%d)",
                   e.progress * 100.0f, e.checked, e.total);

          ImVec2 pos = ImGui::GetCursorScreenPos();
          float width = ImGui::GetContentRegionAvail().x;
          float height = ImGui::GetFrameHeight();

          ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                GetProgressColor(e.progress));
          ImGui::PushStyleColor(ImGuiCol_Border,
                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
          ImGui::ProgressBar(e.progress, ImVec2(-FLT_MIN, 0), "");
          ImGui::PopStyleVar();
          ImGui::PopStyleColor(2);

          ImVec2 text_size = ImGui::CalcTextSize(overlay);
          ImVec2 text_pos = ImVec2(pos.x + (width - text_size.x) * 0.5f,
                                   pos.y + (height - text_size.y) * 0.5f);
          ImGui::GetWindowDrawList()->AddText(text_pos, IM_COL32_WHITE,
                                              overlay);

          ImGui::TableSetColumnIndex(2);

          char delta_str[32];
          bool is_disabled = false;

          if (e.last_activity > 0) {
            double now = ArchipelagoNetwork::GetCurrentTimestamp();
            double delta = std::max(0.0, now - e.last_activity);

            if (delta < 1.0) {
              snprintf(delta_str, sizeof(delta_str), "now");
            } else if (delta < 60) {
              snprintf(delta_str, sizeof(delta_str), "%ds", (int)delta);
            } else if (delta < 3600) {
              snprintf(delta_str, sizeof(delta_str), "%dm%ds", (int)delta / 60,
                       (int)delta % 60);
            } else if (delta < 86400) {
              snprintf(delta_str, sizeof(delta_str), "%dh%dm",
                       (int)delta / 3600, ((int)delta % 3600) / 60);
            } else {
              snprintf(delta_str, sizeof(delta_str), "%dd%dh",
                       (int)delta / 86400, ((int)delta % 86400) / 3600);
            }
          } else {
            snprintf(delta_str, sizeof(delta_str), "N/A");
            is_disabled = true;
          }

          float text_width = ImGui::CalcTextSize(delta_str).x;
          float avail_width = ImGui::GetContentRegionAvail().x;
          if (avail_width > text_width) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_width -
                                 text_width);
          }

          if (is_disabled) {
            ImGui::TextDisabled("%s", delta_str);
          } else {
            ImGui::Text("%s", delta_str);
          }
        }
        if (custom_font)
          ImGui::PopFont();
        ImGui::EndTable();
      }
    }

    ImGui::Separator();

    // Tracker URL
    if (ImGui::CollapsingHeader("Tracker Settings")) {

      ImGui::Text("Tracker URL:");
      if (ImGui::InputText("##TrackerURL", tracker_url_buf_,
                           sizeof(tracker_url_buf_))) {
        sync_triggered_ = false;
      }
      ImGui::SameLine();
      if (ImGui::Button("Save & Sync")) {
        ap_network_.SetTrackerUrl(tracker_url_buf_);
        sync_triggered_ = true;
      }

      if (sync_triggered_ && !ap_network_.GetTrackerUrl().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Syncing...");
      }

      if (ap_network_.GetTrackerUrl().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
        ImGui::TextWrapped(
            "Enter an Archipelago tracker URL to see detailed global stats.");
        ImGui::PopStyleColor();
      }

      ImGui::Spacing();
      if (ImGui::Button("Reset Stats", ImVec2(-1, 0))) {
        ap_network_.ClearGlobalStats();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Resets check totals and removes player slots that\n"
            "don't exist in the current multiworld, then reloads\n"
            "stats from the tracker URL.  This should not be needed\n"
            "during normal use, but is provided to solve unforeseen\n"
            "desynchronization issues.");
      }
    }
  }
  ImGui::End();
}
