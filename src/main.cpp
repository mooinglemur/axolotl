#include "Application.h"
#include <iostream>

int main() {
#ifndef _WIN32
  setenv("QT_LOGGING_RULES", "qt.qpa.services=false", 1);
#endif
  Application app;

  if (!app.Initialize()) {
    std::cerr << "Failed to initialize Axolotl Application." << std::endl;
    return 1;
  }

  app.Run();
  return 0;
}
