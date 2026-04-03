#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"

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
  };
  std::map<std::string, SessionCache> session_caches_;
};
