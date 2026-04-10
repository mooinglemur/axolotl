#include "Platform.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#ifndef __linux__
#include <vector>
#endif

#ifdef _WIN32
#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Platform {

void OpenURL(const std::string &url) {
#ifdef _WIN32
  ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
  pid_t pid = fork();
  if (pid == 0) {
    execl("/usr/bin/open", "open", url.c_str(), (char *)nullptr);
    _exit(1);
  }
#else
  pid_t pid = fork();
  if (pid == 0) {
    execl("/usr/bin/xdg-open", "xdg-open", url.c_str(), (char *)nullptr);
    _exit(1);
  }
#endif
}

std::string PickOpenFileName(const std::string &filter) {
#ifdef _WIN32
  // Convert filter (which contains embedded NUL separators) to wide chars
  std::vector<wchar_t> wfilter(filter.size() + 2, L'\0');
  for (size_t i = 0; i < filter.size(); ++i)
    wfilter[i] = (wchar_t)(unsigned char)filter[i];

  OPENFILENAMEW ofn;
  wchar_t szFile[MAX_PATH] = {0};
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = wfilter.data();
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) {
    int len = WideCharToMultiByte(CP_UTF8, 0, szFile, -1, nullptr, 0, nullptr,
                                  nullptr);
    if (len > 1) {
      std::string result(len - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, szFile, -1, result.data(), len, nullptr,
                          nullptr);
      return result;
    }
  }
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
  std::string first_ext = "*.*";
  const char *ptr = filter.data();
  const char *end = filter.data() + filter.size();
  while (ptr < end && *ptr) {
    std::string desc = ptr;
    ptr += desc.length() + 1;
    if (ptr >= end || !*ptr)
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
#ifndef _WIN32
    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0 && !result.empty()) { // Success
      if (result.back() == '\n')
        result.pop_back();
      return result;
    } else if (exit_code == 1) { // User Cancel
      return "";
    }
    // Fallback on 127 or other errors
#else
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n')
        result.pop_back();
      return result;
    }
#endif
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
#ifndef _WIN32
    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0 && !result.empty()) {
      if (result.back() == '\n')
        result.pop_back();
      return result;
    }
#else
    if (status == 0 && !result.empty()) {
      if (result.back() == '\n')
        result.pop_back();
      return result;
    }
#endif
  }
#endif
  return "";
}

} // namespace Platform
