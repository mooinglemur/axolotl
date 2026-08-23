#include "ChatWindow.h"
#include "Config.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <random>
#include <set>
#include <thread>

namespace {

// How many candidates the !hint popup lists. Everything listed is also
// everything Tab cycles through, so the highlighted row is always visible.
constexpr int kMaxHintRows = 10;

std::string LowerCopy(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return out;
}

// Longest prefix every candidate agrees on, compared case-insensitively but
// returned in the datapackage's own casing, so completing also fixes up case.
std::string CommonPrefixCI(const std::vector<std::string> &v) {
  if (v.empty())
    return "";
  std::string prefix = v.front();
  for (size_t i = 1; i < v.size() && !prefix.empty(); ++i) {
    const std::string &s = v[i];
    size_t limit = std::min(prefix.size(), s.size());
    size_t k = 0;
    while (k < limit && std::tolower((unsigned char)prefix[k]) ==
                            std::tolower((unsigned char)s[k]))
      ++k;
    prefix.resize(k);
  }
  return prefix;
}

// Matches the command word case-insensitively and requires the separating
// space, so completion only engages once the argument has actually started.
// Returns the buffer offset of the argument, or -1 if this isn't the command.
int HintArgOffset(const char *buf, int len, const char *cmd) {
  int n = (int)strlen(cmd);
  if (len < n + 1)
    return -1;
  for (int i = 0; i < n; ++i)
    if (std::tolower((unsigned char)buf[i]) != (unsigned char)cmd[i])
      return -1;
  if (buf[n] != ' ')
    return -1;
  return n + 1;
}

} // namespace

ChatWindow::ChatWindow(ArchipelagoNetwork &ap_network,
                       ConnectionSettings &settings,
                       std::string &live_server_url,
                       std::vector<SlotSettings> &live_slots,
                       const std::string &name)
    : Window(name), ap_network_(ap_network), settings_(settings),
      live_server_url_(live_server_url), live_slots_(live_slots) {
  input_text_.reserve(256);

  // Load initial settings
  strncpy(server_url_, live_server_url_.c_str(), sizeof(server_url_) - 1);
}

