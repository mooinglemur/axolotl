#pragma once
#include <cstdint>
#include <string>

// RAII lock on a profile directory. Lockfile path is
// <profiles>/<name>/.lock and contains "<pid>\n<boot_fingerprint>\n".
//
// Two phases: TryAcquire() attempts to lock; if it returns false, IsStale()
// and the GetLockOwner* getters describe the existing owner so the caller
// (e.g. ProfilePicker) can decide whether to take over. ForceAcquire()
// overwrites the lockfile unconditionally.
class ProfileLock {
public:
  explicit ProfileLock(const std::string &profile_name);
  ~ProfileLock();

  ProfileLock(const ProfileLock &) = delete;
  ProfileLock &operator=(const ProfileLock &) = delete;

  // Returns true on success; on failure populates the GetLockOwner* fields.
  bool TryAcquire();
  // Steals the lock regardless of current owner.
  void ForceAcquire();
  // Releases lock if held; safe to call repeatedly.
  void Release();

  bool IsHeld() const { return held_; }

  // Valid only after a failed TryAcquire().
  int64_t GetLockOwnerPid() const { return owner_pid_; }
  const std::string &GetLockOwnerBootFingerprint() const {
    return owner_boot_fp_;
  }
  // True if the held lockfile was clearly written by a process that no
  // longer exists, or by a previous boot of this machine.
  bool IsStale() const { return owner_stale_; }

  // Build the canonical fingerprint identifying the current OS boot.
  static std::string CurrentBootFingerprint();
  // Returns true if a process with the given pid is currently alive.
  static bool IsProcessAlive(int64_t pid);

  // Read-only probe: returns true iff the named profile has a .lock file
  // owned by an alive process from the current boot. Does not create,
  // modify, or delete any files. Safe to call at high frequency.
  static bool IsHeldByLiveProcess(const std::string &profile_name);

private:
  std::string profile_name_;
  bool held_ = false;
  int64_t owner_pid_ = 0;
  std::string owner_boot_fp_;
  bool owner_stale_ = false;

  void WriteSelf();
};
