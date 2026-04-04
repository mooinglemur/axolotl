#include "TrackerWindow.h"
#include "Application.h"
#include "Config.h"
#include "LogicManager.h"
#include "PackStore.h"
#include "Platform.h"
#include <algorithm>
#include <imgui.h>

TrackerWindow::TrackerWindow(ArchipelagoNetwork &ap_network,
                             const ConnectionSettings &settings,
                             Application &app)
    : Window("Tracker"), ap_network_(ap_network), settings_(settings),
      app_(app) {}

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
            int global_slot_id = (session->GetTeam() << 16) | slot;
            const auto &checked_ids = session->GetCheckedLocations();
            const auto &missing_ids = session->GetMissingLocations();

            SessionCache &cache = session_caches_[tab_name];
            if (cache.data_version != ap_network_.GetDataVersion()) {
              cache.data_version = ap_network_.GetDataVersion();
              cache.game = session->ResolvePlayerGame(global_slot_id);
              cache.unchecked.clear();
              cache.checked_names.clear();

              for (int64_t id : missing_ids) {
                std::string name =
                    session->ResolveLocationName(id, global_slot_id);
                if (!name.empty())
                  cache.unchecked.push_back({id, name});
              }
              for (int64_t id : checked_ids) {
                std::string name =
                    session->ResolveLocationName(id, global_slot_id);
                if (!name.empty())
                  cache.checked_names.push_back(name);
              }

              // Natural sort
              std::sort(cache.unchecked.begin(), cache.unchecked.end(),
                        [](const LocationInfo &a, const LocationInfo &b) {
                          return NaturalCompare(a.name, b.name) < 0;
                        });
              std::sort(cache.checked_names.begin(), cache.checked_names.end(),
                        [](const std::string &a, const std::string &b) {
                          return NaturalCompare(a, b) < 0;
                        });
            }

            ImGui::Text("Game: %s", cache.game.c_str());
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "  Hint Points: %d (Cost: %d)",
                session->GetHintPoints(), session->GetHintCost());
            ImGui::Separator();

            if (ImGui::BeginChild("LocationsChild")) {
              if (ImGui::CollapsingHeader("Logical Progression",
                                          ImGuiTreeNodeFlags_DefaultOpen)) {
                if (cache.game.empty()) {
                  ImGui::TextDisabled("Waiting for game metadata...");
                } else if (!PackStore::HasPack(cache.game)) {
                  ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                                     "No PopTracker pack found for %s",
                                     cache.game.c_str());
                  if (ImGui::Button("Import PopTracker Pack")) {
                    std::string path = Platform::PickOpenFileName(
                        "PopTracker Pack (*.zip)\0*.zip\0All Files "
                        "(*.*)\0*.*\0");
                    if (!path.empty()) {
                      if (PackStore::ImportPack(cache.game, path)) {
                        app_.GetLogic().LoadPack(cache.game);
                      }
                    }
                  }
                  ImGui::SameLine();
                  ImGui::TextColored(ImVec4(1, 0, 0, 1),
                                     "EXPERIMENTAL FEATURE");
                } else {
                  if (app_.GetLogic().GetCurrentGame() != cache.game) {
                    app_.GetLogic().LoadPack(cache.game);
                  }
                  // Push current session data to LogicManager
                  app_.GetLogic().UpdateLogic(session->GetReceivedItemCounts(),
                                              session->GetSlotData(),
                                              session->GetCheckedLocations(),
                                              session->GetMissingLocations(),
                                              (int)session->GetLocalSlot());

                  const auto &accessible = app_.GetLogic().GetLocations();
                  if (accessible.empty()) {
                    ImGui::TextDisabled(
                        "No locations currently accessible in logic.");
                  } else {
                    ImGui::Text("%zu locations accessible", accessible.size());
                    if (custom_font)
                      ImGui::PushFont(custom_font);
                    for (const auto &loc : accessible) {
                      if (matches_filter(loc.name)) {
                        int access = loc.accessibility;
                        if (access == 2) {
                          ImGui::PushStyleColor(ImGuiCol_Text,
                                                ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                        } else if (access == 1) {
                          ImGui::PushStyleColor(ImGuiCol_Text,
                                                ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                        } else {
                          ImGui::PushStyleColor(ImGuiCol_Text,
                                                ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        }
                        ImGui::BulletText("%s", loc.name.c_str());
                        ImGui::PopStyleColor();
                      }
                    }
                    if (custom_font)
                      ImGui::PopFont();
                  }
                }
              }

              if (ImGui::CollapsingHeader("Unchecked Locations",
                                          ImGuiTreeNodeFlags_DefaultOpen)) {
                if (custom_font)
                  ImGui::PushFont(custom_font);
                for (const auto &loc : cache.unchecked) {
                  if (matches_filter(loc.name)) {
                    int access = app_.GetLogic().GetAccessibility(loc.id);

                    if (access == 2) {
                      ImGui::PushStyleColor(ImGuiCol_Text,
                                            ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                    } else if (access == 1) {
                      ImGui::PushStyleColor(ImGuiCol_Text,
                                            ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                    } else {
                      ImGui::PushStyleColor(ImGuiCol_Text,
                                            ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    }

                    ImGui::BulletText("%s", loc.name.c_str());
                    ImGui::PopStyleColor();
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
                for (const auto &name : cache.checked_names) {
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
