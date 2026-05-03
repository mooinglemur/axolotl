#include "ProfilesWindow.h"
#include "Application.h"
#include "Config.h"
#include "ProfileLock.h"
#include <chrono>
#include <ctime>
#include <imgui.h>

namespace {

std::string FormatLastUsed(const ProfileInfo &p) {
  if (!p.has_last_used)
    return "(never)";
  // Convert file_time_type → time_t via clock duration cast. portable across
  // libstdc++ (uses std::chrono::file_clock).
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      p.last_used - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm local_tm{};
#ifdef _WIN32
  localtime_s(&local_tm, &tt);
#else
  localtime_r(&tt, &local_tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local_tm);
  return std::string(buf);
}

} // namespace

ProfilesWindow::ProfilesWindow(Application &app, const std::string &name)
    : Window(name), app_(app) {}

void ProfilesWindow::RefreshIfStale() {
  using namespace std::chrono;
  auto now = steady_clock::now();
  if (!force_refresh_ &&
      duration_cast<milliseconds>(now - last_refresh_).count() < 1000)
    return;
  force_refresh_ = false;
  last_refresh_ = now;
  profiles_cache_ = Config::ListProfiles();
  locked_cache_.clear();
  const std::string &active = Config::GetActiveProfile();
  for (const auto &p : profiles_cache_) {
    if (p.name == active)
      continue; // never probe own lock
    locked_cache_[p.name] = ProfileLock::IsHeldByLiveProcess(p.name);
  }
}

void ProfilesWindow::Render(std::tm *, ImFont *, ImFont *, ImFont *) {
  if (!is_open_)
    return;

  ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::End();
    return;
  }

  const std::string &active = Config::GetActiveProfile();
  RefreshIfStale();
  const auto &profiles = profiles_cache_;

  ImGui::TextDisabled("Active profile: %s", active.c_str());
  ImGui::Separator();

  // Profile list table.
  if (ImGui::BeginTable("profiles", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable,
                        ImVec2(-1, 200))) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Last used", ImGuiTableColumnFlags_WidthFixed,
                            150.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                            100.0f);
    ImGui::TableHeadersRow();

    if (selected_idx_ >= (int)profiles.size())
      selected_idx_ = -1;

    for (int i = 0; i < (int)profiles.size(); ++i) {
      const auto &p = profiles[i];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      bool is_active = (p.name == active);
      bool selected = (selected_idx_ == i);
      ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
      if (ImGui::Selectable(p.name.c_str(), selected, flags))
        selected_idx_ = i;
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(FormatLastUsed(p).c_str());
      ImGui::TableSetColumnIndex(2);
      if (is_active)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "active");
      else {
        auto lk = locked_cache_.find(p.name);
        bool locked = (lk != locked_cache_.end()) && lk->second;
        if (locked)
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "in use");
        else
          ImGui::TextDisabled("idle");
      }
    }
    ImGui::EndTable();
  }

  // Action row: switch / delete
  bool has_selection = (selected_idx_ >= 0 && selected_idx_ < (int)profiles.size());
  std::string sel_name = has_selection ? profiles[selected_idx_].name : "";
  bool sel_is_active = has_selection && (sel_name == active);

  ImGui::BeginDisabled(!has_selection || sel_is_active);
  if (ImGui::Button("Switch to selected")) {
    app_.SwitchProfile(sel_name);
    // ReExec normally doesn't return; if it does, just close the modal.
    is_open_ = false;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  bool sel_locked = false;
  if (has_selection) {
    auto lk = locked_cache_.find(sel_name);
    sel_locked = (lk != locked_cache_.end()) && lk->second;
  }
  ImGui::BeginDisabled(!has_selection || sel_is_active || sel_locked);
  if (ImGui::Button("Delete...")) {
    pending_delete_ = sel_name;
    ImGui::OpenPopup("Delete profile?");
  }
  ImGui::EndDisabled();

  // Delete confirmation popup.
  if (ImGui::BeginPopupModal("Delete profile?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete profile '%s' and all of its files?",
                pending_delete_.c_str());
    ImGui::TextDisabled("This cannot be undone.");
    ImGui::Separator();
    if (ImGui::Button("Delete", ImVec2(120, 0))) {
      if (Config::DeleteProfile(pending_delete_)) {
        status_message_ = "Deleted profile '" + pending_delete_ + "'.";
        selected_idx_ = -1;
        force_refresh_ = true;
      } else {
        status_message_ =
            "Failed to delete profile '" + pending_delete_ + "'.";
      }
      pending_delete_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      pending_delete_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::Separator();
  ImGui::Text("Create new profile (forked from '%s'):", active.c_str());
  ImGui::SetNextItemWidth(-150.0f);
  ImGui::InputText("##NewProfileName", new_name_buf_, sizeof(new_name_buf_));
  ImGui::SameLine();
  std::string new_name = new_name_buf_;
  bool name_valid = Config::ValidateProfileName(new_name);
  // Check the cached list rather than stat'ing every frame.
  bool name_taken = false;
  if (name_valid) {
    for (const auto &p : profiles)
      if (p.name == new_name) {
        name_taken = true;
        break;
      }
  }
  ImGui::BeginDisabled(!name_valid || name_taken);
  if (ImGui::Button("Create")) {
    if (Config::CreateProfile(new_name, active)) {
      status_message_ = "Created profile '" + new_name +
                        "' (forked from " + active +
                        "). Switch to it to start using it.";
      new_name_buf_[0] = '\0';
      force_refresh_ = true;
    } else {
      status_message_ = "Failed to create profile '" + new_name + "'.";
    }
  }
  ImGui::EndDisabled();
  if (!name_valid && !new_name.empty()) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                       "Name must match [A-Za-z0-9_-] (1-64 chars).");
  } else if (name_taken) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "A profile with that name already exists.");
  }

  if (!status_message_.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", status_message_.c_str());
  }

  ImGui::End();
}
