#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <string>

class ReceivedItemsWindow : public Window {
public:
  ReceivedItemsWindow(ArchipelagoNetwork &ap_network,
                      const ConnectionSettings &settings,
                      const std::string &name = "Received Items");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;
  void SaveState(ConnectionSettings &settings) override;

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  bool collapse_ = true;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;
};