void ChatWindow::Render(std::tm *current_tm, ImFont *custom_font,
                        ImFont *preview_font, ImFont *preview_fallback_font) {
  if (!is_open_)
    return;

  ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
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
    bool show_real = !settings_.streamer_mode ||
                     (ImGui::GetActiveID() == url_id) ||
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

    const char *label =
        (input_buf == server_url_) ? "Server URL" : "Server URL##Masked";
    ImGuiInputTextFlags flags =
        ((input_buf == server_url_) ? 0 : ImGuiInputTextFlags_ReadOnly) |
        ImGuiInputTextFlags_CallbackAlways;
    std::string old_url = live_server_url_;
    if (ImGui::InputText(label, input_buf,
                         (input_buf == server_url_) ? sizeof(server_url_)
                                                    : strlen(input_buf) + 1,
                         flags, &UrlWordSelect::Callback, &url_word_select_)) {
      if (input_buf == server_url_) {
        live_server_url_ = server_url_;
        ap_network_.SetServerUrl(live_server_url_);
        if (live_server_url_ != old_url) {
          // Clear player stats / history but keep the tracker URL — async
          // events frequently change AP server ports without changing
          // their tracker page, so re-typing it every time is annoying.
          ap_network_.ClearAllData(true);
        }
      }
    }
    if (input_buf != server_url_ && ImGui::IsItemClicked()) {
      wants_focus_url_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Add Slot")) {
      live_slots_.push_back(SlotSettings("NewPlayer", "", false));
      ap_network_.AddSession(live_slots_.back().name);
      // Config::Save(settings_); // Don't save on every slot addition anymore
    }

    ImGui::Separator();

    for (int i = 0; i < (int)live_slots_.size(); ++i) {
      ImGui::PushID(i);
      auto &slot = live_slots_[i];
      auto session = ap_network_.GetSession(slot.name);
      if (session && session->GetName() != slot.last_name) {
        // This session belongs to another slot
        session = nullptr;
      }
      auto state = session ? session->GetState()
                           : ArchipelagoNetwork::State::Disconnected;

      bool name_is_duplicate = false;
      for (int j = 0; j < (int)live_slots_.size(); ++j) {
        if (i != j && live_slots_[j].name == slot.name) {
          name_is_duplicate = true;
          break;
        }
      }
      bool name_is_empty = slot.name.empty();
      bool name_invalid =
          name_is_empty || (name_is_duplicate &&
                            state == ArchipelagoNetwork::State::Disconnected);

      ImGui::BeginDisabled(state != ArchipelagoNetwork::State::Disconnected);
      ImGui::SetNextItemWidth(slot_width);
      char s_buf[64];
      strncpy(s_buf, slot.name.c_str(), sizeof(s_buf) - 1);
      if (name_invalid)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      if (ImGui::InputText("##SlotName", s_buf, sizeof(s_buf))) {
        // Need careful rename handling if we wanted to be robust
        slot.name = s_buf;
      }
      if (name_invalid) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(name_is_empty ? "Slot name cannot be empty"
                                            : "Duplicate slot name");
      } else {
        ImGui::SetItemTooltip("Slot Name / Player Name");
      }
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
        ImGui::BeginDisabled(name_invalid);
        if (ImGui::Button("Connect")) {
          if (slot.name != slot.last_name) {
            ap_network_.RemoveSession(slot.last_name);
            slot.last_name = slot.name;
            session = nullptr;
          }
          // Config::Save(settings_); // Save before connecting
          if (!session)
            session = ap_network_.AddSession(slot.name);
          session->Connect(live_server_url_, slot.password);
        }
        ImGui::EndDisabled();
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
      bool can_remove = (live_slots_.size() > 1) &&
                        (state == ArchipelagoNetwork::State::Disconnected);
      ImGui::BeginDisabled(!can_remove);
      if (ImGui::Button("Remove")) {
        std::string name_to_remove = slot.name;
        std::string last_name_to_remove = slot.last_name;
        live_slots_.erase(live_slots_.begin() + i);

        // Only remove session if no other slot uses these names
        bool name_still_used = false;
        bool last_name_still_used = false;
        for (const auto &s : live_slots_) {
          if (s.name == name_to_remove || s.last_name == name_to_remove)
            name_still_used = true;
          if (s.name == last_name_to_remove ||
              s.last_name == last_name_to_remove)
            last_name_still_used = true;
        }

        if (!name_still_used)
          ap_network_.RemoveSession(name_to_remove);
        if (!last_name_still_used && last_name_to_remove != name_to_remove)
          ap_network_.RemoveSession(last_name_to_remove);

        // Config::Save(settings_);
        ImGui::EndDisabled();
        ImGui::PopID();
        break;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }

    ImGui::Separator();

    const float footer_height_to_reserve =
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();

    // Day-change detection (simplified for multi-slot - just use system time)
    int current_yday = current_tm->tm_yday;
    int current_year = current_tm->tm_year;

    bool show_date = false;

    if (ImGui::BeginChild("ChatScrollingRegion",
                          ImVec2(0, -footer_height_to_reserve),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      float threshold = 2.0f * ImGui::GetTextLineHeightWithSpacing();
      bool was_at_bottom =
          (last_scroll_max_y_ <= 0.0f ||
           ImGui::GetScrollY() >= last_scroll_max_y_ - threshold);

      bool interacting = (ImGui::IsWindowHovered(
                              ImGuiHoveredFlags_RootAndChildWindows |
                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                          (ImGui::GetIO().MouseWheel != 0.0f ||
                           ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1)));

      // Re-lock ONLY if near bottom and NOT interacting
      if (was_at_bottom && !interacting) {
        locked_to_bottom_ = true;
      }

      // Unlock if user scrolls away manually while interacting
      if (interacting && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 5.0f) {
        locked_to_bottom_ = false;
      }
      if (custom_font)
        ImGui::PushFont(custom_font);

      ImGui::GetWindowDrawList()->ChannelsSplit(2);

      // Part 26: Acquire history lock before accessing vectors
      std::lock_guard<std::recursive_mutex> lock(ap_network_.GetStateMutex());

      const auto &history = ap_network_.GetChatHistory();
      uint64_t current_generation = ap_network_.GetHistoryGeneration();
      if (current_generation != last_history_generation_) {
        // Items shifted: cached row heights and selection indices point at
        // wrong items now. Drop them.
        std::fill(row_height_cache_.begin(), row_height_cache_.end(), -1.0);
        measured_height_sum_ = 0;
        measured_rows_count_ = 0;
        selection_anchor_idx_ = -1;
        selection_active_idx_ = -1;
        last_history_generation_ = current_generation;
      }
      bool history_grew = (history.size() > last_history_size_);
      if (history.size() != last_history_size_) {
        row_height_cache_.resize(history.size(), -1.0);
      }

      if (selection_anchor_idx_ >= (int)history.size())
        selection_anchor_idx_ = history.empty() ? -1 : (int)history.size() - 1;
      if (selection_active_idx_ >= (int)history.size())
        selection_active_idx_ = history.empty() ? -1 : (int)history.size() - 1;

      bool in_bottom_zone =
          (ImGui::GetScrollY() > ImGui::GetScrollMaxY() - 128.0f);

      double min_h = (double)ImGui::GetTextLineHeightWithSpacing();
      double avg_h = (measured_rows_count_ > 0)
                         ? (double)(measured_height_sum_ / measured_rows_count_)
                         : min_h;
      // Cap average to avoid crazy scrollbar behavior if one message is huge
      if (avg_h > 10.0 * min_h)
        avg_h = 10.0 * min_h;

      ImGuiListClipper clipper;
      int history_count = (int)history.size();
      bool use_clipper = (history_count > 100);

      double current_window_width = (double)ImGui::GetWindowWidth();
      double current_scroll_max_y = (double)ImGui::GetScrollMaxY();

      // Part 28: Million-Pixel Absolute Alignment (Prefix-Sum Cache)
      cumulative_heights_.resize(history_count + 1);
      double current_y_sum = 0;
      for (int i = 0; i < history_count; ++i) {
        cumulative_heights_[i] = current_y_sum;
        double h = (row_height_cache_[i] > 0) ? row_height_cache_[i] : avg_h;
        current_y_sum += h;
      }
      cumulative_heights_[history_count] = current_y_sum;
      double total_content_height = current_y_sum;

      bool force_bottom_render =
          (use_clipper && (locked_to_bottom_ || in_bottom_zone) &&
           !history.empty());

      double actual_bottom_y = -1.0;
      bool is_any_interaction =
          (ImGui::IsWindowHovered(
               ImGuiHoveredFlags_RootAndChildWindows |
               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
           (ImGui::GetIO().MouseWheel != 0.0f || ImGui::IsMouseDown(0) ||
            ImGui::IsMouseDown(1))) ||
          (ImGui::IsWindowFocused() &&
           (ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
            ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
            ImGui::IsKeyDown(ImGuiKey_PageUp) ||
            ImGui::IsKeyDown(ImGuiKey_PageDown) ||
            ImGui::IsKeyDown(ImGuiKey_Home) || ImGui::IsKeyDown(ImGuiKey_End)));

      int rendered_count = 0;
      auto render_row = [&](int row_idx) {
        const auto &rm = history[row_idx];
        ImGui::PushID(row_idx);

        ImVec2 pos_start = ImGui::GetCursorScreenPos();

        bool is_selected = false;
        if (selection_anchor_idx_ != -1 && selection_active_idx_ != -1) {
          int sel_start =
              std::min(selection_anchor_idx_, selection_active_idx_);
          int sel_end = std::max(selection_anchor_idx_, selection_active_idx_);
          is_selected = (row_idx >= sel_start && row_idx <= sel_end);
        }

        double row_h = row_height_cache_[row_idx];
        if (row_h < 0)
          row_h = (double)ImGui::GetTextLineHeightWithSpacing();

        char label[32];
        snprintf(label, sizeof(label), "##row_%d", row_idx);
        if (ImGui::Selectable(label, is_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, (float)row_h))) {
        }
        if (rm.sender_slot != -1 && ImGui::IsItemHovered()) {
          std::string game = ap_network_.ResolvePlayerGame(rm.sender_slot);
          if (!game.empty()) {
            ImGui::SetTooltip("Game: %s", game.c_str());
          }
        }
        if (ImGui::IsItemClicked(0)) {
          if (ImGui::GetIO().KeyShift && selection_anchor_idx_ != -1)
            selection_active_idx_ = row_idx;
          else {
            if (selection_anchor_idx_ == row_idx &&
                selection_active_idx_ == row_idx) {
              selection_anchor_idx_ = -1;
              selection_active_idx_ = -1;
            } else {
              selection_anchor_idx_ = row_idx;
              selection_active_idx_ = row_idx;
            }
          }
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDown(0))
          selection_active_idx_ = row_idx;

        ImGui::SetCursorScreenPos(pos_start);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        ImGui::BeginGroup();

        // Timestamp
        const std::tm *tm_ptr = &rm.local_time;
        char time_buf[64];
        char *time_ptr = nullptr;
        if (settings_.show_chat_timestamps) {
          if (show_date) {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_long.c_str(), tm_ptr);
          } else {
            std::strftime(time_buf, sizeof(time_buf),
                          settings_.timestamp_format_short.c_str(), tm_ptr);
          }
          time_ptr = time_buf;
        }

        const std::set<int> &my_slots = ap_network_.GetConnectedSlots();
        RenderRichMessageWrapped(time_ptr, rm.parts, &ap_network_, &my_slots);
        ImGui::EndGroup();
        ImVec2 item_size = ImGui::GetItemRectSize();
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);

        if (settings_.shade_alternating_rows && row_idx % 2 == 1) {
          float x_min =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
          float x_max =
              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(x_min, pos_start.y),
              ImVec2(x_max, pos_start.y + item_size.y),
              ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
        }
        double h =
            (double)item_size.y + (double)ImGui::GetStyle().ItemSpacing.y;
        if (row_height_cache_[row_idx] < 0) {
          measured_height_sum_ += h;
          measured_rows_count_++;
        }
        row_height_cache_[row_idx] = h;

        if (row_idx == (int)history.size() - 1) {
          actual_bottom_y = (double)ImGui::GetCursorPosY();
        }

        if (ImGui::BeginPopupContextItem("ChatLineCtx",
                                         ImGuiPopupFlags_MouseButtonRight)) {
          if (selection_anchor_idx_ == -1) {
            selection_anchor_idx_ = row_idx;
            selection_active_idx_ = row_idx;
          }
          if (ImGui::MenuItem("Copy selection")) {
            std::string selected_text;
            int sel_start = std::max(
                0, std::min(selection_anchor_idx_, selection_active_idx_));
            int sel_end = std::min(
                (int)history.size() - 1,
                std::max(selection_anchor_idx_, selection_active_idx_));
            for (int k = sel_start; k <= sel_end && k < (int)history.size();
                 ++k) {
              for (const auto &p : history[k].parts)
                selected_text += p.text;
              if (k < sel_end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Copy selection (with timestamps)")) {
            std::string selected_text;
            int sel_start = std::max(
                0, std::min(selection_anchor_idx_, selection_active_idx_));
            int sel_end = std::min(
                (int)history.size() - 1,
                std::max(selection_anchor_idx_, selection_active_idx_));
            for (int k = sel_start; k <= sel_end && k < (int)history.size();
                 ++k) {
              const auto &rm_k = history[k];
              const std::tm *tm_ptr_k = &rm_k.local_time;
              char t_buf[64];
              if (show_date) {
                std::strftime(t_buf, sizeof(t_buf),
                              settings_.timestamp_format_long.c_str(),
                              tm_ptr_k);
              } else {
                std::strftime(t_buf, sizeof(t_buf),
                              settings_.timestamp_format_short.c_str(),
                              tm_ptr_k);
              }
              selected_text += t_buf;
              selected_text += " ";
              for (const auto &p : rm_k.parts)
                selected_text += p.text;
              if (k < sel_end)
                selected_text += "\n";
            }
            ImGui::SetClipboardText(selected_text.c_str());
          }
          if (ImGui::MenuItem("Clear Selection")) {
            selection_anchor_idx_ = -1;
            selection_active_idx_ = -1;
          }
          ImGui::EndPopup();
        }
        ImGui::PopID();
      };

      if (history_count > 0) {
        // 1. Precise Viewport Calculation using Row Height Cache
        int vis_start = 0;
        int vis_end = 0;
        double scroll_y = (double)ImGui::GetScrollY();
        double window_h = (double)ImGui::GetWindowHeight();

        // Optimized binary viewport search (O(log N))
        auto it_start = std::lower_bound(cumulative_heights_.begin(),
                                         cumulative_heights_.end(), scroll_y);
        vis_start = std::clamp(
            (int)std::distance(cumulative_heights_.begin(), it_start) - 1, 0,
            history_count - 1);

        auto it_end =
            std::lower_bound(cumulative_heights_.begin() + vis_start,
                             cumulative_heights_.end(), scroll_y + window_h);
        vis_end =
            std::clamp((int)std::distance(cumulative_heights_.begin(), it_end),
                       0, history_count);

        clipper.Begin(history_count, (float)avg_h);

        // 2. Apply Buffers (30 above, 50 below)
        clipper.IncludeItemsByIndex(
            std::clamp(vis_start - 30, 0, history_count),
            std::clamp(vis_end + 50, 0, history_count));

        // 3. Absolute Boundary safety
        clipper.IncludeItemsByIndex(0, std::clamp(20, 0, history_count));
        if (force_bottom_render) {
          clipper.IncludeItemsByIndex(
              std::clamp(history_count - 128, 0, history_count), history_count);
        }

        int lowest_rendered_idx = -1;
        double lowest_rendered_y = -1.0;
        rendered_count = 0;

        while (clipper.Step()) {
          // Force alignment for this range (Overrides clipper's internal
          // inaccurate skip)
          ImGui::SetCursorPosY(
              (float)cumulative_heights_[clipper.DisplayStart]);

          for (int row_idx = clipper.DisplayStart; row_idx < clipper.DisplayEnd;
               ++row_idx) {
            render_row(row_idx);
            if (row_idx > lowest_rendered_idx) {
              lowest_rendered_idx = row_idx;
              lowest_rendered_y = (double)ImGui::GetCursorPosY();
            }
            rendered_count++;
          }
        }

        // Part 23/27/28/29: Deterministic LACH anchored to prefix sum
        if (lowest_rendered_idx >= 0) {
          double convergent_total = lowest_rendered_y;
          // The rest of the list comes from the prefix sum delta
          convergent_total += (cumulative_heights_[history_count] -
                               cumulative_heights_[lowest_rendered_idx + 1]);

          if (std::abs(convergent_total - total_content_height) > 0.001) {
            total_content_height = convergent_total;
          }
        }
      }

      // Part 24/27/30/31: Threshold-based Stability (Hysteresis)
      // Only apply stabilization if NOT locked to bottom.
      // At the bottom, we want the layout to converge immediately to the true
      // MaxY.
      double stability_threshold = 2.0 * avg_h;
      if (!locked_to_bottom_ && history.size() == last_history_size_ &&
          current_window_width == last_window_width_ &&
          std::abs(total_content_height - last_stable_height_) <
              stability_threshold) {
        total_content_height = last_stable_height_;
      }
      last_stable_height_ = total_content_height;

      // Part 12: Force content height to match our estimate
      ImGui::SetCursorPosY((float)total_content_height);
      ImGui::Dummy(ImVec2(0.0f, 0.0f));

      ImGui::GetWindowDrawList()->ChannelsMerge();

      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
          !ImGui::IsAnyItemHovered()) {
        selection_active_idx_ = -1;
        selection_anchor_idx_ = -1;
      }

      // Re-read scroll max because Dummy updated it
      current_scroll_max_y = (double)ImGui::GetScrollMaxY();
      if (is_any_interaction) {
        locked_to_bottom_ =
            (ImGui::GetScrollY() >= (float)current_scroll_max_y - 20.0f) ||
            ImGui::IsKeyDown(ImGuiKey_End);
      }
      if (locked_to_bottom_ && !is_any_interaction) {
        ImGui::SetScrollHereY(1.0f);
        // Ensure MaxY is up to date for telemetry
        current_scroll_max_y = (double)ImGui::GetScrollMaxY();
      }

      if (current_window_width != last_window_width_) {
        std::fill(row_height_cache_.begin(), row_height_cache_.end(), -1.0);
        last_avg_height_ = -1.0;
        measured_height_sum_ = 0;
        measured_rows_count_ = 0;
      }

      last_history_size_ = history.size();
      last_scroll_max_y_ = current_scroll_max_y;
      last_window_width_ = current_window_width;

      if (ap_network_.IsScrollStatsEnabled()) {
        double cur_y = (double)ImGui::GetScrollY();
        double cur_h = (double)ImGui::GetWindowHeight();
        if (cur_y != (double)last_reported_scroll_y_ ||
            current_scroll_max_y != last_reported_scroll_max_y_ ||
            cur_h != (double)last_reported_window_h_ ||
            locked_to_bottom_ != last_reported_locked_) {
          char msg[512];
          snprintf(msg, sizeof(msg),
                   "[Chat Scroll] Y=%.1f MaxY=%.1f H=%.1f EstH=%.1f "
                   "Items=%d/%d Locked=%d Int=%d",
                   cur_y, current_scroll_max_y, cur_h, total_content_height,
                   rendered_count, (int)history.size(), (int)locked_to_bottom_,
                   (int)is_any_interaction);
          std::cout << msg << std::endl;

          last_reported_scroll_y_ = cur_y;
          last_reported_scroll_max_y_ = current_scroll_max_y;
          last_reported_window_h_ = cur_h;
          last_reported_locked_ = locked_to_bottom_;
        }
      }

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
      ImGui::EndDisabled();
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

      ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                        ImGuiInputTextFlags_CallbackHistory |
                                        ImGuiInputTextFlags_CallbackAlways;
      // Only claim Tab when there is something to complete, so it keeps its
      // normal focus-navigation behavior the rest of the time.
      if (ac_active_ ||
          (hc_kind_ != HintCompleteKind::None && !hc_matches_.empty()))
        input_flags |= ImGuiInputTextFlags_CallbackCompletion;

      // Autocomplete logic remains mostly same but uses selected slot's
      // player names
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

      if (hc_kind_ != HintCompleteKind::None && !hc_matches_.empty()) {
        bool truncated = hc_total_ > (int)hc_matches_.size();
        int rows = (int)hc_matches_.size() + (truncated ? 1 : 0);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        pos.y -= (rows * ImGui::GetTextLineHeightWithSpacing()) +
                 ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(600, 400));
        if (ImGui::Begin("HintCompPopup", nullptr,
                         ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
          for (int i = 0; i < (int)hc_matches_.size(); ++i) {
            if (ImGui::Selectable(hc_matches_[i].c_str(),
                                  i == hc_selected_idx_)) {
              // The hint argument runs to end of line, so replacing from the
              // argument offset onward is the whole of it.
              std::string buf_str(input_buf_);
              if ((int)buf_str.size() >= hc_arg_pos_) {
                buf_str = buf_str.substr(0, hc_arg_pos_) + hc_matches_[i];
                snprintf(input_buf_, sizeof(input_buf_), "%s",
                         buf_str.c_str());
                hc_applied_ = hc_matches_[i];
                hc_selected_idx_ = i;
                hc_cycled_ = true;
                // Refocusing re-seeds InputText's edit state from the buffer
                // we just rewrote behind its back.
                focus_input_ = true;
              }
            }
          }
          if (truncated) {
            ImGui::BeginDisabled();
            ImGui::Text("... %d more, keep typing to narrow",
                        hc_total_ - (int)hc_matches_.size());
            ImGui::EndDisabled();
          }
          ImGui::End();
        }
      }

      float button_width = ImGui::CalcTextSize("Send").x +
                           ImGui::GetStyle().FramePadding.x * 2.0f;
      float input_width = ImGui::GetContentRegionAvail().x - button_width -
                          ImGui::GetStyle().ItemSpacing.x;
      if (focus_input_) {
        ImGui::SetKeyboardFocusHere(0);
        focus_input_ = false;
      }
      ImGui::SetNextItemWidth(input_width);
      bool send = ImGui::InputText(
          "##Input", input_buf_, sizeof(input_buf_), input_flags,
          &ChatWindow::TextEditCallbackStub, (void *)this);
      ImGui::SameLine();
      if (ImGui::Button("Send"))
        send = true;

      if (send && input_buf_[0] != '\0' && !connected_slots.empty()) {
        if (!HandleCommand(input_buf_)) {
          ap_network_.SendChat(selected_send_slot_name_, input_buf_);
        }
        if (input_history_.empty() || input_history_.back() != input_buf_)
          input_history_.push_back(input_buf_);
        history_pos_ = -1;
        ac_active_ = false;
        ResetHintCompletion();
        input_buf_[0] = '\0';
        focus_input_ = true;
      }
    }
  }
  ImGui::End();
}

