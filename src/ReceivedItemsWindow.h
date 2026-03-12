#include "ArchipelagoNetwork.h"
#include "Window.h"
#include <string>
#include <vector>

class ReceivedItemsWindow : public Window {
public:
  ReceivedItemsWindow(const std::vector<RichMessage> &history,
                      const std::string &name = "Received Items");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;

private:
  int selection_anchor_ = -1;
  int selection_active_ = -1;
  const std::vector<RichMessage> &history_;
  std::string filter_text_;
};
