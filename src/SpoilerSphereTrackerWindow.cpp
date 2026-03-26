#include "SpoilerSphereTrackerWindow.h"
#include "Config.h"
#include "Platform.h"
#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <set>

SpoilerSphereTrackerWindow::SpoilerSphereTrackerWindow(
    ArchipelagoNetwork &ap_network, const ConnectionSettings &settings)
    : Window("Spoiler Sphere Tracker"), ap_network_(ap_network),
      settings_(settings) {
  SetOpen(false);
}

void SpoilerSphereTrackerWindow::LoadSpoilerLog() {
  std::string path = Platform::PickOpenFileName("*.txt");
  if (!path.empty()) {
    log_path_ = path;

    // Get all player names from all active sessions
    std::vector<std::string> player_names;
    for (const auto &session : ap_network_.GetSessions()) {
      if (session->IsConnected()) {
        const auto &names = session->GetPlayerNames();
        for (const auto &[id, name] : names) {
          player_names.push_back(name);
        }
      }
    }
    // Deduplicate
    std::sort(player_names.begin(), player_names.end());
    player_names.erase(std::unique(player_names.begin(), player_names.end()),
                       player_names.end());

    log_ = SpoilerLog::Parse(path, player_names);
  }
}

void SpoilerSphereTrackerWindow::Render(std::tm *current_tm,
                                        ImFont *custom_font,
                                        ImFont *preview_font,
                                        ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    if (log_path_.empty()) {
      if (ImGui::Button("Load spoiler log")) {
        LoadSpoilerLog();
      }
    } else {
      if (ImGui::Button("Clear")) {
        log_path_.clear();
        log_ = SpoilerLog();
        last_data_version_ = 0;
      }
      ImGui::SameLine();
      ImGui::Text("File: %s",
                  std::filesystem::path(log_path_).filename().string().c_str());
    }

    ImGui::Separator();

    if (!log_path_.empty() &&
        ap_network_.GetDataVersion() != last_data_version_) {
      last_data_version_ = ap_network_.GetDataVersion();

      // Collect latest player names
      std::vector<std::string> player_names;
      for (const auto &session : ap_network_.GetSessions()) {
        if (session->IsConnected()) {
          for (const auto &[id, name] : session->GetPlayerNames()) {
            player_names.push_back(name);
          }
        }
      }
      std::sort(player_names.begin(), player_names.end());
      player_names.erase(std::unique(player_names.begin(), player_names.end()),
                         player_names.end());

      log_ = SpoilerLog::Parse(log_path_, player_names);
    }

    if (!log_.IsLoaded()) {
      ImGui::Text("No spoiler log loaded or parsing failed.");
    } else if (!ap_network_.IsAnySessionActive()) {
      ImGui::TextColored(ImVec4(1, 1, 0, 1),
                         "Waiting for connection to slots...");
    } else if (!ap_network_.IsDataPackageReceived()) {
      ImGui::Text("Loading Archipelago Data Package...");
    } else {
      // Logic to find current sphere
      const auto &my_slots = ap_network_.GetConnectedSlots();

      std::set<std::string> my_player_names;
      for (int slot : my_slots) {
        my_player_names.insert(ap_network_.ResolvePlayerName(slot));
      }

      // Get checked locations per session/slot
      std::map<int, std::set<int64_t>> slot_checked;
      for (const auto &session : ap_network_.GetSessions()) {
        if (session->IsConnected()) {
          int s_slot = session->GetLocalSlot() | (session->GetTeam() << 16);
          slot_checked[s_slot] = session->GetCheckedLocations();
          // Also map the bare slot for team 0
          if (session->GetTeam() == 0) {
            slot_checked[session->GetLocalSlot()] =
                session->GetCheckedLocations();
          }
        }
      }

      const auto &spheres = log_.GetSpheres();

      // Map to store state for each connected slot
      struct SlotProgression {
        int current_sphere =
            0; // The sphere we are reporting (possibly the source of blockage)
        int locations_sphere = 0; // The sphere where the locations come from
        bool blocked = false;
        std::vector<int> available_location_indices;
        std::string blocking_item = "";
      };
      std::map<int, SlotProgression> progression;

      for (int slot : my_slots) {
        std::string playerName = ap_network_.ResolvePlayerName(slot);
        SlotProgression &prog = progression[slot];

        // Track items received for this slot (counts)
        std::map<std::string, int> received_item_counts;
        auto session = ap_network_.GetSessionBySlot(slot);
        if (session) {
          for (const auto &msg : session->GetReceivedItems()) {
            std::string itemName =
                session->ResolveItemName(msg.item_id, msg.receiver_slot);
            received_item_counts[itemName]++;
          }
        }

        std::map<std::string, int> required_counts;
        for (int i = 0; i < (int)spheres.size(); ++i) {
          const auto &sphere = spheres[i];

          // 1. Verify we have all requirements from PREVIOUS spheres to be in
          // Sphere i
          for (auto const &[name, count] : required_counts) {
            if (received_item_counts[name] < count) {
              if (!prog.blocked) {
                prog.blocked = true;
                prog.blocking_item = name;

                // Find the sphere k < i that added the requirement we are
                // missing
                std::map<std::string, int> trace_counts;
                for (int k = 0; k < i; ++k) {
                  for (const auto &item : spheres[k].items)
                    if (item.player == playerName)
                      trace_counts[item.name]++;
                  for (const auto &loc : spheres[k].locations)
                    if (loc.item.player == playerName)
                      trace_counts[loc.item.name]++;
                  if (trace_counts[name] > received_item_counts[name]) {
                    prog.current_sphere = k;
                    break;
                  }
                }
              }
              // DO NOT goto next_slot yet! We want to find the first sphere
              // with work.
              break;
            }
          }

          // 2. Check for unchecked locations in current sphere
          bool all_my_checked = true;
          std::vector<int> unchecked_indices;
          for (int j = 0; j < (int)sphere.locations.size(); ++j) {
            const auto &loc = sphere.locations[j];
            if (loc.player == playerName) {
              int64_t lid = ap_network_.ResolveLocationID(loc.name, slot);
              if (lid == -1 || !slot_checked[slot].count(lid)) {
                all_my_checked = false;
                unchecked_indices.push_back(j);
              }
            }
          }

          if (!all_my_checked) {
            // We found the first sphere with unchecked locations!
            prog.locations_sphere = i;
            prog.available_location_indices = unchecked_indices;
            if (!prog.blocked) {
              prog.current_sphere = i;
            }

            // Natural sort the locations for better display
            std::sort(prog.available_location_indices.begin(),
                      prog.available_location_indices.end(), [&](int a, int b) {
                        return NaturalCompare(sphere.locations[a].name,
                                              sphere.locations[b].name) < 0;
                      });

            goto next_slot;
          }

          // 3. Current sphere is fully checked, add its items to requirements
          // for next sphere
          for (const auto &item : sphere.items)
            if (item.player == playerName)
              required_counts[item.name]++;
          for (const auto &loc : sphere.locations)
            if (loc.item.player == playerName)
              required_counts[loc.item.name]++;
        }
        prog.current_sphere = -1; // Finished
      next_slot:;
      }

      // Display progression for each slot (order by login interface)
      std::set<int> seen_slots;
      for (const auto &slot_setting : settings_.slots) {
        auto session = ap_network_.GetSession(slot_setting.name);
        if (!session || !session->IsConnected())
          continue;
        int slot = (session->GetTeam() << 16) | session->GetLocalSlot();
        if (!progression.count(slot) || seen_slots.count(slot))
          continue;
        seen_slots.insert(slot);

        std::string playerName = ap_network_.ResolvePlayerName(slot);
        const auto &prog = progression[slot];

        ImGui::PushID(slot);
        if (ImGui::CollapsingHeader(playerName.c_str(),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          bool goal_reached = false;
          auto session = ap_network_.GetSessionBySlot(slot);
          if (session && prog.current_sphere >= 0 &&
              (size_t)prog.current_sphere < (size_t)spheres.size()) {
            const auto &current_sphere_data = spheres[prog.current_sphere];
            for (const auto &loc : current_sphere_data.locations) {
              if (loc.item.name == "Victory" && loc.player == playerName) {
                goal_reached = true;
                break;
              }
            }
          }

          if (goal_reached) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Final sphere: %d",
                               prog.current_sphere);
            ImGui::TextDisabled("Victory is in logic.");
          } else if (prog.current_sphere == -1) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1),
                               "No progression found for this slot!");
          } else {
            if (prog.blocked) {
              ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                                 "Logical BK after completing Sphere: %d",
                                 prog.current_sphere);
              ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                                 "You are now at or ahead of logic.");
              ImGui::Text("Current Sphere: %d", prog.locations_sphere);
            } else {
              ImGui::Text("Current Sphere: %d", prog.current_sphere);
            }

            for (int idx : prog.available_location_indices) {
              const auto &loc = spheres[prog.locations_sphere].locations[idx];
              ImGui::BulletText("%s", loc.name.c_str());
            }
          }
        }
        ImGui::PopID();
      }

      if (settings_.show_details_in_sphere_tracker) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Raw Sphere View")) {
          for (const auto &sphere : spheres) {
            if (ImGui::TreeNode((void *)(intptr_t)sphere.number, "Sphere %d",
                                sphere.number)) {
              if (sphere.number == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                for (const auto &item : sphere.items) {
                  ImGui::BulletText("%s (%s)", item.name.c_str(),
                                    item.player.c_str());
                }
                ImGui::PopStyleColor();
              } else {
                // Group by player
                std::map<std::string, std::vector<const SpoilerLocation *>>
                    grouped;
                for (const auto &loc : sphere.locations) {
                  grouped[loc.player].push_back(&loc);
                }

                // Collect and sort player names
                std::vector<std::string> players;
                for (auto const &[name, locs] : grouped)
                  players.push_back(name);
                std::sort(players.begin(), players.end());

                for (const auto &playerName : players) {
                  auto &locs = grouped[playerName];
                  // Natural sort check names
                  std::sort(
                      locs.begin(), locs.end(),
                      [](const SpoilerLocation *a, const SpoilerLocation *b) {
                        return NaturalCompare(a->name, b->name) < 0;
                      });

                  // Check if this player is connected
                  bool player_connected = my_player_names.count(playerName);

                  for (const auto *loc_ptr : locs) {
                    const auto &loc = *loc_ptr;
                    bool checked = false;

                    // Resolve player ID for this location's owner
                    int loc_player_id = -1;
                    for (const auto &s : ap_network_.GetSessions()) {
                      if (s->IsConnected()) {
                        for (auto const &[id, name] : s->GetPlayerNames()) {
                          if (name == loc.player) {
                            loc_player_id = id;
                            break;
                          }
                        }
                      }
                      if (loc_player_id != -1)
                        break;
                    }

                    if (loc_player_id != -1) {
                      int64_t lid = ap_network_.ResolveLocationID(
                          loc.name, loc_player_id);
                      if (lid != -1 && slot_checked.count(loc_player_id) &&
                          slot_checked[loc_player_id].count(lid)) {
                        checked = true;
                      }
                    }

                    if (checked || !player_connected)
                      ImGui::PushStyleColor(ImGuiCol_Text,
                                            ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::BulletText("%s (%s) -> %s (%s)", loc.name.c_str(),
                                      loc.player.c_str(), loc.item.name.c_str(),
                                      loc.item.player.c_str());
                    if (checked || !player_connected)
                      ImGui::PopStyleColor();
                  }
                }
              }
              ImGui::TreePop();
            }
          }
        }
      }
    }
  }
  ImGui::End();
}
