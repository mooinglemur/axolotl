#include "Application.h"
#include <iostream>

#include <csignal>
#include <thread>
#include <chrono>

int main(int argc, char **argv) {
#ifndef _WIN32
  setenv("QT_LOGGING_RULES", "qt.qpa.services=false", 1);
  std::signal(SIGINT, Application::SignalHandler);
  std::signal(SIGTERM, Application::SignalHandler);
#endif

  Application app;
  app.ParseArguments(argc, argv);

  if (!app.InitializeNetwork()) {
    std::cerr << "Failed to initialize Archipelago Network." << std::endl;
    return 1;
  }

  while (!app.UserRequestedExit()) {
    if (app.InitializeUI()) {
      app.Run();
      app.CleanupUI();
    } else {
      if (app.UserRequestedExit())
        break;
      std::cerr << "Failed to initialize UI. Retrying in 5 seconds..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    if (app.WasDisconnected()) {
      std::cerr << "UI connection lost. Attempting to recover..." << std::endl;
      app.ResetDisconnected();
    }
  }

  return 0;
}
