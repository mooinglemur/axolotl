#include "Application.h"
#include "Config.h"
#include "Platform.h"
#include "ProfileLock.h"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

// Returns the explicit --profile=NAME / --profile NAME from argv, or empty
// string if no such flag was passed.
std::string ParseExplicitProfileName(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (std::strncmp(a, "--profile=", 10) == 0)
      return std::string(a + 10);
    if (std::strcmp(a, "--profile") == 0 && i + 1 < argc)
      return std::string(argv[i + 1]);
  }
  return "";
}

// When no flag is given, pick the most recently used profile. Falls back to
// "default" when no profiles exist (first run after EnsureMigrated).
std::string PickMostRecentProfile() {
  auto profiles = Config::ListProfiles();
  if (profiles.empty())
    return "default";
  return profiles.front().name;
}

} // namespace

int main(int argc, char **argv) {
#ifndef _WIN32
  setenv("QT_LOGGING_RULES", "qt.qpa.services=false", 1);
  std::signal(SIGINT, Application::SignalHandler);
  std::signal(SIGTERM, Application::SignalHandler);
#endif

  std::string profile_name = ParseExplicitProfileName(argc, argv);
  bool profile_was_explicit = !profile_name.empty();
  if (profile_was_explicit && !Config::ValidateProfileName(profile_name)) {
    std::cerr << "Invalid profile name: '" << profile_name
              << "'. Allowed: [A-Za-z0-9_-]{1,64}" << std::endl;
    return 2;
  }

  // Migrate pre-profile state into profiles/default/ on first run after
  // upgrade. Idempotent.
  Config::EnsureMigrated();

  // No --profile flag → resume the most recently used profile.
  if (!profile_was_explicit)
    profile_name = PickMostRecentProfile();

  // Set tentatively so any subsequent Config::GetConfigDir() resolves
  // against the right directory.
  Config::SetActiveProfile(profile_name);

  // Auto-create the profile directory if the user supplied a brand-new
  // name on the command line. The picker handles forking from in-app.
  if (!Config::ProfileExists(profile_name))
    Config::CreateProfile(profile_name, "");

  // Construct the application now so the platform render backend is
  // available for the picker (if needed). This loads the tentative
  // profile's config but doesn't start the network or any user-facing
  // state until after we own the lock.
  Application app;
  app.ParseArguments(argc, argv);

  if (!app.InitializeBackend()) {
    std::cerr << "Failed to initialize render backend." << std::endl;
    return 1;
  }

  // Lock acquisition with picker loop.
  while (true) {
    auto lock = std::make_unique<ProfileLock>(profile_name);
    if (lock->TryAcquire()) {
      Config::TouchLastUsed(profile_name);
      app.SetProfileLock(std::move(lock));
      break;
    }

    auto choice = app.ShowProfilePicker(profile_name, lock->GetLockOwnerPid(),
                                        lock->IsStale());
    if (choice.action == Application::PickerResult::Action::Quit) {
      // _Exit (not return) to skip atexit handlers. GLFW/EGL teardown
      // registered via atexit can hang inside the NVIDIA EGL driver on
      // Wayland (especially when another instance is still using the
      // driver).
      std::_Exit(0);
    }
    if (choice.action == Application::PickerResult::Action::TakeOver) {
      lock->ForceAcquire();
      Config::TouchLastUsed(profile_name);
      app.SetProfileLock(std::move(lock));
      break;
    }
    if (choice.action == Application::PickerResult::Action::SwitchTo) {
      // Nothing to save — we have not yet started network/windows. Re-exec
      // into the chosen profile. execv replaces the process image entirely
      // (kernel reclaims GLFW/Wayland resources), and atexit handlers do
      // not run.
      Platform::ReExec({"--profile=" + choice.profile_name});
      // Only reached on failure. _Exit to skip atexit (see Quit case).
      std::_Exit(1);
    }
  }

  if (!app.InitializeNetwork()) {
    std::cerr << "Failed to initialize Archipelago Network." << std::endl;
    std::_Exit(1);
  }

  if (!app.InitializeUI()) {
    std::cerr << "Failed to initialize UI." << std::endl;
    std::_Exit(1);
  }
  app.Run();

  // Do all the safe teardown explicitly — save state, stop the web
  // server, stop the network thread, join pack-loader threads, release
  // the profile lock — then _Exit() to skip atexit handlers and static
  // destructors. NVIDIA EGL teardown registered via atexit can hang or
  // crash on Wayland (especially when another instance is sharing the
  // driver), and the destructor for `app` would normally trigger that
  // teardown indirectly when the process exits.
  app.PrepareForExit();
  std::_Exit(0);
}
