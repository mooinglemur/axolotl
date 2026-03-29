#pragma once
#include "ArchipelagoNetwork.h"
#include <ctime>
#include <imgui.h>
#include <set>
#include <cctype>
#include <string>
#include <vector>

struct ConnectionSettings;

inline int NaturalCompare(const std::string &a, const std::string &b) {
  if (a.empty() && b.empty())
    return 0;
  if (a.empty())
    return -1;
  if (b.empty())
    return 1;

  auto itA = a.begin(), itB = b.begin();
  while (itA != a.end() && itB != b.end()) {
    if (isdigit(*itA) && isdigit(*itB)) {
      unsigned long numA = 0;
      while (itA != a.end() && isdigit(*itA))
        numA = numA * 10 + (*itA - '0'), ++itA;
      unsigned long numB = 0;
      while (itB != b.end() && isdigit(*itB))
        numB = numB * 10 + (*itB - '0'), ++itB;
      if (numA != numB)
        return (numA < numB) ? -1 : 1;
    } else {
      char cA = (char)tolower(*itA), cB = (char)tolower(*itB);
      if (cA != cB)
        return (cA < cB) ? -1 : 1;
      ++itA;
      ++itB;
    }
  }
  if (itA == a.end() && itB == b.end())
    return 0;
  return (itA == a.end()) ? -1 : 1;
}

inline void RenderRichMessageWrapped(const char *timestamp_str,
                                     const std::vector<MessagePart> &parts,
                                     ArchipelagoNetwork *ap_network,
                                     const std::set<int> *my_slots = nullptr) {
  if (timestamp_str && timestamp_str[0] != '\0') {
    ImGui::TextDisabled("%s", timestamp_str);
    ImGui::SameLine(0, 4.0f);
  }

  float window_visible_x2 =
      ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  float content_min_x =
      ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;

  for (size_t i = 0; i < parts.size(); ++i) {
    const auto &part = parts[i];
    uint32_t use_color = part.color;
    if (my_slots && part.player_id != -1) {
      use_color = my_slots->count(part.player_id) ? 0xFFFF00FF : 0xFFCCCCCC;
    }
    ImVec4 color = ImGui::ColorConvertU32ToFloat4(use_color);
    ImGui::PushStyleColor(ImGuiCol_Text, color);

    const char *word_start = part.text.c_str();
    const char *text_end = word_start + part.text.length();

    while (word_start < text_end) {
      const char *word_end = word_start;
      while (word_end < text_end && *word_end != ' ' && *word_end != '\n') {
        word_end++;
      }
      if (word_end < text_end && *word_end == ' ') {
        word_end++; // Include trailing space
      }

      ImVec2 word_size = ImGui::CalcTextSize(word_start, word_end);
      float next_x = ImGui::GetCursorScreenPos().x + word_size.x;

      if (next_x > window_visible_x2 &&
          ImGui::GetCursorScreenPos().x > content_min_x + 20.0f) {
        ImGui::NewLine();
      }

      ImGui::TextUnformatted(word_start, word_end);
      if (part.player_id != -1 && ap_network &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                               ImGuiHoveredFlags_AllowWhenOverlapped)) {
        std::string game = ap_network->ResolvePlayerGame(part.player_id);
        if (!game.empty()) {
          ImGui::SetTooltip("Game: %s", game.c_str());
        }
      }

      if (word_end < text_end && *word_end == '\n') {
        ImGui::NewLine();
        word_end++;
      } else if (word_start < text_end &&
                 (word_end < text_end || i < parts.size() - 1)) {
        ImGui::SameLine(0, 0);
      }
      word_start = word_end;
    }
    ImGui::PopStyleColor();
  }
}

inline bool RenderFilterInput(const char *label, std::string &filter_text,
                               bool &focus_filter) {
  char buf[256];
  strncpy(buf, filter_text.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  ImGui::SetNextItemAllowOverlap();
  if (focus_filter) {
    ImGui::SetKeyboardFocusHere(0);
    focus_filter = false;
  }

  bool changed = false;
  if (ImGui::InputText(label, buf, sizeof(buf))) {
    filter_text = buf;
    changed = true;
  }

  if (!filter_text.empty()) {
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    float size = max.y - min.y;
    ImVec2 button_pos = ImVec2(max.x - size, min.y);
    ImGui::SetCursorScreenPos(button_pos);
    if (ImGui::InvisibleButton("##Clear", ImVec2(size, size))) {
      filter_text.clear();
      focus_filter = true;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImVec2 center =
        ImVec2(button_pos.x + size * 0.5f, button_pos.y + size * 0.5f);
    float sz = size * 0.2f;
    ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(center.x - sz, center.y - sz),
                                        ImVec2(center.x + sz, center.y + sz),
                                        color, 2.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(center.x - sz, center.y + sz),
                                        ImVec2(center.x + sz, center.y - sz),
                                        color, 2.0f);
  }
  return changed;
}

class Window {
public:
  Window(const std::string &name) : name_(name) {}
  virtual ~Window() = default;

  virtual void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
                      ImFont *preview_font = nullptr,
                      ImFont *preview_fallback_font = nullptr) = 0;

  virtual void SaveState(ConnectionSettings &settings) {}

  const std::string &GetName() const { return name_; }
  bool &GetOpen() { return is_open_; }
  void SetOpen(bool open) { is_open_ = open; }

protected:
  std::string name_;
  bool is_open_ = true;
};
