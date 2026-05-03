#include "Platform.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
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

#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty())
    return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
  std::wstring out(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
  return out;
}

// Quote argv element per the Windows CommandLineToArgvW rules.
static std::wstring QuoteArg(const std::wstring &arg) {
  if (!arg.empty() &&
      arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    } else if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(arg[i]);
    } else {
      out.append(backslashes, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}
#endif

void ReExec(const std::vector<std::string> &args) {
#ifdef _WIN32
  wchar_t exe_w[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
  if (n == 0 || n == MAX_PATH) {
    std::cerr << "ReExec: GetModuleFileNameW failed" << std::endl;
    return;
  }
  std::wstring cmdline = QuoteArg(exe_w);
  for (const auto &a : args) {
    cmdline.push_back(L' ');
    cmdline.append(QuoteArg(Utf8ToWide(a)));
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  // CreateProcessW may modify the cmdline buffer.
  std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
  mutable_cmdline.push_back(L'\0');
  if (!CreateProcessW(exe_w, mutable_cmdline.data(), nullptr, nullptr, FALSE,
                      0, nullptr, nullptr, &si, &pi)) {
    std::cerr << "ReExec: CreateProcessW failed (err " << GetLastError()
              << ")" << std::endl;
    return;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  ExitProcess(0);
#else
  // Resolve self path.
  std::string exe_path;
#ifdef __APPLE__
  uint32_t buf_size = 0;
  _NSGetExecutablePath(nullptr, &buf_size);
  std::vector<char> buf(buf_size);
  if (_NSGetExecutablePath(buf.data(), &buf_size) != 0) {
    std::cerr << "ReExec: _NSGetExecutablePath failed" << std::endl;
    return;
  }
  exe_path = std::string(buf.data());
#else
  char buf[4096];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    std::cerr << "ReExec: readlink /proc/self/exe failed" << std::endl;
    return;
  }
  buf[len] = '\0';
  exe_path = std::string(buf);
#endif

  std::vector<std::string> owned;
  owned.reserve(args.size() + 1);
  owned.push_back(exe_path);
  for (const auto &a : args)
    owned.push_back(a);

  std::vector<char *> argv;
  argv.reserve(owned.size() + 1);
  for (auto &s : owned)
    argv.push_back(s.data());
  argv.push_back(nullptr);

  execv(exe_path.c_str(), argv.data());
  // Only reached on failure.
  std::cerr << "ReExec: execv failed" << std::endl;
#endif
}

} // namespace Platform
