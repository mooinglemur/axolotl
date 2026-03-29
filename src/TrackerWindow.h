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
};
