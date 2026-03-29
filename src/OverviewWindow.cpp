#include "OverviewWindow.h"
#include <cstring>
#include <imgui.h>
#include <string>

OverviewWindow::OverviewWindow(ArchipelagoNetwork &ap_network,
                               ConnectionSettings &settings)
    : Window("Overview"), ap_network_(ap_network), settings_(settings) {
  std::memset(tracker_url_buf_, 0, sizeof(tracker_url_buf_));
  if (!settings_.tracker_url.empty()) {
    std::strncpy(tracker_url_buf_, settings_.tracker_url.c_str(),
                 sizeof(tracker_url_buf_) - 1);
  }
}

void OverviewWindow::Render(std::tm *current_tm, ImFont *custom_font,
                            ImFont *preview_font,
                            ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  // Tracker URL might have been cleared externally (e.g., server change)
  if (settings_.tracker_url != last_settings_tracker_url_) {
    std::strncpy(tracker_url_buf_, settings_.tracker_url.c_str(),
                 sizeof(tracker_url_buf_) - 1);
    last_settings_tracker_url_ = settings_.tracker_url;
  }

  // Tracker URL might have been cleared externally (e.g., server change)

  ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    const auto &stats = ap_network_.GetGlobalStats();

    ImGui::Text("Multiworld Status");
    ImGui::Separator();
    if (custom_font)
      ImGui::PushFont(custom_font);

    // Games Completed
    ImGui::Text("Games Completed:");
    ImGui::SameLine();
    ImGui::Text("%d / %d", (int)stats.completed_slots.size(),
                stats.total_games);

    // Locations Checked
    float progress =
        (stats.total_locations > 0)
            ? (float)stats.checked_locations / stats.total_locations
            : 0.0f;

    ImGui::Text("Global Progress:");

    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%.1f%% (%d/%d)", progress * 100.0f,
             stats.checked_locations, stats.total_locations);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 bar_size =
        ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                          ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    ImGui::ProgressBar(progress, ImVec2(-1, 0), "");

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImVec2 text_size = ImGui::CalcTextSize(overlay);
    ImVec2 text_pos = ImVec2(pos.x + (bar_size.x - text_size.x) * 0.5f,
                             pos.y + (bar_size.y - text_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(text_pos, IM_COL32_WHITE, overlay);

    if (custom_font)
      ImGui::PopFont();

    double current_sync_time = ap_network_.GetLastTrackerSyncTime();
    if (current_sync_time > last_seen_sync_time_) {
      sync_triggered_ = false;
      last_seen_sync_time_ = current_sync_time;
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
        settings_.tracker_url = tracker_url_buf_;
        ap_network_.ForceTrackerSync();
        sync_triggered_ = true;
      }

      if (sync_triggered_ && !settings_.tracker_url.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Syncing...");
      }

      if (settings_.tracker_url.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
        ImGui::TextWrapped(
            "Enter an Archipelago tracker URL to see detailed global stats.");
        ImGui::PopStyleColor();
      }
    }
  }
  ImGui::End();
}
