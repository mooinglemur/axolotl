#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct SlotSettings {
  std::string name = "Player1";
  std::string last_name = "Player1";
  std::string password = "";
  bool connect_on_launch = false;

  SlotSettings() = default;
  SlotSettings(const std::string &n, const std::string &p, bool col)
      : name(n), last_name(n), password(p), connect_on_launch(col) {}
};

struct ConnectionSettings {
  std::string server_url = "archipelago.gg:0";
  std::string tracker_url = "";
  std::vector<SlotSettings> slots;

  // UI Preferences
  float ui_scale = 1.0f;
  float content_scale = 1.0f;
  std::string font_path = "";
  std::string fallback_font_path = "";
  bool show_hints = true;
  bool show_details_in_sphere_tracker = true;
  int max_history_size = 0;
  std::string timestamp_format_long = "[%Y-%m-%d %H:%M:%S]";
  std::string timestamp_format_short = "[%H:%M:%S]";
  int window_width = 1280;
  int window_height = 720;
  int window_x = -1; // -1 means center on monitor
  int window_y = -1;
  bool collapse_received_items = true;
  bool streamer_mode = false;
  bool shade_alternating_rows = true;
  bool confirm_exit = true;
  bool show_chat_timestamps = true;
  bool show_feed_timestamps = true;
  bool show_deathlink_messages = true;
  bool show_deathlinks_in_personal_feed = false;
  bool hide_found_hints = false;
  std::string uuid = "";
  std::map<std::string, bool> show_windows;

  bool http_server_enabled = false;
  std::string http_server_bind_address = "127.0.0.1";
  int http_server_port = 3621;
};

struct ProfileInfo {
  std::string name;
  std::filesystem::file_time_type last_used; // mtime of .last_used; epoch if absent
  bool has_last_used = false;
};

class Config {
public:
  // Active profile is set once at startup before any other Config use.
  static void SetActiveProfile(const std::string &name);
  static const std::string &GetActiveProfile();

  // Returns <root>/profiles/<active>/. Creates if missing.
  static std::filesystem::path GetConfigDir();
  static std::filesystem::path GetConfigPath();
  static std::filesystem::path GetImguiIniPath();

  // Profile-agnostic locations.
  static std::filesystem::path GetProfilesRoot();          // <root>/profiles/
  static std::filesystem::path GetProfileDir(const std::string &name);
  static std::filesystem::path GetBundleDir();
  static std::filesystem::path GetCaBundlePath();
  static std::filesystem::path GetCacheDir();
  static std::filesystem::path GetDataPackageCacheDir();

  // Profile management.
  static bool ValidateProfileName(const std::string &name);
  static bool ProfileExists(const std::string &name);
  static std::vector<ProfileInfo> ListProfiles(); // sorted: most-recent first
  // Creates a new profile dir; if fork_from is non-empty and exists, copies
  // config.yaml and imgui.ini from it. Returns true on success.
  static bool CreateProfile(const std::string &name,
                            const std::string &fork_from);
  static bool DeleteProfile(const std::string &name);
  static void TouchLastUsed(const std::string &name);

  // Idempotent. Moves any pre-profile state files from <root>/ into
  // profiles/default/ if profiles/ does not yet exist.
  static void EnsureMigrated();

  static ConnectionSettings Load();
  static void Save(const ConnectionSettings &settings);
  static std::string GenerateUUID();

private:
  // Root config dir (no profiles/ suffix). Always exists after first call.
  static std::filesystem::path GetConfigRoot();
};
