#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <string>

class ItemFeedWindow : public Window {
public:
  ItemFeedWindow(ArchipelagoNetwork &ap_network,
                 const ConnectionSettings &settings, bool personal_only = false,
                 const std::string &name = "Personal Feed");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  bool personal_only_;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;
};
