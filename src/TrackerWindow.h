#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"

class TrackerWindow : public Window {
public:
  TrackerWindow(ArchipelagoNetwork &ap_network,
                const ConnectionSettings &settings);

  void Render(std::tm *current_tm, ImFont *custom_font, ImFont *preview_font,
              ImFont *preview_fallback_font) override;

private:
  ArchipelagoNetwork &ap_network_;
  std::string filter_text_;
  bool focus_filter_ = false;
  const ConnectionSettings &settings_;

  struct SessionCache {
    uint64_t data_version = 0;
    std::vector<std::string> unchecked_names;
    std::vector<std::string> checked_names;
    std::string game;
  };
  std::map<std::string, SessionCache> session_caches_;
};
