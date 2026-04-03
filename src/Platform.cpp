#include "Platform.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#ifdef __APPLE__
#include <vector>
#endif

#ifdef _WIN32
#include <commdlg.h>
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

std::string PickOpenFileName(const std::string &filter) {
#ifdef _WIN32
  OPENFILENAMEA ofn;
  char szFile[MAX_PATH] = {0};
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = sizeof(szFile);
  ofn.lpstrFilter = filter.c_str();
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameA(&ofn))
    return szFile;
#elif __APPLE__
  // Extract extensions for Apple Script
  // filter format: Description\0*.ext\0Description\0*.ext\0\0
  std::vector<std::string> extensions;
  const char *ptr = filter.c_str();
  while (*ptr) {
    std::string desc = ptr;
    ptr += desc.length() + 1;
    if (!*ptr)
      break;
    std::string ext = ptr;
    if (ext.find("*.") == 0) {
      extensions.push_back(ext.substr(2));
    }
    ptr += ext.length() + 1;
  }

  std::string choice_types = "";
  for (size_t i = 0; i < extensions.size(); ++i) {
    choice_types += "\"" + extensions[i] + "\"";
    if (i < extensions.size() - 1)
      choice_types += ",";
  }

  std::string cmd = "osascript -e 'POSIX path of (choose file of type {" +
                    choice_types +
                    "} "
                    "with prompt \"Select File\")'";
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
  // Extract first found extension for Zenity/KDialog fallback
  std::string first_ext = "*.txt";
  const char *ptr = filter.c_str();
  while (*ptr) {
    std::string desc = ptr;
    ptr += desc.length() + 1;
    if (!*ptr)
      break;
    std::string ext = ptr;
    if (ext.find("*.") == 0) {
      first_ext = ext;
      break;
    }
    ptr += ext.length() + 1;
  }

  // Try zenity first
  std::string cmd =
      "zenity --file-selection --file-filter=\"" + first_ext + "\" 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  std::string result = "";
  if (pipe) {
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
      result = buffer;
    }
    int status = pclose(pipe);
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n')
        result.pop_back();
      return result;
    }
  }

  // Fallback to kdialog
  cmd = "kdialog --getopenfilename . \"" + first_ext + "\" 2>/dev/null";
  pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    char buffer[1024];
    result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
      result = buffer;
    }
    int status = pclose(pipe);
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n')
        result.pop_back();
      return result;
    }
  }
#endif
  return "";
}

} // namespace Platform
