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
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;
  void SaveState(ConnectionSettings &settings) override;

  struct DisplayRow {
    RichMessage rm;
    int count = 1;
    std::string text_cache;
    std::string text_lower_cache;
  };

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  bool collapse_ = true;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;

  std::vector<DisplayRow> display_rows_;
  bool force_rebuild_ = true;
  size_t last_history_count_ = 0;
  bool last_collapse_ = false;
  uint64_t last_data_version_ = 0;
  bool show_long_dates_ = false;
};
