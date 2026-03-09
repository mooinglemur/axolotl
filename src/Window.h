#pragma once
#include "ArchipelagoNetwork.h"
#include <imgui.h>
#include <string>
#include <vector>

inline void RenderRichMessageWrapped(const char *timestamp_str,
                                     const std::vector<MessagePart> &parts) {
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
    ImVec4 color = ImGui::ColorConvertU32ToFloat4(part.color);
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

  virtual void Render(ImFont *custom_font = nullptr,
                      ImFont *preview_font = nullptr) = 0;

  const std::string &GetName() const { return name_; }
  bool &GetOpen() { return is_open_; }
  void SetOpen(bool open) { is_open_ = open; }

protected:
  std::string name_;
  bool is_open_ = true;
};
