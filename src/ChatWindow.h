#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <string>
#include <vector>

class ChatWindow : public Window {
public:
  ChatWindow(ArchipelagoNetwork &ap_network, ConnectionSettings &settings,
             std::string &live_server_url, std::vector<SlotSettings> &live_slots,
             const std::string &name = "Chat");
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  ArchipelagoNetwork &ap_network_;
  ConnectionSettings &settings_;
  std::string &live_server_url_;
  std::vector<SlotSettings> &live_slots_;

  char server_url_[256] = "archipelago.gg:0";
  char masked_url_[256] = "";
  std::string selected_send_slot_name_;

  std::string input_text_;
  char input_buf_[256] = "";
  std::vector<std::string> input_history_;
  int history_pos_ = -1;

  // Autocomplete state
  bool ac_active_ = false;
  int ac_cursor_pos_ = -1;
  int ac_selected_idx_ = 0;
  std::string ac_match_string_;
  std::vector<std::string> ac_matches_;

  int selection_anchor_idx_ = -1;
  int selection_active_idx_ = -1;
  bool wants_focus_url_ = false;
  bool focus_input_ = false;

  static int TextEditCallbackStub(ImGuiInputTextCallbackData *data);
  int TextEditCallback(ImGuiInputTextCallbackData *data);
  bool HandleCommand(const std::string &line);

  std::vector<float> row_height_cache_;
  size_t last_history_size_ = 0;
  float last_scroll_max_y_ = 0;
  float last_window_width_ = 0;
  int last_display_end_ = 0;
  float last_avg_height_ = -1.0f;
  double measured_height_sum_ = 0;
  int measured_rows_count_ = 0;
  bool locked_to_bottom_ = true;
};
