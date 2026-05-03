#pragma once
#include "Config.h"
#include "Window.h"
#include <chrono>
#include <map>
#include <string>
#include <vector>

class Application;

// Standalone "Profiles" management modal. Launched from the main menu via
// File → Profiles..., or by setting `is_open_ = true` programmatically.
//
// Lets the user list, fork, switch to, and delete profiles. Switching saves
// current state, releases the lock, and re-execs into the chosen profile —
// the calling instance does not return.
class ProfilesWindow : public Window {
public:
  explicit ProfilesWindow(Application &app,
                          const std::string &name = "Profiles");
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  Application &app_;
  int selected_idx_ = -1;
  // Buffer for the "new profile name" text input.
  char new_name_buf_[80] = {0};
  std::string status_message_; // last operation feedback
  std::string pending_delete_; // name awaiting confirmation

  // Cached profile list and "in use by another instance" map. Refreshed at
  // ~1Hz from the render loop; force-refreshed after any action that
  // mutates the profiles dir (Create / Delete).
  std::vector<ProfileInfo> profiles_cache_;
  std::map<std::string, bool> locked_cache_;
  std::chrono::steady_clock::time_point last_refresh_{};
  bool force_refresh_ = true; // populate on first render
  void RefreshIfStale();
};
