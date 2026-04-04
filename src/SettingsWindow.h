#pragma once
#include "Config.h"
#include "FontScanner.h"
#include "PackStore.h"
#include "Window.h"
#include <functional>
#include <vector>

class SettingsWindow : public Window {
public:
  SettingsWindow(const ConnectionSettings &settings,
                 std::function<void(const ConnectionSettings &)> on_save,
                 std::function<void(const std::string &)> on_preview = nullptr,
                 std::function<void(const std::string &)> on_fallback_preview = nullptr,
                 std::function<void(const std::string &)> on_remove_pack = nullptr,
                 const std::string &name = "Settings");
  void Render(std::tm *current_tm, ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr,
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
  std::function<void(const std::string &)> on_remove_pack_;
  std::vector<AxolotlFontInfo> available_fonts_;
  char font_search_[128] = "";
  char fallback_font_search_[128] = "";
  std::string selected_pack_to_remove_;
  bool show_remove_popup_ = false;
  std::vector<PackStore::PackInfo> cached_packs_;
  bool packs_cache_valid_ = false;
  bool last_packs_header_open_ = false;
};
