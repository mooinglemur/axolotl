#include "ChatWindow.h"
#include "Config.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <set>

ChatWindow::ChatWindow(ArchipelagoNetwork &ap_network,
                       ConnectionSettings &settings, const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings) {
  input_text_.reserve(256);

  // Load initial settings
  strncpy(server_url_, settings.server_url.c_str(), sizeof(server_url_) - 1);
}

void ChatWindow::Render(ImFont *custom_font, ImFont *preview_font,
                        ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    // Multi-slot Connection Controls
    float avail_width = ImGui::GetContentRegionAvail().x;
    float server_width = std::max(210.0f, avail_width * 0.25f);
    float slot_width = std::max(150.0f, avail_width * 0.2f);
    float pw_width = std::max(100.0f, avail_width * 0.15f);

    ImGui::SetNextItemWidth(server_width);
    ImGui::BeginDisabled(ap_network_.IsAnySessionActive());

    char *input_buf = server_url_;
    ImGuiID url_id = ImGui::GetID("Server URL");
    // We show the real URL if focused OR if streamer mode is OFF
    bool show_real = !settings_.streamer_mode || (ImGui::GetActiveID() == url_id) ||
                     (ImGui::GetFocusID() == url_id) || wants_focus_url_;

    if (!show_real) {
      std::string masked = ArchipelagoNetwork::MaskURL(server_url_);
      memset(masked_url_, 0, sizeof(masked_url_));
      strncpy(masked_url_, masked.c_str(), sizeof(masked_url_) - 1);
      input_buf = masked_url_;
    }

    if (show_real && wants_focus_url_) {
      ImGui::SetKeyboardFocusHere();
      wants_focus_url_ = false;
    }

    const char *label = (input_buf == server_url_) ? "Server URL" : "Server URL##Masked";
    ImGuiInputTextFlags flags =
        (input_buf == server_url_) ? 0 : ImGuiInputTextFlags_ReadOnly;
    if (ImGui::InputText(label, input_buf,
                         (input_buf == server_url_) ? sizeof(server_url_)
                                                    : strlen(input_buf) + 1,
                         flags)) {
      if (input_buf == server_url_) {
        settings_.server_url = server_url_;
      }
    }
    if (input_buf != server_url_ && ImGui::IsItemClicked()) {
      wants_focus_url_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Add Slot")) {
      settings_.slots.push_back({"NewPlayer", "", false});
      ap_network_.AddSession(settings_.slots.back().name);
      Config::Save(settings_);
    }

    ImGui::Separator();

    for (int i = 0; i < (int)settings_.slots.size(); ++i) {
      ImGui::PushID(i);
      auto &slot = settings_.slots[i];
      auto session = ap_network_.GetSession(slot.name);
      auto state = session ? session->GetState()
                           : ArchipelagoNetwork::State::Disconnected;

      ImGui::BeginDisabled(state != ArchipelagoNetwork::State::Disconnected);
      ImGui::SetNextItemWidth(slot_width);
      char s_buf[64];
      strncpy(s_buf, slot.name.c_str(), sizeof(s_buf) - 1);
      if (ImGui::InputText("##SlotName", s_buf, sizeof(s_buf))) {
        // Need careful rename handling if we wanted to be robust
        slot.name = s_buf;
      }
      ImGui::SetItemTooltip("Slot Name / Player Name");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(pw_width);
      char p_buf[64];
      strncpy(p_buf, slot.password.c_str(), sizeof(p_buf) - 1);
      if (ImGui::InputText("##Password", p_buf, sizeof(p_buf),
                           ImGuiInputTextFlags_Password)) {
        slot.password = p_buf;
      }
      ImGui::SetItemTooltip(
          "Input slot password here if the server requires one.");
      ImGui::EndDisabled();

      ImGui::SameLine();
      if (state == ArchipelagoNetwork::State::Disconnected) {
        if (ImGui::Button("Connect")) {
          Config::Save(settings_); // Save before connecting
          if (!session)
            session = ap_network_.AddSession(slot.name);
          session->Connect(settings_.server_url, slot.password);
        }
      } else if (state == ArchipelagoNetwork::State::Connecting) {
        if (ImGui::Button("Cancel")) {
          if (session)
            session->Disconnect();
        }
      } else if (state == ArchipelagoNetwork::State::Connected) {
        if (ImGui::Button("Disconnect")) {
          if (session)
            session->Disconnect();
        }
      }

      ImGui::SameLine();
      bool can_remove = (settings_.slots.size() > 1) &&
                        (state == ArchipelagoNetwork::State::Disconnected);
      ImGui::BeginDisabled(!can_remove);
      if (ImGui::Button("Remove")) {
        ap_network_.RemoveSession(slot.name);
        settings_.slots.erase(settings_.slots.begin() + i);
        Config::Save(settings_);
        ImGui::EndDisabled();
        ImGui::PopID();
        break;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }

    ImGui::Separator();

    // History
    const auto &history = ap_network_.GetChatHistory();
    const float footer_height_to_reserve =
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();

    // Day-change detection (simplified for multi-slot - just use system time)
    int current_yday = -1;
    int current_year = -1;
    {
      std::time_t now = std::time(nullptr);
      std::tm *now_tm = std::localtime(&now);
      current_yday = now_tm->tm_yday;
      current_year = now_tm->tm_year;
    }

    bool show_date = false;
    if (!history.empty()) {
      std::time_t t = (std::time_t)history.front().timestamp;
      std::tm *rm_tm = std::localtime(&t);
      if (rm_tm->tm_yday != current_yday || rm_tm->tm_year != current_year) {
        show_date = true;
      }
    }

    if (ImGui::BeginChild(
            "ChatScrollingRegion", ImVec2(0, -footer_height_to_reserve),
            ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
      if (custom_font)
        ImGui::PushFont(custom_font);
      const std::set<int> &my_slots = ap_network_.GetConnectedSlots();
      int visible_row_idx = 0;
      for (int i = 0; i < (int)history.size(); ++i) {
        const auto &rm = history[i];
        ImGui::PushID(i);

        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

        ImVec2 pos_start = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();

        // Timestamp
        std::time_t t = (std::time_t)rm.timestamp;
        std::tm *tm_ptr = std::localtime(&t);
        char time_buf[64];
        if (show_date) {
          std::strftime(time_buf, sizeof(time_buf),
                        settings_.timestamp_format_long.c_str(), tm_ptr);
        } else {
          std::strftime(time_buf, sizeof(time_buf),
                        settings_.timestamp_format_short.c_str(), tm_ptr);
        }

        RenderRichMessageWrapped(time_buf, rm.parts, &my_slots);
        ImGui::EndGroup();
        ImVec2 item_size = ImGui::GetItemRectSize();
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
        ImGui::SetCursorScreenPos(pos_start);

        if (visible_row_idx % 2 == 1) {
          float x_min =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
          float x_max =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(x_min, pos_start.y),
              ImVec2(x_max, pos_start.y + item_size.y),
              ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
        }

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
        }
        if (ImGui::IsItemClicked(0)) {
          if (ImGui::GetIO().KeyShift && selection_anchor_ != -1)
            selection_active_ = i;
          else {
            selection_anchor_ = i;
            selection_active_ = i;
          }
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(0))
          selection_active_ = i;

        if (ImGui::BeginPopupContextItem("ChatLineCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (ImGui::MenuItem("Copy selection")) {
            std::string selected_text;
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            for (int j = start; j <= end; ++j) {
              for (const auto &p : history[j].parts)
                selected_text += p.text;
              if (j < end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Copy selection (with timestamps)")) {
            std::string selected_text;
            int start = std::min(selection_anchor_, selection_active_);
            int end = std::max(selection_anchor_, selection_active_);
            for (int j = start; j <= end; ++j) {
              const auto &rm_j = history[j];
              std::time_t t = (std::time_t)rm_j.timestamp;
              std::tm *tm_ptr = std::localtime(&t);
              char time_buf[64];
              if (show_date) {
                std::strftime(time_buf, sizeof(time_buf),
                              settings_.timestamp_format_long.c_str(), tm_ptr);
              } else {
                std::strftime(time_buf, sizeof(time_buf),
                              settings_.timestamp_format_short.c_str(), tm_ptr);
              }
              selected_text += time_buf;
              selected_text += " ";
              for (const auto &p : rm_j.parts)
                selected_text += p.text;
              if (j < end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Clear Selection")) {
            selection_anchor_ = -1;
            selection_active_ = -1;
          }
          ImGui::EndPopup();
        }

        ImGui::GetWindowDrawList()->ChannelsMerge();
        ImGui::PopID();
        visible_row_idx++;
      }

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

    // Input with Slot Selection Dropdown
    std::vector<std::string> connected_slots;
    for (const auto &s : ap_network_.GetSessions()) {
      if (s->IsConnected())
        connected_slots.push_back(s->GetName());
    }

    int selected_idx = -1;
    if (!selected_send_slot_name_.empty()) {
      auto it = std::find(connected_slots.begin(), connected_slots.end(),
                          selected_send_slot_name_);
      if (it != connected_slots.end())
        selected_idx = std::distance(connected_slots.begin(), it);
    }
    if (selected_idx == -1 && !connected_slots.empty()) {
      selected_idx = 0;
      selected_send_slot_name_ = connected_slots[0];
    }

    if (connected_slots.empty()) {
      ImGui::BeginDisabled();
      ImGui::Text("Connect a slot to chat...");
    } else {
      float max_name_width = 0.0f;
      for (const auto &name : connected_slots) {
        max_name_width =
            std::max(max_name_width, ImGui::CalcTextSize(name.c_str()).x);
      }
      float combo_width = max_name_width +
                          ImGui::GetStyle().FramePadding.x * 2.0f +
                          ImGui::GetFrameHeight();
      ImGui::SetNextItemWidth(combo_width);
      if (ImGui::BeginCombo("##SlotSelect",
                            connected_slots[selected_idx].c_str())) {
        for (int i = 0; i < (int)connected_slots.size(); ++i) {
          if (ImGui::Selectable(connected_slots[i].c_str(), i == selected_idx))
            selected_send_slot_name_ = connected_slots[i];
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
    }

    ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory |
                                      ImGuiInputTextFlags_CallbackAlways;
    if (ac_active_)
      input_flags |= ImGuiInputTextFlags_CallbackCompletion;

    // Autocomplete logic remains mostly same but uses selected slot's player
    // names
    if (ac_active_ && !ac_matches_.empty()) {
      ImVec2 pos = ImGui::GetCursorScreenPos();
      pos.y -= (ac_matches_.size() * ImGui::GetTextLineHeightWithSpacing()) +
               ImGui::GetStyle().WindowPadding.y * 2;
      ImGui::SetNextWindowPos(pos);
      ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(500, 200));
      if (ImGui::Begin("AutoCompPopup", nullptr,
                       ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (int i = 0; i < (int)ac_matches_.size(); ++i) {
          if (ImGui::Selectable(ac_matches_[i].c_str(),
                                i == ac_selected_idx_)) {
            std::string buf_str(input_buf_);
            buf_str.replace(ac_cursor_pos_, ac_match_string_.length(),
                            ac_matches_[i] + " ");
            strncpy(input_buf_, buf_str.c_str(), sizeof(input_buf_) - 1);
            ac_active_ = false;
            ImGui::SetKeyboardFocusHere(-1);
          }
        }
        ImGui::End();
      }
    }

    float button_width =
        ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float input_width = ImGui::GetContentRegionAvail().x - button_width -
                        ImGui::GetStyle().ItemSpacing.x;
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere(0);
      focus_input_ = false;
    }
    ImGui::SetNextItemWidth(input_width);
    bool send =
        ImGui::InputText("##Input", input_buf_, sizeof(input_buf_), input_flags,
                         &ChatWindow::TextEditCallbackStub, (void *)this);
    ImGui::SameLine();
    if (ImGui::Button("Send"))
      send = true;

    if (send && input_buf_[0] != '\0' && !connected_slots.empty()) {
      ap_network_.SendChat(selected_send_slot_name_, input_buf_);
      if (input_history_.empty() || input_history_.back() != input_buf_)
        input_history_.push_back(input_buf_);
      history_pos_ = -1;
      ac_active_ = false;
      input_buf_[0] = '\0';
      focus_input_ = true;
    }
    if (connected_slots.empty())
      ImGui::EndDisabled();
  }
  ImGui::End();
}

int ChatWindow::TextEditCallbackStub(ImGuiInputTextCallbackData *data) {
  return ((ChatWindow *)data->UserData)->TextEditCallback(data);
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

      std::vector<std::string> connected_slots;
      for (const auto &s : ap_network_.GetSessions())
        if (s->IsConnected())
          connected_slots.push_back(s->GetName());

      int selected_idx = -1;
      if (!selected_send_slot_name_.empty()) {
        auto it = std::find(connected_slots.begin(), connected_slots.end(),
                            selected_send_slot_name_);
        if (it != connected_slots.end())
          selected_idx = std::distance(connected_slots.begin(), it);
      }

      if (selected_idx != -1) {
        auto s = ap_network_.GetSession(connected_slots[selected_idx]);
        if (s) {
          for (const auto &[id, name] : s->GetPlayerNames()) {
            if (name == "Unknown" || name == "Server")
              continue;
            std::string l_name = name;
            std::transform(l_name.begin(), l_name.end(), l_name.begin(),
                           ::tolower);
            if (l_name.find(l_match) == 0)
              ac_matches_.push_back(name);
          }
        }
      }
      if (ac_matches_.empty())
        ac_active_ = false;
      else if (ac_selected_idx_ >= (int)ac_matches_.size())
        ac_selected_idx_ = ac_matches_.size() - 1;
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
      int old_pos = history_pos_;
      if (data->EventKey == ImGuiKey_UpArrow) {
        if (history_pos_ == -1)
          history_pos_ = input_history_.size() - 1;
        else if (history_pos_ > 0)
          history_pos_--;
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (history_pos_ != -1 && ++history_pos_ >= (int)input_history_.size())
          history_pos_ = -1;
      }
      if (old_pos != history_pos_) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(
            0, (history_pos_ >= 0) ? input_history_[history_pos_].c_str() : "");
      }
    }
  }
  return 0;
}
