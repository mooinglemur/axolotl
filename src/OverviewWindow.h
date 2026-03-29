#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"

class OverviewWindow : public Window {
public:
  OverviewWindow(ArchipelagoNetwork &ap_network, ConnectionSettings &settings);
  void Render(std::tm *current_tm, ImFont *custom_font, ImFont *preview_font,
              ImFont *preview_fallback_font) override;

private:
  ArchipelagoNetwork &ap_network_;
  ConnectionSettings &settings_;
  char tracker_url_buf_[512];
  std::string last_settings_tracker_url_;
  bool sync_triggered_ = false;
  double last_seen_sync_time_ = -1.0;
  std::string filter_text_;
  bool focus_filter_ = false;
  bool exclude_goal_complete_ = false;
};
