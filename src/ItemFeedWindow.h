#pragma once
#include "ArchipelagoNetwork.h"
#include "Window.h"
#include <functional>
#include <string>
#include <vector>

class ItemFeedWindow : public Window {
public:
  ItemFeedWindow(const std::vector<RichMessage> &history,
                 std::function<int()> get_global_slot,
                 bool personal_only = false,
                 const std::string &name = "Item Feed");
  void Render(ImFont *custom_font = nullptr,
              ImFont *preview_font = nullptr) override;

private:
  int selection_anchor_ = -1;
  int selection_active_ = -1;
  const std::vector<RichMessage> &history_;
  std::function<int()> get_global_slot_;
  bool personal_only_;
  std::string filter_text_;
};
