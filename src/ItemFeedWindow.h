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

  int selection_anchor_idx_ = -1;
  int selection_active_idx_ = -1;

  std::vector<float> row_height_cache_;
  std::vector<int> display_indices_;
  size_t last_history_data_size_ = 0;
  int last_display_indices_size_ = 0;
  float last_scroll_max_y_ = 0;
  float last_window_width_ = 0;
  std::string last_filter_text_;
  bool show_long_dates_ = false;
  int last_display_end_ = 0;
  float last_avg_height_ = -1.0f;
  double measured_height_sum_ = 0;
  int measured_rows_count_ = 0;
};
