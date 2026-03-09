#include "ArchipelagoNetwork.h"
#include "Window.h"
#include <functional>
#include <string>
#include <vector>

class ChatWindow : public Window {
public:
  ChatWindow(
      const std::vector<RichMessage> &history,
      std::function<void(const std::string &)> on_send_chat,
      std::function<ArchipelagoNetwork::State()> get_state,
      std::function<void(const std::string &, const std::string &,
                         const std::string &)>
          on_connect,
      std::function<void()> on_disconnect,
      std::function<const std::map<int, std::string> &()> get_player_names,
      const std::string &name = "Chat");
  void Render(ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr) override;

private:
  char server_url_[256] = "archipelago.gg:0";
  char slot_name_[64] = "Player1";
  char password_[64] = "";

  std::function<ArchipelagoNetwork::State()> get_state_;
  std::function<void(const std::string &, const std::string &,
                     const std::string &)>
      on_connect_;
  std::function<void()> on_disconnect_;
  std::function<const std::map<int, std::string> &()> get_player_names_;

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
  const std::vector<RichMessage> &history_;
  std::function<void(const std::string &)> on_send_chat_;

  static int TextEditCallbackStub(ImGuiInputTextCallbackData *data);
  int TextEditCallback(ImGuiInputTextCallbackData *data);
};
