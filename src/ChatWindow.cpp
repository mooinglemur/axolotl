#include "ChatWindow.h"
#include "Config.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <imgui.h>

ChatWindow::ChatWindow(
    const std::vector<RichMessage> &history,
    std::function<void(const std::string &)> on_send_chat,
    std::function<ArchipelagoNetwork::State()> get_state,
    std::function<void(const std::string &, const std::string &,
                       const std::string &)>
        on_connect,
    std::function<void()> on_disconnect,
    std::function<const std::map<int, std::string> &()> get_player_names,
    const std::string &name)
    : Window(name), history_(history), on_send_chat_(on_send_chat),
      get_state_(get_state), on_connect_(on_connect),
      on_disconnect_(on_disconnect), get_player_names_(get_player_names) {
  input_text_.reserve(256);

  // Load initial settings
  auto settings = Config::Load();
  strncpy(server_url_, settings.server_url.c_str(), sizeof(server_url_) - 1);
  strncpy(slot_name_, settings.slot_name.c_str(), sizeof(slot_name_) - 1);
  strncpy(password_, settings.password.c_str(), sizeof(password_) - 1);
}

void ChatWindow::Render(ImFont *custom_font, ImFont *preview_font,
                        ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    // Connection Controls (Single Line)
    auto state =
        get_state_ ? get_state_() : ArchipelagoNetwork::State::Disconnected;
    bool is_disconnected = (state == ArchipelagoNetwork::State::Disconnected);

    ImGui::BeginDisabled(!is_disconnected);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.18f);
    ImGui::InputText("##Server", server_url_, sizeof(server_url_));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Server URL");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.2f);
    ImGui::InputText("##Slot", slot_name_, sizeof(slot_name_));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Slot Name");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.2f);
    ImGui::InputText("##Password", password_, sizeof(password_),
                     ImGuiInputTextFlags_Password);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Password");

    ImGui::EndDisabled();

    ImGui::SameLine();
    if (state == ArchipelagoNetwork::State::Disconnected) {
      if (ImGui::Button("Connect")) {
        if (on_connect_) {
          on_connect_(server_url_, slot_name_, password_);
        }
      }
    } else if (state == ArchipelagoNetwork::State::Connecting) {
      if (ImGui::Button("Cancel")) {
        if (on_disconnect_)
          on_disconnect_();
      }
    } else if (state == ArchipelagoNetwork::State::Connected) {
      if (ImGui::Button("Disconnect")) {
        if (on_disconnect_)
          on_disconnect_();
      }
    }

    ImGui::Separator();

    ImGui::Separator();

    // History
    const float footer_height_to_reserve =
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild(
            "ScrollingRegion", ImVec2(0, -footer_height_to_reserve),
            ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
      if (custom_font)
        ImGui::PushFont(custom_font);
      for (int i = 0; i < (int)history_.size(); ++i) {
        const auto &rm = history_[i];
        ImGui::PushID(i);

        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

        ImVec2 pos_start = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();

        // Timestamp
        std::time_t t = (std::time_t)rm.timestamp;
        std::tm *tm_ptr = std::localtime(&t);
        char time_buf[16];
        std::strftime(time_buf, sizeof(time_buf), "[%H:%M:%S]", tm_ptr);

        RenderRichMessageWrapped(time_buf, rm.parts);

        ImGui::EndGroup();
        ImVec2 item_size = ImGui::GetItemRectSize();
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
        ImGui::SetCursorScreenPos(pos_start);

        bool is_selected = false;
        if (selection_anchor_ != -1 && selection_active_ != -1) {
          int start = std::min(selection_anchor_, selection_active_);
          int end = std::max(selection_anchor_, selection_active_);
          is_selected = (i >= start && i <= end);
        }

        char label[32];
        snprintf(label, sizeof(label), "##row_%d", i);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, item_size.y))) {
          // Handled by mouse state below for drag support
        }

        if (ImGui::IsItemClicked(0)) {
          if (ImGui::GetIO().KeyShift && selection_anchor_ != -1) {
            selection_active_ = i;
          } else {
            if (selection_anchor_ == i && selection_active_ == i) {
              // Clicked the only selected item, so toggle off
              selection_anchor_ = -1;
              selection_active_ = -1;
            } else {
              selection_anchor_ = i;
              selection_active_ = i;
            }
          }
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(0)) {
          selection_active_ = i;
        }

        ImGui::GetWindowDrawList()->ChannelsMerge();

        ImGui::PopID();
      }

      if (ImGui::BeginPopupContextWindow("MessageCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Copy Selected")) {
          if (selection_anchor_ != -1 && selection_active_ != -1) {
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            std::string full_text;
            for (int sel = start; sel <= end; ++sel) {
              const auto &m = history_[sel];
              std::time_t mt = (std::time_t)m.timestamp;
              std::tm *mtm = std::localtime(&mt);
              char mt_buf[16];
              std::strftime(mt_buf, sizeof(mt_buf), "[%H:%M:%S] ", mtm);
              full_text += mt_buf;
              for (const auto &p : m.parts)
                full_text += p.text;
              full_text += "\n";
            }
            ImGui::SetClipboardText(full_text.c_str());
          }
        }
        if (ImGui::MenuItem("Clear Selection")) {
          selection_anchor_ = -1;
          selection_active_ = -1;
        }
        ImGui::EndPopup();
      }

      // Clear selection if clicking empty space
      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_anchor_ = -1;
        selection_active_ = -1;
      }

      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
      if (custom_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Input
    ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory |
                                      ImGuiInputTextFlags_CallbackAlways |
                                      ImGuiInputTextFlags_CallbackCompletion;

    // Autocomplete popup
    if (ac_active_ && !ac_matches_.empty()) {
      ImVec2 pos = ImGui::GetCursorScreenPos();
      pos.y -= (ac_matches_.size() * ImGui::GetTextLineHeightWithSpacing()) +
               ImGui::GetStyle().WindowPadding.y * 2;
      ImGui::SetNextWindowPos(pos);
      ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(500, 200));
      ImGuiWindowFlags popup_flags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoFocusOnAppearing;

      if (ImGui::Begin("AutoCompPopup", nullptr, popup_flags)) {
        for (int i = 0; i < (int)ac_matches_.size(); ++i) {
          bool is_selected = (i == ac_selected_idx_);
          if (ImGui::Selectable(ac_matches_[i].c_str(), is_selected)) {
            // Mouse click support
            std::string buf_str(input_buf_);
            if (ac_cursor_pos_ >= 0 &&
                ac_cursor_pos_ + ac_match_string_.length() <=
                    buf_str.length()) {
              buf_str.replace(ac_cursor_pos_, ac_match_string_.length(),
                              ac_matches_[i] + " ");
              strncpy(input_buf_, buf_str.c_str(), sizeof(input_buf_) - 1);
              input_buf_[sizeof(input_buf_) - 1] = '\0';
            }
            ac_active_ = false;
            ImGui::SetKeyboardFocusHere(-1); // Re-focus on input after clicking
          }
          if (is_selected)
            ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::End();
    }

    if (ImGui::InputText("##Input", input_buf_, sizeof(input_buf_), input_flags,
                         &ChatWindow::TextEditCallbackStub, (void *)this)) {
      if (input_buf_[0] != '\0') {
        std::string msg(input_buf_);
        if (on_send_chat_) {
          on_send_chat_(msg);
        }
        if (input_history_.empty() || input_history_.back() != msg) {
          input_history_.push_back(msg);
        }
        history_pos_ = -1;
        ac_active_ = false;
        input_buf_[0] = '\0'; // Clear input
      }
      ImGui::SetKeyboardFocusHere(-1); // Re-focus on enter
    }
  }
  ImGui::End();
}

