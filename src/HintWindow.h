#pragma once
#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

class HintWindow : public Window {
public:
  HintWindow(
      const std::vector<Hint> &hints,
      const std::map<int, std::string> &player_names,
      const std::map<std::string, std::map<int64_t, std::string>> &item_names,
      const std::map<std::string, std::map<int64_t, std::string>>
          &location_names,
      const std::map<int, std::string> &slot_to_game,
      std::function<int()> get_global_slot, const ConnectionSettings &settings,
      const std::string &name = "Hints");

  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  const std::vector<Hint> &hints_;
  const std::map<int, std::string> &player_names_;
  const std::map<std::string, std::map<int64_t, std::string>> &item_names_;
  const std::map<std::string, std::map<int64_t, std::string>> &location_names_;
  const std::map<int, std::string> &slot_to_game_;
  std::function<int()> get_global_slot_;
  const ConnectionSettings &settings_;
  std::vector<int> sorted_indices_;
  char filter_text_[256] = "";
};
