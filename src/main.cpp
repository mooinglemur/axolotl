#include "Application.h"
#include <iostream>

int main() {
  Application app;

  if (!app.Initialize()) {
    std::cerr << "Failed to initialize Axolotl Application." << std::endl;
    return 1;
  }

  app.Run();
  return 0;
}
