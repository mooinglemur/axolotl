#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "UrlWordSelect.h"
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
  // ':' isn't a word boundary in ImGui, so "archipelago.gg:12345" would
  // otherwise double-click-select as "gg:12345".
  UrlWordSelect url_word_select_;
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

  // Argument completion for the server's !hint / !hint_location commands,
  // driven off the datapackage for the selected slot's own game.
  //
  // Deliberately separate from the '@' completion above rather than folded
  // into it: item and location names contain spaces, so the text being
  // matched runs from the command's argument all the way to the caret instead
  // of stopping at the first space. Tab and Up/Down are taken over while the
  // popup is up, matching the '@' popup's keys.
  enum class HintCompleteKind { None, Item, Location };
  HintCompleteKind hc_kind_ = HintCompleteKind::None;
  int hc_arg_pos_ = -1;   // buffer offset where the argument begins
  int hc_selected_idx_ = 0;
  int hc_total_ = 0;      // matches found before truncating to the popup size
  bool hc_cycled_ = false;    // Tab has walked the list at least once
  bool hc_navigated_ = false; // arrows picked a row, so Tab takes it as-is
  std::string hc_stem_;   // text the current match list was built from
  std::string hc_common_; // longest common prefix over all hc_total_ matches
  std::string hc_applied_;// last text Tab wrote, so cycling doesn't re-match
  std::string hc_slot_;   // slot the match list was built against
  std::vector<std::string> hc_matches_;

  void UpdateHintCompletion(ImGuiInputTextCallbackData *data);
  void ApplyHintCompletion(ImGuiInputTextCallbackData *data,
                           const std::string &text);
  void ResetHintCompletion();

  int selection_anchor_idx_ = -1;
  int selection_active_idx_ = -1;
  bool wants_focus_url_ = false;
  bool focus_input_ = false;

  static int TextEditCallbackStub(ImGuiInputTextCallbackData *data);
  int TextEditCallback(ImGuiInputTextCallbackData *data);
  bool HandleCommand(const std::string &line);

  std::vector<double> row_height_cache_;
  std::vector<double> cumulative_heights_;
  size_t last_history_size_ = 0;
  uint64_t last_history_generation_ = 0;
  double last_scroll_max_y_ = 0;
  double last_window_width_ = 0;
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
};
