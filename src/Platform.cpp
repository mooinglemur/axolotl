#include "Platform.h"
#include <cstdlib>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace Platform {

void OpenURL(const std::string &url) {
#ifdef _WIN32
  ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
  std::string command = "open " + url;
  std::system(command.c_str());
#else
  std::string command = "xdg-open " + url;
  std::system(command.c_str());
#endif
}

} // namespace Platform
