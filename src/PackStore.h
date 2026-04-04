#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class PackStore {
public:
  static fs::path GetCacheDir();
  static bool HasPack(const std::string &game);
  static fs::path GetPackPath(const std::string &game);
  static bool ImportPack(const std::string &game, const fs::path &sourcePath);
  static std::string SanitizeFileName(const std::string &name);

  struct PackInfo {
    std::string dir_name;
    std::string display_name;
  };

  static std::vector<PackInfo> ListPacks();
  static bool RemovePack(const std::string &game);
};
