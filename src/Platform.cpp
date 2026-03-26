#include "Platform.h"
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
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

std::string PickOpenFileName(const std::string &filter) {
#ifdef _WIN32
  OPENFILENAMEA ofn;
  char szFile[MAX_PATH] = {0};
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = sizeof(szFile);
  ofn.lpstrFilter = "Spoiler Log (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameA(&ofn))
    return szFile;
#elif __APPLE__
  std::string cmd = "osascript -e 'POSIX path of (choose file of type {\"txt\"} "
                    "with prompt \"Select Spoiler Log\")'";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    char buffer[1024];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL)
      result += buffer;
    pclose(pipe);
    if (!result.empty() && result.back() == '\n')
      result.pop_back();
    return result;
  }
#else
  // Try zenity first
  std::string cmd = "zenity --file-selection --file-filter=\"*.txt\" 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  std::string result = "";
  if (pipe) {
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
      result = buffer;
    }
    int status = pclose(pipe);
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n') result.pop_back();
      return result;
    }
  }

  // Fallback to kdialog
  cmd = "kdialog --getopenfilename . \"*.txt\" 2>/dev/null";
  pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    char buffer[1024];
    result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
      result = buffer;
    }
    int status = pclose(pipe);
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n') result.pop_back();
      return result;
    }
  }
#endif
  return "";
}

} // namespace Platform
