#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

class PackStore {
public:
  static fs::path GetCacheDir();
  static bool HasPack(const std::string &game);
  static fs::path GetPackPath(const std::string &game);
  static bool ImportPack(const std::string &game, const fs::path &sourcePath);
  static std::string SanitizeFileName(const std::string &name);
};
