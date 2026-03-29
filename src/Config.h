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
  std::string uuid = "";
  std::map<std::string, bool> show_windows;
};

class Config {
public:
  static std::filesystem::path GetConfigDir();
  static std::filesystem::path GetConfigPath();
  static std::filesystem::path GetImguiIniPath();
  static std::filesystem::path GetBundleDir();
  static std::filesystem::path GetCaBundlePath();
  static std::filesystem::path GetCacheDir();
  static std::filesystem::path GetDataPackageCacheDir();
  static ConnectionSettings Load();
  static void Save(const ConnectionSettings &settings);
  static std::string GenerateUUID();
};
