#include "SettingsWindow.h"
#include <algorithm>
#include <imgui.h>

SettingsWindow::SettingsWindow(
    const ConnectionSettings &settings,
    std::function<void(const ConnectionSettings &)> on_save,
    std::function<void(const std::string &)> on_preview,
    const std::string &name)
    : Window(name), settings_(settings), on_save_(on_save),
      on_preview_(on_preview) {
  available_fonts_ = FontScanner::GetAvailableFonts();
}

void SettingsWindow::Render(ImFont *, ImFont *preview_font) {
  if (!is_open_)
    return;

  if (ImGui::Begin(name_.c_str(), &is_open_)) {
    ImGui::Text("UI Preferences");
    ImGui::Separator();

    ImGui::SliderFloat("UI Scale", &settings_.ui_scale, 0.5f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Adjust the global size of the UI.");

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
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Revert to default UI font");
    }

    ImGui::Text("UI Font");
    ImGui::InputText("Filter##Font", font_search_, sizeof(font_search_));

    if (ImGui::BeginChild("FontList", ImVec2(0, 150), true)) {
      for (const auto &font : available_fonts_) {
        // Simple case-insensitive search
        std::string search = font_search_;
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);
        std::string name = font.name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        if (search.empty() || name.find(search) != std::string::npos) {
          bool is_selected = (settings_.font_path == font.name ||
                              settings_.font_path == font.path);
          if (ImGui::Selectable(font.name.c_str(), is_selected)) {
            settings_.font_path =
                font.name; // Store the name, will resolve to path on Save
            if (on_preview_) {
              on_preview_(font.path);
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", font.path.c_str());
          }
        }
      }
    }
    ImGui::EndChild();

    RenderPreview(preview_font);

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save and Apply")) {
      if (on_save_) {
        on_save_(settings_);
      }
      is_open_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      is_open_ = false;
    }

    ImGui::End();
  }
}

void SettingsWindow::RenderPreview(ImFont *preview_font) {
  ImGui::Separator();
  ImGui::Text("Font Preview:");
  ImGui::Spacing();
  if (preview_font) {
    ImGui::PushFont(preview_font);
    ImGui::TextWrapped("%s", "The quick brown fox jumps over the lazy dog.\n"
                             "0123456789 !@#$%^&*()");
    ImGui::PopFont();
  } else {
    ImGui::TextDisabled("(Select a font to see preview)");
  }
  ImGui::Spacing();
  ImGui::Separator();
}
