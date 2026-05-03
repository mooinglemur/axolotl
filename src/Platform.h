#pragma once
#include <string>
#include <vector>

namespace Platform {
void OpenURL(const std::string &url);
std::string PickOpenFileName(const std::string &filter = "");

// Replace the current process with a fresh invocation of self, passing
// `args` as the new argv (after argv[0], which is filled in automatically).
// On POSIX this uses execv() and never returns on success. On Windows it
// spawns a new process and exits the current one.
// On any failure the function returns to the caller.
void ReExec(const std::vector<std::string> &args);
} // namespace Platform
