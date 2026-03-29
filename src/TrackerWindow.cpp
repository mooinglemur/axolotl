#include "TrackerWindow.h"
#include "Config.h"
#include <algorithm>
#include <imgui.h>

TrackerWindow::TrackerWindow(ArchipelagoNetwork &ap_network,
                             const ConnectionSettings &settings)
    : Window("Tracker"), ap_network_(ap_network), settings_(settings) {
  SetOpen(false);
}

void TrackerWindow::Render(std::tm *current_tm, ImFont *custom_font,
                           ImFont *preview_font,
                           ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    if (!ap_network_.IsAnySessionActive()) {
      ImGui::TextColored(ImVec4(1, 1, 0, 1), "Waiting for connection...");
    } else if (!ap_network_.IsDataPackageReceived()) {
      ImGui::Text("Loading Archipelago Data Package...");
    } else {
      ImGui::Text("Filter:");
      ImGui::SameLine();
      ImGui::PushItemWidth(-1.0f);
      RenderFilterInput("##Filter", filter_text_, focus_filter_);
      ImGui::PopItemWidth();
      ImGui::Separator();

      // Case-insensitive substring filter
      std::string l_filter = filter_text_;
      std::transform(l_filter.begin(), l_filter.end(), l_filter.begin(),
                     ::tolower);

      auto matches_filter = [&](const std::string &text) {
        if (l_filter.empty())
          return true;
        std::string l_text = text;
        std::transform(l_text.begin(), l_text.end(), l_text.begin(), ::tolower);
        return l_text.find(l_filter) != std::string::npos;
      };

      if (ImGui::BeginTabBar("TrackerTabs")) {
        for (const auto &session : ap_network_.GetSessions()) {
          if (!session->IsConnected())
            continue;

          std::string tab_name = session->GetName();
          if (ImGui::BeginTabItem(tab_name.c_str())) {
            int slot = session->GetLocalSlot();
            const auto &checked_ids = session->GetCheckedLocations();
            const auto &missing_ids = session->GetMissingLocations();
            int global_slot_id = (session->GetTeam() << 16) | slot;
            std::string game = session->ResolvePlayerGame(global_slot_id);

            std::vector<std::string> unchecked_names;
            std::vector<std::string> checked_names;

            for (int64_t id : missing_ids) {
              std::string name = session->ResolveLocationName(id, global_slot_id);
              if (!name.empty())
                unchecked_names.push_back(name);
            }
            for (int64_t id : checked_ids) {
              std::string name = session->ResolveLocationName(id, global_slot_id);
              if (!name.empty())
                checked_names.push_back(name);
            }

            // Natural sort
            std::sort(unchecked_names.begin(), unchecked_names.end(),
                      [](const std::string &a, const std::string &b) {
                        return NaturalCompare(a, b) < 0;
                      });
            std::sort(checked_names.begin(), checked_names.end(),
                      [](const std::string &a, const std::string &b) {
                        return NaturalCompare(a, b) < 0;
                      });

            ImGui::Text("Game: %s", game.c_str());
            ImGui::Separator();

            if (ImGui::BeginChild("LocationsChild")) {
              if (ImGui::CollapsingHeader("Unchecked Locations",
                                           ImGuiTreeNodeFlags_DefaultOpen)) {
                if (custom_font)
                  ImGui::PushFont(custom_font);
                for (const auto &name : unchecked_names) {
                  if (matches_filter(name)) {
                    ImGui::BulletText("%s", name.c_str());
                  }
                }
                if (custom_font)
                  ImGui::PopFont();
              }

              if (ImGui::CollapsingHeader("Checked Locations")) {
                if (custom_font)
                  ImGui::PushFont(custom_font);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                for (const auto &name : checked_names) {
                  if (matches_filter(name)) {
                    ImGui::BulletText("%s", name.c_str());
                  }
                }
                ImGui::PopStyleColor();
                if (custom_font)
                  ImGui::PopFont();
              }
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
          }
        }
        ImGui::EndTabBar();
      }
    }
  }
  ImGui::End();
}
