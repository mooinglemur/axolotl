#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "Window.h"
#include <string>
#include <vector>

class ReceivedItemsWindow : public Window {
public:
  ReceivedItemsWindow(const std::vector<RichMessage> &history,
                      const ConnectionSettings &settings,
                      const std::string &name = "Received Items");
  void Render(ImFont *custom_font = nullptr, ImFont *preview_font = nullptr,
              ImFont *preview_fallback_font = nullptr) override;
  void SaveState(ConnectionSettings &settings) override;

private:
  int selection_anchor_ = -1;
  int selection_active_ = -1;
  bool collapse_ = false;
  const std::vector<RichMessage> &history_;
  const ConnectionSettings &settings_;
  std::string filter_text_;
};
