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
                        std::string("PopTracker Pack (*.zip)\0*.zip\0All Files "
                                    "(*.*)\0*.*\0",
                                    51));
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
                  // Push current session data to LogicManager atomically
                  {
                    std::lock_guard<std::recursive_mutex> lock(ap_network_.GetStateMutex());
                    app_.GetLogic().UpdateLogic(session->GetReceivedItemCounts(),
                                                session->GetSlotData(),
                                                session->GetCheckedLocations(),
                                                session->GetMissingLocations(),
                                                (int)session->GetLocalSlot());
                  }

                  const auto &accessible = app_.GetLogic().GetLocations();
                  if (accessible.empty()) {
                    ImGui::TextDisabled(
                        "No locations currently accessible in logic.");
                  } else {
                    // Build grouped structure: section -> sorted location entries.
                    // Only actual location checks (id > 0) are grouped; region
                    // entries (id <= 0) are excluded. Sections with no checks
                    // are not displayed.
                    struct LocEntry {
                      std::string label; // path[1..] joined with " > "
                      int accessibility;
                    };
                    // std::map gives free alphabetical section ordering.
                    std::map<std::string, std::vector<LocEntry>> groups;

                    for (const auto &loc : accessible) {
                      if (loc.id <= 0)
                        continue; // skip region-only entries
                      if (!matches_filter(loc.name))
                        continue;

                      std::string section;
                      std::string label;
                      if (loc.path.size() >= 2) {
                        section = loc.path[0];
                        label = loc.path[1];
                        for (size_t i = 2; i < loc.path.size(); ++i)
                          label += " > " + loc.path[i];
                      } else {
                        // Fallback: no parent segment — place in "Global"
                        section = "Global";
                        label = loc.path.empty() ? loc.name : loc.path[0];
                      }
                      groups[section].push_back({std::move(label), loc.accessibility});
                    }

                    // Sort within each section: Normal (2) first, then
                    // Sequence Break (1); alphabetically within each tier.
                    for (auto &[sec, entries] : groups) {
                      std::sort(entries.begin(), entries.end(),
                                [](const LocEntry &a, const LocEntry &b) {
                                  if (a.accessibility != b.accessibility)
                                    return a.accessibility > b.accessibility;
                                  return a.label < b.label;
                                });
                    }

                    // Count visible location checks for the header line.
                    size_t total = 0;
                    for (const auto &[sec, entries] : groups)
                      total += entries.size();
                    ImGui::Text("%zu locations accessible", total);

                    if (custom_font)
                      ImGui::PushFont(custom_font);

                    auto &open_states = cache.section_open_states;

                    for (auto &[section, entries] : groups) {
                      if (entries.empty())
                        continue;

                      bool has_normal = false;
                      for (const auto &e : entries)
                        if (e.accessibility == 2) { has_normal = true; break; }

                      // Tinted header: dark green for Normal, dark yellow for
                      // Sequence Break only.
                      ImVec4 hdr_col  = has_normal
                          ? ImVec4(0.0f,  0.35f, 0.0f,  1.0f)
                          : ImVec4(0.35f, 0.35f, 0.0f,  1.0f);
                      ImVec4 hdr_hov  = has_normal
                          ? ImVec4(0.0f,  0.45f, 0.0f,  1.0f)
                          : ImVec4(0.45f, 0.45f, 0.0f,  1.0f);
                      ImVec4 hdr_act  = has_normal
                          ? ImVec4(0.0f,  0.55f, 0.0f,  1.0f)
                          : ImVec4(0.55f, 0.55f, 0.0f,  1.0f);

                      ImGui::PushStyleColor(ImGuiCol_Header,        hdr_col);
                      ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  hdr_hov);
                      ImGui::PushStyleColor(ImGuiCol_HeaderActive,   hdr_act);

                      // Persist open state across disappear/reappear within
                      // this app session. New (unseen) sections start collapsed.
                      bool is_new = (open_states.find(section) == open_states.end());
                      if (is_new)
                        open_states[section] = false;
                      ImGui::SetNextItemOpen(open_states[section], ImGuiCond_Always);

                      bool node_open = ImGui::TreeNodeEx(
                          section.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                      open_states[section] = node_open;

                      ImGui::PopStyleColor(3);

                      if (node_open) {
                        for (const auto &e : entries) {
                          ImVec4 txt = (e.accessibility == 2)
                              ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)  // green
                              : ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // yellow
                          ImGui::PushStyleColor(ImGuiCol_Text, txt);
                          ImGui::BulletText("%s", e.label.c_str());
                          ImGui::PopStyleColor();
                        }
                        ImGui::TreePop();
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
