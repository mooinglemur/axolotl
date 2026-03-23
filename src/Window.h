#pragma once
#include "ArchipelagoNetwork.h"
#include <ctime>
#include <imgui.h>
#include <set>
#include <string>
#include <vector>

struct ConnectionSettings;

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
