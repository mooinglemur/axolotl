#include "Config.h"
#include <fstream>
#include <iostream>
#include <map>
#include <yaml-cpp/yaml.h>

#ifdef _WIN32
#include <shlobj.h>
#endif

std::filesystem::path Config::GetConfigDir() {
  std::filesystem::path config_dir;
#ifdef __APPLE__
  const char *home = getenv("HOME");
  if (home) {
    config_dir = std::filesystem::path(home) / "Library" /
                 "Application Support" / "axolotl-apclient";
  }
#elif defined(_WIN32)
  char path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
    config_dir = std::filesystem::path(path) / "axolotl-apclient";
  }
#else
  const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
  if (xdg_config_home) {
    config_dir = std::filesystem::path(xdg_config_home) / "axolotl-apclient";
  } else {
    const char *home = getenv("HOME");
    if (home) {
      config_dir = std::filesystem::path(home) / ".config" / "axolotl-apclient";
    }
  }
#endif

  if (!config_dir.empty() && !std::filesystem::exists(config_dir)) {
    std::filesystem::create_directories(config_dir);
  }

  return config_dir;
}

std::filesystem::path Config::GetConfigPath() {
  return GetConfigDir() / "config.yaml";
}

std::filesystem::path Config::GetImguiIniPath() {
  return GetConfigDir() / "imgui.ini";
}

ConnectionSettings Config::Load() {
  ConnectionSettings settings;
  auto path = GetConfigPath();
  if (!std::filesystem::exists(path)) {
    return settings;
  }

  try {
    YAML::Node config = YAML::LoadFile(path.string());
    if (config["server_url"])
      settings.server_url = config["server_url"].as<std::string>();
    if (config["slot_name"])
      settings.slot_name = config["slot_name"].as<std::string>();
    if (config["password"])
      settings.password = config["password"].as<std::string>();
    if (config["ui_scale"])
      settings.ui_scale = config["ui_scale"].as<float>();
    if (config["use_hidpi"])
      settings.use_hidpi = config["use_hidpi"].as<bool>();
    if (config["font_path"])
      settings.font_path = config["font_path"].as<std::string>();
    if (config["max_history_size"])
      settings.max_history_size = config["max_history_size"].as<int>();
    if (config["show_windows"]) {
      for (const auto &kv : config["show_windows"]) {
        settings.show_windows[kv.first.as<std::string>()] =
            kv.second.as<bool>();
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error loading config: " << e.what() << std::endl;
  }

  return settings;
}

void Config::Save(const ConnectionSettings &settings) {
  auto path = GetConfigPath();
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "server_url" << YAML::Value << settings.server_url;
  out << YAML::Key << "slot_name" << YAML::Value << settings.slot_name;
  out << YAML::Key << "password" << YAML::Value << settings.password;
  out << YAML::Key << "ui_scale" << YAML::Value << settings.ui_scale;
  out << YAML::Key << "use_hidpi" << YAML::Value << settings.use_hidpi;
  out << YAML::Key << "font_path" << YAML::Value << settings.font_path;
  out << YAML::Key << "max_history_size" << YAML::Value
      << settings.max_history_size;

  out << YAML::Key << "show_windows" << YAML::Value << YAML::BeginMap;
  for (const auto &kv : settings.show_windows) {
    out << YAML::Key << kv.first << YAML::Value << kv.second;
  }
  out << YAML::EndMap;

  out << YAML::EndMap;

  std::ofstream fout(path);
  if (!fout.is_open()) {
    std::cerr << "Error: Could not open config file for writing: " << path
              << std::endl;
    return;
  }
  fout << out.c_str();
}
