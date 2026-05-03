#include "ProfileLock.h"
#include "Config.h"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

namespace {

std::filesystem::path LockPath(const std::string &profile_name) {
  return Config::GetProfilesRoot() / profile_name / ".lock";
}

int64_t CurrentPid() {
#ifdef _WIN32
  return static_cast<int64_t>(GetCurrentProcessId());
#else
  return static_cast<int64_t>(getpid());
#endif
}

} // namespace

std::string ProfileLock::CurrentBootFingerprint() {
#ifdef __linux__
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  if (f) {
    std::string id;
    std::getline(f, id);
    if (!id.empty())
      return "linux:" + id;
  }
  return "linux:unknown";
#elif defined(__APPLE__)
  struct timeval tv;
  size_t len = sizeof(tv);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &tv, &len, nullptr, 0) == 0) {
    std::ostringstream os;
    os << "macos:" << tv.tv_sec << "." << tv.tv_usec;
    return os.str();
  }
  return "macos:unknown";
#elif defined(_WIN32)
  // Approximate boot time = now - GetTickCount64 (uptime in ms).
  // GetTickCount64 has ~16ms resolution while GetSystemTimeAsFileTime is
  // 100ns, so the subtraction drifts across calls within one boot.
  // Quantize to seconds — drift is well under 1s, so two calls at
  // different points during the same boot produce the same fingerprint.
  uint64_t uptime_ms = GetTickCount64();
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t now_100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  uint64_t boot_100ns = now_100ns - (uptime_ms * 10000ULL);
  uint64_t boot_sec = boot_100ns / 10000000ULL;
  std::ostringstream os;
  os << "win:" << boot_sec;
  return os.str();
#else
  return "unknown";
#endif
}

bool ProfileLock::IsHeldByLiveProcess(const std::string &profile_name) {
  auto path = LockPath(profile_name);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return false;
  std::ifstream in(path);
  if (!in)
    return false;
  std::string pid_line, fp_line;
  std::getline(in, pid_line);
  std::getline(in, fp_line);
  int64_t pid = 0;
  try {
    pid = std::stoll(pid_line);
  } catch (...) {
    return false;
  }
  if (fp_line != CurrentBootFingerprint())
    return false; // different boot — definitely stale
  return IsProcessAlive(pid);
}

bool ProfileLock::IsProcessAlive(int64_t pid) {
  if (pid <= 0)
    return false;
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
  if (!h)
    return false;
  DWORD exit_code = 0;
  bool alive = false;
  if (GetExitCodeProcess(h, &exit_code))
    alive = (exit_code == STILL_ACTIVE);
  CloseHandle(h);
  return alive;
#else
  // kill(pid, 0): 0 = exists; -1 + EPERM = exists but not ours; -1 + ESRCH = no
  if (kill(static_cast<pid_t>(pid), 0) == 0)
    return true;
  return errno == EPERM;
#endif
}

ProfileLock::ProfileLock(const std::string &profile_name)
    : profile_name_(profile_name) {}

ProfileLock::~ProfileLock() { Release(); }

bool ProfileLock::TryAcquire() {
  if (held_)
    return true;
  auto path = LockPath(profile_name_);
  if (std::filesystem::exists(path)) {
    // Read existing lock contents.
    std::ifstream in(path);
    std::string pid_line, fp_line;
    std::getline(in, pid_line);
    std::getline(in, fp_line);
    in.close();

    int64_t other_pid = 0;
    try {
      other_pid = std::stoll(pid_line);
    } catch (...) {
      other_pid = 0;
    }
    owner_pid_ = other_pid;
    owner_boot_fp_ = fp_line;

    bool boot_match = (fp_line == CurrentBootFingerprint());
    bool alive = boot_match && IsProcessAlive(other_pid);

    owner_stale_ = !alive;
    if (alive)
      return false;
    // Stale: fall through and overwrite.
  } else {
    owner_pid_ = 0;
    owner_boot_fp_.clear();
    owner_stale_ = false;
  }

  WriteSelf();
  held_ = true;
  return true;
}

void ProfileLock::ForceAcquire() {
  if (held_)
    return;
  WriteSelf();
  held_ = true;
}

void ProfileLock::Release() {
  if (!held_)
    return;
  std::error_code ec;
  std::filesystem::remove(LockPath(profile_name_), ec);
  held_ = false;
}

void ProfileLock::WriteSelf() {
  // Make sure profile dir exists.
  std::filesystem::create_directories(Config::GetProfilesRoot() /
                                      profile_name_);
  auto path = LockPath(profile_name_);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    std::cerr << "ProfileLock: could not write " << path << std::endl;
    return;
  }
  out << CurrentPid() << "\n" << CurrentBootFingerprint() << "\n";
}
