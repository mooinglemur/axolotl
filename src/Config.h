#pragma once
#include <filesystem>
#include <map>
#include <string>

struct ConnectionSettings {
  std::string server_url = "archipelago.gg:0";
  std::string slot_name = "Player1";
  std::string password = "";

  // UI Preferences
  float ui_scale = 1.0f;
  float content_scale = 1.0f;
  bool use_hidpi = true;
  std::string font_path = "";
  std::string fallback_font_path = "";
  int max_history_size = 0;
  std::string timestamp_format_long = "[%Y-%m-%d %H:%M:%S]";
  std::string timestamp_format_short = "[%H:%M:%S]";
  int window_width = 1280;
  int window_height = 720;
  int window_x = -1; // -1 means center on monitor
  int window_y = -1;
  std::map<std::string, bool> show_windows;
};

class Config {
public:
  static std::filesystem::path GetConfigDir();
  static std::filesystem::path GetConfigPath();
  static std::filesystem::path GetImguiIniPath();
  static std::filesystem::path GetBundleDir();
  static std::filesystem::path GetCaBundlePath();
  static ConnectionSettings Load();
  static void Save(const ConnectionSettings &settings);
};
