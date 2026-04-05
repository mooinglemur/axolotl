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
  bool exclude_filler_ = false;
  bool focus_filter_ = false;

  int selection_anchor_idx_ = -1;
  int selection_active_idx_ = -1;

  std::vector<double> row_height_cache_;
  std::vector<double> cumulative_heights_;
  std::vector<int> display_indices_;
  size_t last_history_data_size_ = 0;
  int last_display_indices_size_ = 0;
  double last_scroll_max_y_ = 0;
  double last_window_width_ = 0;
  std::string last_filter_text_;
  bool show_long_dates_ = false;
  int last_display_end_ = 0;
  double last_avg_height_ = -1.0f;
  double measured_height_sum_ = 0;
  int measured_rows_count_ = 0;
  bool locked_to_bottom_ = true;
  double last_reported_scroll_y_ = -1.0f;
  double last_reported_scroll_max_y_ = -1.0f;
  double last_reported_window_h_ = -1.0f;
  bool last_reported_locked_ = false;
  double last_stable_height_ = 0.0f;
  bool last_exclude_filler_ = false;
};
