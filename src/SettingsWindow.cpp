#include "SettingsWindow.h"
#include <algorithm>
#include <imgui.h>

SettingsWindow::SettingsWindow(
    const ConnectionSettings &settings,
    std::function<void(const ConnectionSettings &)> on_save,
    std::function<void(const std::string &)> on_preview,
    std::function<void(const std::string &)> on_fallback_preview,
    const std::string &name)
    : Window(name), settings_(settings), on_save_(on_save),
      on_preview_(on_preview), on_fallback_preview_(on_fallback_preview) {
  available_fonts_ = FontScanner::GetAvailableFonts();

  // Load initial previews if fonts are already selected
  if (!settings_.font_path.empty() && on_preview_) {
    on_preview_(settings_.font_path);
  }
  if (!settings_.fallback_font_path.empty() && on_fallback_preview_) {
    on_fallback_preview_(settings_.fallback_font_path);
  }
}

void SettingsWindow::Render(ImFont *custom_font, ImFont *preview_font,
                            ImFont *preview_fallback_font) {
  bool opening = !was_open_ && is_open_;
  bool closing = was_open_ && !is_open_;
  was_open_ = is_open_;

  if (opening) {
    initial_settings_ = settings_;
    saved_ = false;
  }

  if (closing && !saved_) {
    settings_ = initial_settings_;
    if (on_save_) {
      on_save_(settings_);
    }
  }

  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("UI Preferences");
    ImGui::Separator();

    auto render_scale_slider = [&](const char *label, float *value,
                                   const char *tooltip, bool is_ui_scale) {
      ImGui::PushID(label);
      if (ImGui::SliderFloat(label, value, 0.5f, 3.0f, "%.2f")) {
        // Live feedback for UI scale if using FontGlobalScale
        // if (is_ui_scale) ImGui::GetIO().FontGlobalScale = *value;
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (on_save_)
          on_save_(settings_);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);

      if (ImGui::BeginPopupContextItem("##ScaleContextMenu")) {
        if (ImGui::IsWindowAppearing())
          ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputFloat("Value", value, 0.01f, 0.1f, "%.2f")) {
          // value changed via input
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          if (on_save_)
            on_save_(settings_);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    };

    render_scale_slider("UI Scale", &settings_.ui_scale,
                        "Adjust the global size of the UI fonts.", true);
    render_scale_slider(
        "Content Scale", &settings_.content_scale,
        "Adjust the size of the main content font (Chat, Items, "
        "etc.).",
        false);

    ImGui::Checkbox("Use HiDPI Scaling", &settings_.use_hidpi);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Enable high-resolution coordinate scaling.");

    ImGui::InputInt("Max Feed History", &settings_.max_history_size, 100, 1000);
    if (settings_.max_history_size < 0)
      settings_.max_history_size = 0;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Maximum number of lines to retain in the Item feed (0 "
                        "= Unlimited).");

    ImGui::Text("Current Font: %s", settings_.font_path.empty()
                                        ? "Default"
                                        : settings_.font_path.c_str());
    ImGui::SameLine();
    if (!settings_.font_path.empty()) {
      if (ImGui::Button("Clear")) {
        settings_.font_path = "";
        if (on_preview_) {
          on_preview_("");
        }
        if (on_save_) {
          on_save_(settings_);
        }
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Revert to default UI font");
    }

    ImGui::Text("Content Font");
    ImGui::PushItemWidth(-1.0f);
    ImGui::InputText("##FilterFont", font_search_, sizeof(font_search_));
    ImGui::PopItemWidth();

    if (ImGui::BeginChild("FontList", ImVec2(0, 150), true)) {
      for (const auto &font : available_fonts_) {
        // Simple case-insensitive search
        std::string search = font_search_;
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);
        std::string name = font.name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        if (search.empty() || name.find(search) != std::string::npos) {
          bool is_valid = font.is_valid;
          bool is_selected = (settings_.font_path == font.name ||
                              settings_.font_path == font.path);

          std::string label = font.name;
          if (!is_valid) {
            label += " [Unsupported]";
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
          }

          if (ImGui::Selectable(label.c_str(), is_selected)) {
            if (FontScanner::IsValidFontFile(font.path)) {
              settings_.font_path = font.path;
              if (on_preview_) {
                on_preview_(font.path);
              }
            } else {
              // Mark as invalid so it shows the warning next frame
              const_cast<AxolotlFontInfo &>(font).is_valid = false;
            }
          }

          if (!is_valid) {
            ImGui::PopStyleColor();
          }

          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", font.path.c_str());
          }
        }
      }
    }
    ImGui::EndChild();
    RenderContentFontPreview(preview_font ? preview_font : custom_font);

    ImGui::Spacing();
    ImGui::Text("Fallback Font (for CJK and/or Emoji)");
    ImGui::Text("Current Fallback: %s",
                settings_.fallback_font_path.empty()
                    ? "None"
                    : settings_.fallback_font_path.c_str());
    if (!settings_.fallback_font_path.empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Clear##Fallback")) {
        settings_.fallback_font_path = "";
        if (on_fallback_preview_) {
          on_fallback_preview_("");
        }
        if (on_save_) {
          on_save_(settings_);
        }
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clear fallback font");
    }
    ImGui::PushItemWidth(-1.0f);
    ImGui::InputText("##FilterFallbackFont", fallback_font_search_,
                     sizeof(fallback_font_search_));
    ImGui::PopItemWidth();

    if (ImGui::BeginChild("FallbackFontList", ImVec2(0, 150), true)) {
      for (const auto &font : available_fonts_) {
        std::string search = fallback_font_search_;
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);
        std::string name = font.name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        if (search.empty() || name.find(search) != std::string::npos) {
          bool is_valid = font.is_valid;
          bool is_selected = (settings_.fallback_font_path == font.name ||
                              settings_.fallback_font_path == font.path);

          std::string label = font.name + "##Fallback";
          if (!is_valid) {
            label = font.name + " [Unsupported]##Fallback";
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
          }

          if (ImGui::Selectable(label.c_str(), is_selected)) {
            if (FontScanner::IsValidFontFile(font.path)) {
              settings_.fallback_font_path = font.path;
              if (on_fallback_preview_) {
                on_fallback_preview_(font.path);
              }
            } else {
              // Mark as invalid so it shows the warning next frame
              const_cast<AxolotlFontInfo &>(font).is_valid = false;
            }
          }

          if (!is_valid) {
            ImGui::PopStyleColor();
          }

          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", font.path.c_str());
          }
        }
      }
    }
    ImGui::EndChild();
    RenderFallbackFontPreview(preview_fallback_font ? preview_fallback_font
                                                    : custom_font);

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save and Apply")) {
      saved_ = true;
      if (on_save_) {
        on_save_(settings_);
      }
      is_open_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      is_open_ = false;
    }
  }
  ImGui::End();
}