void ChatWindow::ResetHintCompletion() {
  hc_kind_ = HintCompleteKind::None;
  hc_arg_pos_ = -1;
  hc_selected_idx_ = 0;
  hc_total_ = 0;
  hc_cycled_ = false;
  hc_navigated_ = false;
  hc_stem_.clear();
  hc_slot_.clear();
  hc_common_.clear();
  hc_applied_.clear();
  hc_matches_.clear();
}

void ChatWindow::ApplyHintCompletion(ImGuiInputTextCallbackData *data,
                                     const std::string &text) {
  data->DeleteChars(hc_arg_pos_, data->CursorPos - hc_arg_pos_);
  data->InsertChars(data->CursorPos, text.c_str());
  hc_applied_ = text;
}

void ChatWindow::UpdateHintCompletion(ImGuiInputTextCallbackData *data) {
  // "!hint_location" has to be tested first -- "!hint" is a prefix of it.
  HintCompleteKind kind = HintCompleteKind::Location;
  int arg_pos = HintArgOffset(data->Buf, data->BufTextLen, "!hint_location");
  if (arg_pos < 0) {
    kind = HintCompleteKind::Item;
    arg_pos = HintArgOffset(data->Buf, data->BufTextLen, "!hint");
  }
  // Caret behind the argument means the user is editing the command itself.
  if (arg_pos < 0 || data->CursorPos < arg_pos) {
    ResetHintCompletion();
    return;
  }

  std::string stem(data->Buf + arg_pos, data->CursorPos - arg_pos);

  // CallbackAlways fires every frame, not just on keystrokes, and the scan
  // below walks every name in the game. Only redo it when the input actually
  // changed, or a large game would burn a full pass per frame.
  if (hc_kind_ == kind && hc_arg_pos_ == arg_pos &&
      hc_slot_ == selected_send_slot_name_ &&
      (stem == hc_stem_ ||
       // Tab rewrites the argument in place while cycling. Recognize our own
       // insertion and keep the list built from what the user actually typed,
       // instead of collapsing it to the single candidate just inserted.
       (!hc_applied_.empty() && stem == hc_applied_)))
    return;

  hc_kind_ = kind;
  hc_arg_pos_ = arg_pos;
  hc_slot_ = selected_send_slot_name_;
  hc_stem_ = stem;
  hc_applied_.clear();
  hc_common_.clear();
  hc_matches_.clear();
  hc_selected_idx_ = 0;
  hc_total_ = 0;
  hc_cycled_ = false;
  hc_navigated_ = false;

  auto session = ap_network_.GetSession(selected_send_slot_name_);
  if (!session || !session->IsDataPackageReceived())
    return;

  std::vector<std::string> prefix_hits, substring_hits;
  {
    // Metadata is refreshed on the network thread; GetStateMutex() is
    // recursive, so taking it here is safe even under an outer hold.
    std::lock_guard<std::recursive_mutex> lock(ap_network_.GetStateMutex());
    auto meta = session->GetMetadata();
    if (!meta)
      return;
    // Both commands resolve against the sending slot's own game.
    const auto &table = (kind == HintCompleteKind::Item) ? meta->item_names
                                                         : meta->location_names;
    auto game_it = table.find(session->ResolvePlayerGame());
    if (game_it == table.end())
      return;

    const std::string needle = LowerCopy(stem);
    for (const auto &[id, name] : game_it->second) {
      if (needle.empty()) {
        prefix_hits.push_back(name);
        continue;
      }
      std::string lname = LowerCopy(name);
      if (lname.rfind(needle, 0) == 0)
        prefix_hits.push_back(name);
      else if (lname.find(needle) != std::string::npos)
        substring_hits.push_back(name);
    }
  }

  // Names that start with what was typed rank above ones that merely contain
  // it, since that is what the typist was most likely reaching for.
  std::sort(prefix_hits.begin(), prefix_hits.end());
  std::sort(substring_hits.begin(), substring_hits.end());
  hc_matches_ = std::move(prefix_hits);
  hc_matches_.insert(hc_matches_.end(), substring_hits.begin(),
                     substring_hits.end());

  hc_total_ = (int)hc_matches_.size();
  // Computed before truncating: completing to a prefix shared by only the
  // visible slice would insert text the hidden candidates disagree with.
  hc_common_ = CommonPrefixCI(hc_matches_);
  if (hc_total_ > kMaxHintRows)
    hc_matches_.resize(kMaxHintRows);
}

