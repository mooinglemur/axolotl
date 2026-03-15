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
                 std::function<void(const std::string &)> on_fallback_preview,
                 const std::string &name = "Settings");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  void RenderContentFontPreview(ImFont *preview_font);
  void RenderFallbackFontPreview(ImFont *preview_fallback_font);

  ConnectionSettings settings_;
  ConnectionSettings initial_settings_;
  bool was_open_ = false;
  bool saved_ = false;
  std::function<void(const ConnectionSettings &)> on_save_;
  std::function<void(const std::string &)> on_preview_;
  std::function<void(const std::string &)> on_fallback_preview_;
  std::vector<AxolotlFontInfo> available_fonts_;
  char font_search_[128] = "";
  char fallback_font_search_[128] = "";
};
