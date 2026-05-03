#pragma once
#include "Window.h"
#include <vector>

class Application;

class ChecksGraphWindow : public Window {
public:
  ChecksGraphWindow(Application &app);
  void Render(std::tm *current_tm, ImFont *custom_font, ImFont *preview_font,
              ImFont *preview_fallback_font) override;

private:
  Application &app_;
};
