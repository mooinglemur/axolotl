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
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  ArchipelagoNetwork &ap_network_;
  const ConnectionSettings &settings_;
  bool personal_only_;
  std::string filter_text_;

  int selection_anchor_ = -1;
  int selection_active_ = -1;

  std::vector<float> row_height_cache_;
  std::vector<int> display_indices_;
  size_t last_history_size_ = 0;
  float last_scroll_max_y_ = 0;
  float last_window_width_ = 0;
  std::string last_filter_text_;
  bool show_long_dates_ = false;
};
