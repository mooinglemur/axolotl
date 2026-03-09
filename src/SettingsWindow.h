#pragma once
#include "Config.h"
#include "FontScanner.h"
#include "Window.h"
#include <functional>
#include <vector>

class SettingsWindow : public Window {
public:
  SettingsWindow(const ConnectionSettings &settings,
                 std::function<void(const ConnectionSettings &)> on_save,
                 std::function<void(const std::string &)> on_preview,
                 const std::string &name = "Settings");
  void Render(ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr) override;
  void RenderPreview(ImFont *preview_font);

private:
  ConnectionSettings settings_;
  std::function<void(const ConnectionSettings &)> on_save_;
  std::function<void(const std::string &)> on_preview_;
  std::vector<FontInfo> available_fonts_;
  char font_search_[128] = "";
};
