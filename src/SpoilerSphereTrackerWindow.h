#pragma once
#include "ArchipelagoNetwork.h"
#include "SpoilerLog.h"
#include "Window.h"
#include <string>
#include <vector>

class SpoilerSphereTrackerWindow : public Window {
public:
  SpoilerSphereTrackerWindow(ArchipelagoNetwork &ap_network,
                             const ConnectionSettings &settings);

  void Render(std::tm *current_tm, ImFont *custom_font, ImFont *preview_font,
              ImFont *preview_fallback_font) override;

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  SpoilerLog log_;
  std::string log_path_;

  uint64_t last_data_version_ = 0;

  void LoadSpoilerLog();
};