void SettingsWindow::RenderContentFontPreview(ImFont *preview_font) {
  ImGui::Spacing();
  ImGui::Text("Content Font Preview:");
  ImGui::Spacing();
  if (preview_font) {
    ImGui::PushFont(preview_font);
    ImGui::TextWrapped("%s", "The quick brown fox jumps over the lazy dog.\n"
                             "0123456789 !@#$%^&*()\n"
                             "ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
                             "abcdefghijklmnopqrstuvwxyz");
    ImGui::PopFont();
  } else {
    ImGui::TextDisabled("(Select a font to see preview)");
  }
  ImGui::Spacing();
  ImGui::Separator();
}

void SettingsWindow::RenderFallbackFontPreview(ImFont *preview_fallback_font) {
  ImGui::Spacing();
  ImGui::Text("Fallback Font Preview:");
  ImGui::Spacing();
  if (preview_fallback_font) {
    ImGui::PushFont(preview_fallback_font);
    ImGui::TextWrapped("%s", "你好 (Hello) | こんにちは (Hello)\n"
                             "Emoji: \xF0\x9F\x98\x80 \xF0\x9F\x8E\xAE");
    ImGui::PopFont();
  } else {
    ImGui::TextDisabled("(Select a fallback font to see preview)");
  }
  ImGui::Spacing();
  ImGui::Separator();
}
