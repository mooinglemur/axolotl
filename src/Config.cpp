#include "Config.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
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

std::filesystem::path Config::GetBundleDir() {
  // Respect APPDIR if set (standard for AppImage)
  const char *appdir = getenv("APPDIR");
  if (appdir) {
    return std::filesystem::path(appdir) / "usr" / "local" / "share" /
           "axolotl";
  }

#ifdef _WIN32
  // Fallback to executable directory on Windows
  wchar_t buffer[MAX_PATH];
  GetModuleFileNameW(NULL, buffer, MAX_PATH);
  return std::filesystem::path(buffer).parent_path();
#else
  // Fallback to /proc/self/exe on Linux
  std::error_code ec;
  auto exe_path = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    // Binary is in bin, share is in ../share/axolotl
    return exe_path.parent_path().parent_path() / "share" / "axolotl";
  }
#endif

  return std::filesystem::current_path();
}

std::filesystem::path Config::GetCaBundlePath() {
  return GetBundleDir() / "cacert.pem";
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
    if (config["slots"] && config["slots"].IsSequence()) {
      for (const auto &slot_node : config["slots"]) {
        SlotSettings slot;
        if (slot_node["name"])
          slot.name = slot_node["name"].as<std::string>();
        slot.last_name = slot.name;
        if (slot_node["password"])
          slot.password = slot_node["password"].as<std::string>();
        if (slot_node["connect_on_launch"])
          slot.connect_on_launch = slot_node["connect_on_launch"].as<bool>();
        settings.slots.push_back(slot);
      }
    } else if (config["slot_name"]) {
      // Migration
      SlotSettings slot;
      slot.name = config["slot_name"].as<std::string>();
      slot.last_name = slot.name;
      if (config["password"])
        slot.password = config["password"].as<std::string>();
      settings.slots.push_back(slot);
    }
    if (config["ui_scale"])
      settings.ui_scale = config["ui_scale"].as<float>();
    if (config["content_scale"])
      settings.content_scale = config["content_scale"].as<float>();
    if (config["font_path"])
      settings.font_path = config["font_path"].as<std::string>();
    if (config["fallback_font_path"])
      settings.fallback_font_path =
          config["fallback_font_path"].as<std::string>();
    if (config["max_history_size"])
      settings.max_history_size = config["max_history_size"].as<int>();
    if (config["window_width"])
      settings.window_width = config["window_width"].as<int>();
    if (config["window_height"])
      settings.window_height = config["window_height"].as<int>();
    if (config["window_x"])
      settings.window_x = config["window_x"].as<int>();
    if (config["window_y"])
      settings.window_y = config["window_y"].as<int>();
    if (config["collapse_received_items"])
      settings.collapse_received_items =
          config["collapse_received_items"].as<bool>();
    if (config["streamer_mode"])
      settings.streamer_mode = config["streamer_mode"].as<bool>();
    if (config["show_windows"]) {
      for (const auto &kv : config["show_windows"]) {
        settings.show_windows[kv.first.as<std::string>()] =
            kv.second.as<bool>();
      }
    }
    if (config["shade_alternating_rows"])
      settings.shade_alternating_rows =
          config["shade_alternating_rows"].as<bool>();
    if (config["confirm_exit"])
      settings.confirm_exit = config["confirm_exit"].as<bool>(true);
    if (config["show_chat_timestamps"])
      settings.show_chat_timestamps =
          config["show_chat_timestamps"].as<bool>(true);
    if (config["show_feed_timestamps"])
      settings.show_feed_timestamps =
          config["show_feed_timestamps"].as<bool>(true);
    if (config["timestamp_format_long"])
      settings.timestamp_format_long =
          config["timestamp_format_long"].as<std::string>();
    if (config["timestamp_format_short"])
      settings.timestamp_format_short =
          config["timestamp_format_short"].as<std::string>();
    if (config["uuid"])
      settings.uuid = config["uuid"].as<std::string>();

    if (settings.uuid.empty()) {
      settings.uuid = GenerateUUID();
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
  out << YAML::Key << "slots" << YAML::Value << YAML::BeginSeq;
  for (const auto &slot : settings.slots) {
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value << slot.name;
    out << YAML::Key << "password" << YAML::Value << slot.password;
    out << YAML::Key << "connect_on_launch" << YAML::Value
        << slot.connect_on_launch;
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::Key << "ui_scale" << YAML::Value << settings.ui_scale;
  out << YAML::Key << "content_scale" << YAML::Value << settings.content_scale;
  out << YAML::Key << "font_path" << YAML::Value << settings.font_path;
  out << YAML::Key << "fallback_font_path" << YAML::Value
      << settings.fallback_font_path;
  out << YAML::Key << "max_history_size" << YAML::Value
      << settings.max_history_size;
  out << YAML::Key << "window_width" << YAML::Value << settings.window_width;
  out << YAML::Key << "window_height" << YAML::Value << settings.window_height;
  out << YAML::Key << "window_x" << YAML::Value << settings.window_x;
  out << YAML::Key << "window_y" << YAML::Value << settings.window_y;
  out << YAML::Key << "collapse_received_items" << YAML::Value
      << settings.collapse_received_items;
  out << YAML::Key << "streamer_mode" << YAML::Value << settings.streamer_mode;
  out << YAML::Key << "shade_alternating_rows" << YAML::Value
      << settings.shade_alternating_rows;
  out << YAML::Key << "confirm_exit" << YAML::Value << settings.confirm_exit;
  out << YAML::Key << "show_chat_timestamps" << YAML::Value
      << settings.show_chat_timestamps;
  out << YAML::Key << "show_feed_timestamps" << YAML::Value
      << settings.show_feed_timestamps;
  out << YAML::Key << "timestamp_format_long" << YAML::Value
      << settings.timestamp_format_long;
  out << YAML::Key << "timestamp_format_short" << YAML::Value
      << settings.timestamp_format_short;
  out << YAML::Key << "show_windows" << YAML::Value << YAML::BeginMap;
  for (const auto &kv : settings.show_windows) {
    out << YAML::Key << kv.first << YAML::Value << kv.second;
  }
  out << YAML::EndMap;
  out << YAML::Key << "uuid" << YAML::Value << settings.uuid;

  out << YAML::EndMap;

  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::mt19937_64 gen(now);
  std::uniform_int_distribution<uint64_t> dist;
  std::string tmp_name = "config.yaml.tmp." + std::to_string(dist(gen));
  auto tmp_path = path.parent_path() / tmp_name;

  std::ofstream fout(tmp_path);
  if (!fout.is_open()) {
    std::cerr << "Error: Could not open config file for writing: " << tmp_path
              << std::endl;
    return;
  }
  fout << out.c_str();
  fout.close();

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::cerr << "Error: Could not rename config file: " << ec.message()
              << std::endl;
  }
}

std::string Config::GenerateUUID() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  static std::uniform_int_distribution<> dis2(8, 11);
  static const char *digits = "0123456789abcdef";

  std::string uuid = "";
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      uuid += "-";
    } else if (i == 14) {
      uuid += "4";
    } else if (i == 19) {
      uuid += digits[dis2(gen)];
    } else {
      uuid += digits[dis(gen)];
    }
  }
  return uuid;
}
