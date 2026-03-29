#include "DataPackageCache.h"
#include "Config.h"
#include <filesystem>
#include <fstream>
#include <random>

std::mutex DataPackageCache::cache_mutex_;

void DataPackageCache::SaveGameData(const std::string &game_name,
                                    const std::string &checksum,
                                    const nlohmann::json &data) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto dir = Config::GetDataPackageCacheDir();
  auto filename = SanitizeFileName(game_name) + "_" + checksum + ".json";
  auto path = dir / filename;

  // Atomic write using a temporary file
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::string tmp_name = filename + ".tmp." + std::to_string(dist(gen));
  auto tmp_path = dir / tmp_name;

  std::ofstream fout(tmp_path);
  if (fout.is_open()) {
    fout << data.dump();
    fout.close();

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
      // Cleanup on failure
      std::filesystem::remove(tmp_path, ec);
    }
  }
}

nlohmann::json DataPackageCache::LoadGameData(const std::string &game_name,
                                              const std::string &checksum) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto dir = Config::GetDataPackageCacheDir();
  auto filename = SanitizeFileName(game_name) + "_" + checksum + ".json";
  auto path = dir / filename;

  if (std::filesystem::exists(path)) {
    std::ifstream fin(path);
    if (fin.is_open()) {
      try {
        nlohmann::json data;
        fin >> data;
        return data;
      } catch (...) {
      }
    }
  }
  return nlohmann::json();
}

std::string DataPackageCache::SanitizeFileName(const std::string &name) {
  std::string sanitized = name;
  for (char &c : sanitized) {
    if (!isalnum(c) && c != '-' && c != '_') {
      c = '_';
    }
  }
  return sanitized;
}