int ChatWindow::TextEditCallbackStub(ImGuiInputTextCallbackData *data) {
  ChatWindow *window = (ChatWindow *)data->UserData;
  return window->TextEditCallback(data);
}

int ChatWindow::TextEditCallback(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
    if (!ac_active_) {
      // Check for trigger
      int cursor = data->CursorPos;
      if (cursor > 0 && data->Buf[cursor - 1] == '@') {
        ac_active_ = true;
        ac_cursor_pos_ = cursor;
        ac_match_string_ = "";
        ac_selected_idx_ = 0;
      }
    } else {
      // We are active
      int cursor = data->CursorPos;
      if (cursor < ac_cursor_pos_) {
        // Backspaced before trigger
        ac_active_ = false;
        return 0;
      }

      // Update match string
      ac_match_string_ =
          std::string(data->Buf + ac_cursor_pos_, cursor - ac_cursor_pos_);

      // Stop if there is a space
      if (ac_match_string_.find_first_of(" \n") != std::string::npos) {
        ac_active_ = false;
        return 0;
      }

      // Populate matches
      ac_matches_.clear();
      std::string l_match = ac_match_string_;
      std::transform(l_match.begin(), l_match.end(), l_match.begin(),
                     ::tolower);

      if (get_player_names_) {
        for (const auto &[id, name] : get_player_names_()) {
          if (name == "Unknown" || name == "Server")
            continue;
          std::string l_name = name;
          std::transform(l_name.begin(), l_name.end(), l_name.begin(),
                         ::tolower);
          if (l_name.find(l_match) == 0) { // starts with
            ac_matches_.push_back(name);
          }
        }
      }

      if (ac_matches_.empty()) {
        ac_active_ = false;
      } else {
        if (ac_selected_idx_ >= (int)ac_matches_.size()) {
          ac_selected_idx_ = ac_matches_.size() - 1;
        }
      }
    }
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    if (ac_active_ && !ac_matches_.empty()) {
      data->DeleteChars(ac_cursor_pos_, data->CursorPos - ac_cursor_pos_);
      data->InsertChars(data->CursorPos, ac_matches_[ac_selected_idx_].c_str());
      data->InsertChars(data->CursorPos, " ");
      ac_active_ = false;
    }
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    if (ac_active_) {
      if (data->EventKey == ImGuiKey_UpArrow) {
        ac_selected_idx_--;
        if (ac_selected_idx_ < 0)
          ac_selected_idx_ = ac_matches_.size() - 1;
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        ac_selected_idx_++;
        if (ac_selected_idx_ >= (int)ac_matches_.size())
          ac_selected_idx_ = 0;
      }
    } else {
      const int prev_history_pos = history_pos_;
      if (data->EventKey == ImGuiKey_UpArrow) {
        if (history_pos_ == -1)
          history_pos_ = input_history_.size() - 1;
        else if (history_pos_ > 0)
          history_pos_--;
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (history_pos_ != -1) {
          if (++history_pos_ >= (int)input_history_.size()) {
            history_pos_ = -1;
          }
        }
      }

      if (prev_history_pos != history_pos_) {
        const char *history_str =
            (history_pos_ >= 0) ? input_history_[history_pos_].c_str() : "";
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, history_str);
      }
    }
  }
  return 0;
}
