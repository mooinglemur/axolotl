#pragma once
#include <filesystem>
#include <cstdint>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>

struct LocationLogic {
  std::string name;
  int64_t id;
  std::string rule;
  std::string transpiledRule;
  int ruleIndex = -1; // Index into uniqueRules_
  int accessibility; // 0=None, 1=Partial, 2=Full
};

class LogicManager {
public:
  LogicManager();
  ~LogicManager();

  bool LoadPack(const std::string &game);
  void UpdateLogic(const std::map<int64_t, int> &itemCounts,
                   const nlohmann::json &slotData,
                   const std::set<int64_t> &checkedLocationIds,
                   const std::set<int64_t> &missingLocationIds,
                   int playerNumber);
  void Reset();

  const std::vector<LocationLogic> &GetLocations() const;
  void SetDebugMode(bool debug);
  bool GetDebugMode() const { return debug_mode_; }
  int GetAccessibility(int64_t locationId) const;
  const std::string &GetCurrentGame() const;

private:
  bool debug_mode_ = false;
  sol::state lua_;
  std::string currentGame_;
  std::vector<LocationLogic> locations_;
  std::vector<LocationLogic> allLocations_;
  std::map<int64_t, int> accessibilityCache_;
  std::unordered_set<std::string> reportedFailedRules_;
  std::map<std::string, int> lastItemNameCounts_;
  std::map<std::string, std::string> nameToCode_;
  std::unordered_map<std::string, std::string> ruleCache_;
  std::vector<std::string> uniqueRules_;
  nlohmann::json lastSlotData_;
  std::map<std::string, sol::table> trackerObjects_;
  std::vector<sol::function> clearHandlers_;
  std::map<int64_t, int> lastItemCounts_;
  std::set<int64_t> lastCheckedLocationIds_;
  std::set<int64_t> lastMissingLocationIds_;
  int lastPlayerNumber_ = -1;
  bool firstRun_ = true;

  void BindGlobals();
  void LoadLocationsFromPack(const std::filesystem::path &packPath);
  void LoadItemsFromPack(const std::filesystem::path &packPath);
  std::string TranspileRule(const std::string &rule);

  mutable std::mutex state_mutex_;
};
