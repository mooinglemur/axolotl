#pragma once
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

class DataPackageCache {
public:
  static void SaveGameData(const std::string &game_name,
                           const std::string &checksum,
                           const nlohmann::json &data);
  static nlohmann::json LoadGameData(const std::string &game_name,
                                     const std::string &checksum);

private:
  static std::string SanitizeFileName(const std::string &name);
  static std::mutex cache_mutex_;
};
