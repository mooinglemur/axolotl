#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <unordered_map>

class Application;

class TrackerWindow : public Window {
public:
  TrackerWindow(ArchipelagoNetwork &ap_network,
                const ConnectionSettings &settings,
                Application &app);

  void Render(std::tm *current_tm, ImFont *custom_font, ImFont *preview_font,
              ImFont *preview_fallback_font) override;

private:
  ArchipelagoNetwork &ap_network_;
  Application &app_;
  std::string filter_text_;
  bool focus_filter_ = false;
  const ConnectionSettings &settings_;

  struct LocationInfo {
    int64_t id;
    std::string name;
  };
  struct SessionCache {
    uint64_t data_version = 0;
    std::vector<LocationInfo> unchecked;
    std::vector<std::string> checked_names;
    std::string game;
    // Per-section open/closed state for the grouped Logical Progression UI.
    // Keyed by top-level path segment (section header name). Persists for the
    // lifetime of the app session; new (unseen) sections default to collapsed.
    std::unordered_map<std::string, bool> section_open_states;
  };
  std::map<std::string, SessionCache> session_caches_;
};
