#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <imgui.h>
#include <string>

class HintWindow : public Window {
public:
  HintWindow(ArchipelagoNetwork &ap_network, const ConnectionSettings &settings,
             const std::string &name = "Hints");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;
};
