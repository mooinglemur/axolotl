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
#include <unordered_map>
#include <mutex>
#include <memory>

struct TrackerObject {
    std::string code;
    std::function<void(std::string)> on_change;

    bool active = false;
    int stage = 0;
    int count = 0;
    int increment = 1;
    int chestCount = 0;
    int availableChestCount = 0;
    int accessibilityLevel = 0;

    int get_count() const { return count; }
    void set_count(int c) { 
        if (count == c) return;
        count = c; stage = c; if(on_change) on_change(code); 
    }
    int get_stage() const { return stage; }
    void set_stage(int s) { 
        if (stage == s) return;
        stage = s; if(on_change) on_change(code); 
    }
    bool get_active() const { return active; }
    void set_active(bool a) { 
        if (active == a) return;
        active = a; if(on_change) on_change(code); 
    }
};

struct LocationLogic {
  std::string name;
  int64_t id;
  std::string logicalId; // Unique ID for pooling (e.g. __id_3626171 or @Name)
  std::string rule;
  std::string transpiledRule;
  int ruleIndex = -1; // Index into uniqueRules_
  int accessibility; // 0=None, 1=Partial, 2=Full
};

struct ItemDefault {
  bool active = false;
  int stage = 0;
  int count = 0;
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
  mutable std::recursive_mutex state_mutex_;
  std::filesystem::path currentPackPath_;
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
  struct ItemSnapshot {
    int64_t id;
    int count;
    std::string name;
    int player;
  };
  std::vector<ItemSnapshot> itemHistory_;
  std::vector<sol::object> compiledRules_;
  nlohmann::json lastSlotData_;
  std::map<std::string, std::shared_ptr<TrackerObject>> trackerObjects_;
  std::map<std::string, ItemDefault> itemDefaults_;
  std::map<std::string, sol::function> clearHandlers_;
  std::map<std::string, sol::function> itemHandlers_;
  std::map<std::string, sol::function> locationHandlers_;
  std::map<int64_t, int> lastItemCounts_;
  std::set<int64_t> lastCheckedLocationIds_;
  std::set<int64_t> lastMissingLocationIds_;
  int lastPlayerNumber_ = -1;
  int nextItemHandlerIndex_ = 1; // Global item handler index; persisted across calls to match pack CUR_INDEX
  std::map<std::string, std::map<std::string, sol::function>> watches_;
  bool firstRun_ = true;

  void BindGlobals();
  void LoadLocationsFromPack(const std::filesystem::path &packPath);
  void ProcessLocationNode(const nlohmann::json &node, const std::string &parentName,
                           const std::string &parentRule,
                           std::unordered_map<std::string, int> &ruleToIdx);
  void LoadItemsFromPack(const std::filesystem::path &dir);
  void ProcessItemJson(const nlohmann::json &j);
  std::shared_ptr<TrackerObject> GetTrackerObject(const std::string &code);
  std::string TranspileRule(const std::string &rule);
};
