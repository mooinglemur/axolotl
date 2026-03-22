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
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

  struct ResolvedHint {
    Hint hint;
    std::string item_name;
    std::string receiver_name;
    std::string location_name;
    std::string entrance_name;
    std::string finder_name;
    std::string status_name;
    std::string lower_combined;
  };

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;

  std::vector<ResolvedHint> resolved_hints_;
  bool force_rebuild_ = true;
  size_t last_hint_count_ = 0;
  uint64_t last_data_version_ = 0;
};
