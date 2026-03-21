#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <string>
#include <vector>

class ChatWindow : public Window {
public:
  ChatWindow(ArchipelagoNetwork &ap_network, ConnectionSettings &settings,
             const std::string &name = "Chat");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  ArchipelagoNetwork &ap_network_;
  ConnectionSettings &settings_;

  char server_url_[256] = "archipelago.gg:0";
  int selected_send_slot_idx_ = 0;

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

  int selection_anchor_ = -1;
  int selection_active_ = -1;
  bool focus_input_ = false;

  static int TextEditCallbackStub(ImGuiInputTextCallbackData *data);
  int TextEditCallback(ImGuiInputTextCallbackData *data);
};
