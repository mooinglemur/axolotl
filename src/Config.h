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
  bool use_hidpi = true;
  std::string font_path = "";
  int max_history_size = 0;
  std::map<std::string, bool> show_windows;
};

class Config {
public:
  static std::filesystem::path GetConfigDir();
  static std::filesystem::path GetConfigPath();
  static std::filesystem::path GetImguiIniPath();
  static ConnectionSettings Load();
  static void Save(const ConnectionSettings &settings);
};