int ChatWindow::TextEditCallbackStub(ImGuiInputTextCallbackData *data) {
  return ((ChatWindow *)data->UserData)->TextEditCallback(data);
}

int ChatWindow::TextEditCallback(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
    UpdateHintCompletion(data);
    if (hc_kind_ != HintCompleteKind::None) {
      // One popup at a time: an '@' inside an item name shouldn't pull up the
      // player list on top of the hint candidates.
      ac_active_ = false;
      return 0;
    }
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
    if (hc_kind_ != HintCompleteKind::None && !hc_matches_.empty()) {
      // Shell-style: first fill in as much as every candidate agrees on, and
      // only once that adds nothing do repeated Tabs cycle the candidates.
      // Prefix extension is only ever the opening move: once something has
      // been inserted, or the arrows have picked a row, Tab must not yank the
      // argument back to a shared prefix.
      if (hc_applied_.empty() && !hc_navigated_ &&
          hc_common_.size() > hc_stem_.size()) {
        ApplyHintCompletion(data, hc_common_);
        hc_stem_ = hc_common_;
        hc_cycled_ = false;
      } else {
        // Arrows select a row outright; Tab on its own walks to the next.
        if (hc_cycled_ && !hc_navigated_)
          hc_selected_idx_ = (hc_selected_idx_ + 1) % (int)hc_matches_.size();
        hc_cycled_ = true;
        hc_navigated_ = false;
        ApplyHintCompletion(data, hc_matches_[hc_selected_idx_]);
      }
    } else if (ac_active_ && !ac_matches_.empty()) {
      data->DeleteChars(ac_cursor_pos_, data->CursorPos - ac_cursor_pos_);
      data->InsertChars(data->CursorPos, ac_matches_[ac_selected_idx_].c_str());
      data->InsertChars(data->CursorPos, " ");
      ac_active_ = false;
    }
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    if (hc_kind_ != HintCompleteKind::None && !hc_matches_.empty()) {
      if (data->EventKey == ImGuiKey_UpArrow) {
        if (--hc_selected_idx_ < 0)
          hc_selected_idx_ = (int)hc_matches_.size() - 1;
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (++hc_selected_idx_ >= (int)hc_matches_.size())
          hc_selected_idx_ = 0;
      }
      hc_navigated_ = true;
    } else if (ac_active_) {
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

bool ChatWindow::HandleCommand(const std::string &line) {
  if (line.empty() || line[0] != '/')
    return false;

  std::string full_cmd = line.substr(1);
  size_t space = full_cmd.find(' ');
  std::string cmd =
      (space == std::string::npos) ? full_cmd : full_cmd.substr(0, space);
  std::string args =
      (space == std::string::npos) ? "" : full_cmd.substr(space + 1);

  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

  if (cmd == "help") {
    ap_network_.OnStatusMessage(nullptr, "Local commands:");
    ap_network_.OnStatusMessage(nullptr, "  /help          - Show this help");
    ap_network_.OnStatusMessage(nullptr,
                                "  /clear         - Clear chat history");
    ap_network_.OnStatusMessage(nullptr,
                                "  /disconnect    - Disconnect selected slot");
    ap_network_.OnStatusMessage(nullptr,
                                "  /disconnectall - Disconnect all slots");
    ap_network_.OnStatusMessage(nullptr,
                                "  /say <message> - Send a message in chat");
    return true;
  } else if (cmd == "clear") {
    ap_network_.ClearChatHistory();
    ap_network_.OnStatusMessage(nullptr, "Chat history cleared.");
    return true;
  } else if (cmd == "disconnect") {
    if (!selected_send_slot_name_.empty()) {
      auto s = ap_network_.GetSession(selected_send_slot_name_);
      if (s) {
        s->Disconnect();
        ap_network_.OnStatusMessage(nullptr, "Disconnected slot: " +
                                                 selected_send_slot_name_);
      }
    } else {
      ap_network_.OnStatusMessage(nullptr, "No slot selected to disconnect.");
    }
    return true;
  } else if (cmd == "disconnectall") {
    ap_network_.DisconnectAll();
    ap_network_.OnStatusMessage(nullptr, "Disconnected all slots.");
    return true;
  } else if (cmd == "say") {
    if (!args.empty()) {
      ap_network_.SendChat(selected_send_slot_name_, args);
    }
    return true;
  } else if (cmd == "debug") {
    std::string subcmd;
    size_t sub_space = args.find(' ');
    if (sub_space != std::string::npos) {
      subcmd = args.substr(0, sub_space);
      args = args.substr(sub_space + 1);
    } else {
      subcmd = args;
      args = "";
    }
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::tolower);

    int n = 0;
    try {
      n = std::stoi(args);
    } catch (...) {
      n = 1;
    }

    if (ap_network_.IsDebugMode()) {
      std::cout << "[Debug] subcmd: " << subcmd << ", args: " << args
                << ", n: " << n << std::endl;
    }

    if (subcmd == "scrollstats") {
      bool active = !ap_network_.IsScrollStatsEnabled();
      ap_network_.SetScrollStatsEnabled(active);
      std::cout << "Scroll debug stats: " << (active ? "ENABLED" : "DISABLED")
                << std::endl;
      return true;
    }

    if (subcmd == "fillchat") {
      for (int i = 0; i < n; ++i) {
        ap_network_.OnStatusMessage(nullptr,
                                    "debug filler " + std::to_string(i + 1));
      }
      return true;
    } else if (subcmd == "fillfeed") {
      int n = 1;
      int d = 0;
      {
        std::stringstream ss(args);
        std::string part;
        if (ss >> part)
          try {
            n = std::stoi(part);
          } catch (...) {
          }
        if (ss >> part)
          try {
            d = std::stoi(part);
          } catch (...) {
          }
      }

      std::string slot_name = selected_send_slot_name_.empty()
                                  ? "Player"
                                  : selected_send_slot_name_;
      int slot_id = -1;
      if (!selected_send_slot_name_.empty()) {
        if (auto session = ap_network_.GetSession(selected_send_slot_name_)) {
          slot_id = (session->GetTeam() << 16) | session->GetLocalSlot();
        }
      }

      auto generate_fill = [this, n, d, slot_name, slot_id]() {
        struct MetadataRef {
          int slot;
          std::string name;
        };
        std::vector<MetadataRef> players;
        std::vector<std::pair<int64_t, std::string>> items;
        std::vector<std::pair<int64_t, std::string>> locations;

        {
          std::lock_guard<std::recursive_mutex> lock(
              ap_network_.GetStateMutex());
          for (const auto &session : ap_network_.GetSessions()) {
            auto meta = session->GetMetadata();
            if (meta && meta->data_package_received) {
              for (auto const &[id, name] : meta->player_names)
                players.push_back({id, name});
              for (auto const &[game, item_map] : meta->item_names)
                for (auto const &[id, name] : item_map)
                  items.push_back({id, name});
              for (auto const &[game, loc_map] : meta->location_names)
                for (auto const &[id, name] : loc_map)
                  locations.push_back({id, name});
              if (!players.empty())
                break;
            }
          }
        }

        std::random_device rd;
        std::mt19937 gen(rd());

        for (int i = 0; i < n; ++i) {
          RichMessage rm;
          rm.timestamp =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count() /
              1000000.0;
          rm.populate_local_time();
          rm.source_slot = slot_name;
          rm.type = "ItemSend";

          if (!players.empty() && !items.empty() && !locations.empty()) {
            std::uniform_int_distribution<> dis_p(0, players.size() - 1);
            std::uniform_int_distribution<> dis_i(0, items.size() - 1);
            std::uniform_int_distribution<> dis_l(0, locations.size() - 1);
            std::uniform_int_distribution<> dis_type(0, 99);

            auto &p1 = players[dis_p(gen)];
            auto &item = items[dis_i(gen)];
            auto &loc = locations[dis_l(gen)];

            rm.sender_slot = p1.slot;
            rm.item_id = item.first;
            rm.location_id = loc.first;

            // Randomize item flags
            int flag_roll = dis_type(gen);
            if (flag_roll < 20)
              rm.item_flags = 0; // 20% Filler
            else if (flag_roll < 60)
              rm.item_flags = 1; // 40% Progression
            else if (flag_roll < 85)
              rm.item_flags = 2; // 25% Useful
            else
              rm.item_flags = 4; // 15% Trap

            uint32_t item_color = 0xFFFFFF00; // Filler/Default (Cyan)
            std::string item_class = "item_id item_filler";
            if (rm.item_flags & 0x01) {
              item_color = 0xFFFF5FAF; // Progression (Lavender)
              item_class = "item_id item_progression";
            } else if (rm.item_flags & 0x02) {
              item_color = 0xFFED9564; // Useful (Blue)
              item_class = "item_id item_useful";
            } else if (rm.item_flags & 0x04) {
              item_color = 0xFF0045FF; // Trap (Red-Orange)
              item_class = "item_id item_trap";
            }

            const std::set<int> &my_slots = ap_network_.GetConnectedSlots();
            auto player_class = [&](int slot) -> std::string {
              return my_slots.count(slot) ? "player_id player_self" : "player_id";
            };

            if (dis_type(gen) < 30) {
              rm.receiver_slot = p1.slot;
              rm.parts.push_back(
                  MessagePart{p1.name, 0xFFFF00FF, p1.slot, player_class(p1.slot)});
              rm.parts.push_back(
                  MessagePart{" found their ", 0xFFFFFFFF, -1, "text"});
            } else {
              auto &p2 = players[dis_p(gen)];
              rm.receiver_slot = p2.slot;
              rm.parts.push_back(
                  MessagePart{p1.name, 0xFFFF00FF, p1.slot, player_class(p1.slot)});
              rm.parts.push_back(MessagePart{" sent ", 0xFFFFFFFF, -1, "text"});
              rm.parts.push_back(
                  MessagePart{item.second, item_color, -1, item_class});
              rm.parts.push_back(MessagePart{" to ", 0xFFFFFFFF, -1, "text"});
              rm.parts.push_back(
                  MessagePart{p2.name, 0xFFFF00FF, p2.slot, player_class(p2.slot)});
              rm.parts.push_back(MessagePart{" (", 0xFFFFFFFF, -1, "text"});
            }

            if (rm.parts.size() <= 2) {
              rm.parts.push_back(
                  MessagePart{item.second, item_color, -1, item_class});
              rm.parts.push_back(MessagePart{" (", 0xFFFFFFFF, -1, "text"});
            }

            rm.parts.push_back(
                MessagePart{loc.second, 0xFF00FF00, -1, "location_id"});
            rm.parts.push_back(MessagePart{")", 0xFFFFFFFF, -1, "text"});

          } else {
            rm.sender_slot = slot_id;
            rm.receiver_slot = slot_id;

            // Randomize item flags for fallback case
            int flag_roll = i % 100;
            if (flag_roll < 20)
              rm.item_flags = 0;
            else if (flag_roll < 60)
              rm.item_flags = 1;
            else if (flag_roll < 85)
              rm.item_flags = 2;
            else
              rm.item_flags = 4;

            uint32_t item_color = 0xFFFFFF00;
            std::string item_class2 = "item_id item_filler";
            if (rm.item_flags & 0x01) {
              item_color = 0xFFFF5FAF;
              item_class2 = "item_id item_progression";
            } else if (rm.item_flags & 0x02) {
              item_color = 0xFFED9564;
              item_class2 = "item_id item_useful";
            } else if (rm.item_flags & 0x04) {
              item_color = 0xFF0045FF;
              item_class2 = "item_id item_trap";
            }

            rm.parts.push_back(MessagePart{slot_name, 0xFFFF00FF, -1,
                                           "player_id player_self"});
            rm.parts.push_back(
                MessagePart{" found their ", 0xFFFFFFFF, -1, "text"});
            rm.parts.push_back(MessagePart{"Item " + std::to_string(i + 1),
                                           item_color, -1, item_class2});
            rm.parts.push_back(MessagePart{" (", 0xFFFFFFFF, -1, "text"});
            rm.parts.push_back(MessagePart{"Location " + std::to_string(i + 1),
                                           0xFF00FF00, -1, "location_id"});
            rm.parts.push_back(MessagePart{")", 0xFFFFFFFF, -1, "text"});
          }

          ap_network_.OnGlobalMessage(nullptr, rm, true, 0, true);

          if (d > 0 && i < n - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(d));
          }
        }
      };

      if (d > 0) {
        std::thread(generate_fill).detach();
      } else {
        generate_fill();
      }
      return true;
    } else if (subcmd == "deathlink") {
      if (!selected_send_slot_name_.empty()) {
        if (auto session = ap_network_.GetSession(selected_send_slot_name_)) {
          std::string cause =
              selected_send_slot_name_ +
              " wielded the power of the Axolotl debug command.";
          session->SendDeathLink(cause);
        }
      }
      return true;
    } else if (subcmd == "goal") {
      // Synthesize a Goal RichMessage and route it through the
      // on_message_received callback. That fires the /stats goal_event
      // broadcast (and the /feed entry) without going through
      // OnGlobalMessage — so we don't pollute chat history or actually
      // mark the slot as completed.
      int sender_slot = -1;
      if (!selected_send_slot_name_.empty()) {
        if (auto session = ap_network_.GetSession(selected_send_slot_name_)) {
          if (session->IsConnected()) {
            sender_slot =
                (session->GetTeam() << 16) | session->GetLocalSlot();
          }
        }
      }
      if (sender_slot < 0) {
        for (const auto &session : ap_network_.GetSessions()) {
          if (session->IsConnected()) {
            sender_slot =
                (session->GetTeam() << 16) | session->GetLocalSlot();
            break;
          }
        }
      }
      if (sender_slot < 0) {
        ap_network_.OnStatusMessage(
            nullptr, "[/debug goal] No connected slot to goal as.");
        return true;
      }

      RichMessage rm;
      rm.type = "Goal";
      rm.sender_slot = sender_slot;
      rm.timestamp = ArchipelagoNetwork::GetCurrentTimestamp();
      rm.populate_local_time();
      std::string slot_name = ap_network_.ResolvePlayerName(sender_slot);
      rm.parts.push_back(
          {slot_name, 0xFFFF00FF, sender_slot, "player_id"});
      rm.parts.push_back(
          {" completed their goal!", 0xFFFFFFFF, -1, "text"});

      if (ap_network_.on_message_received) {
        ap_network_.on_message_received(rm);
      }
      return true;
    }
    return true; // Handle all /debug subcommands locally
  }

  ap_network_.OnStatusMessage(nullptr, "Unrecognized command: /" + cmd);
  ap_network_.OnStatusMessage(
      nullptr, "Use /help to see local commands, or /say <msg> to send a "
               "message starting with /");
  return true;
}
