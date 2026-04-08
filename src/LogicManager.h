#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sol/sol.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TrackerObject {
  std::string code;
  std::string type; // "toggle", "progressive", "consumable", etc.
  std::function<void(std::string)> on_change;
  sol::table extra_props_; // arbitrary Lua fields set by pack scripts

  bool active = false;
  int stage = 0;
  int count = 0;
  int increment = 1;
  int chestCount = 0;
  int availableChestCount = 0;
  int accessibilityLevel = 0;

  int get_count() const { return count; }
  void set_count(int c) {
    if (count == c)
      return;
    count = c;
    stage = c;
    if (on_change)
      on_change(code);
  }
  int get_stage() const { return stage; }
  void set_stage(int s) {
    if (stage == s)
      return;
    stage = s;
    if (on_change)
      on_change(code);
  }
  bool get_active() const { return active; }
  void set_active(bool a) {
    if (active == a)
      return;
    active = a;
    if (on_change)
      on_change(code);
  }
};

struct LocationLogic {
  std::string name;              // full breadcrumb string (for logging/debug)
  std::vector<std::string> path; // individual path segments, e.g. ["Tower of
                                 // Wing Cap Entrance", "Rainbow Ride", "Star"]
  int64_t id;
  std::string logicalId; // Unique ID for pooling (e.g. __id_3626171 or @Name)
  std::string rule;
  std::string transpiledRule;
  std::string refLeaf; // Second-to-last segment of a PopTracker "ref" path,
                       // used as fallback for ID resolution
  int ruleIndex = -1;  // Index into uniqueRules_
  int accessibility;   // 0=None, 1=Partial, 2=Full
};

struct ItemDefault {
  bool active = false;
  int stage = 0;
  int count = 0;
};

class LogicManager {
public:
  enum class LoadState { Uninitialized, Loading, Ready, Error };

  LogicManager();
  ~LogicManager();

  bool LoadPack(const std::string &game,
                const nlohmann::json &slotData = nlohmann::json{});
  bool LoadPackAsync(const std::string &game,
                     const nlohmann::json &slotData = nlohmann::json{});
  void UpdateLogic(const std::map<int64_t, int> &itemCounts,
                   const nlohmann::json &slotData,
                   const std::set<int64_t> &checkedLocationIds,
                   const std::set<int64_t> &missingLocationIds,
                   int playerNumber);
  void Reset();

  bool IsReady() const { return load_state_.load() == LoadState::Ready; }
  bool IsLoading() const { return load_state_.load() == LoadState::Loading; }
  bool HasError() const { return load_state_.load() == LoadState::Error; }
  const std::string &GetPendingGame() const { return pending_game_; }
  const std::string &GetPendingVariant() const { return pending_variant_; }
  bool WasLoadedWithSlotData() const { return loaded_with_slot_data_; }
  // Resolve location IDs from the Archipelago data package by name matching.
  // Should be called once after the pack is ready and the data package
  // received.
  void ResolveLocationIds(const std::map<std::string, int64_t> &nameToId);

  const std::vector<LocationLogic> &GetLocations() const;
  void SetDebugMode(bool debug);
  bool GetDebugMode() const { return debug_mode_; }
  int GetAccessibility(int64_t locationId) const;
  const std::string &GetCurrentGame() const;

private:
  bool debug_mode_ = false;
  int instance_id_ = 0;
  std::atomic<LoadState> load_state_{LoadState::Uninitialized};
  std::thread load_thread_;
  std::string pending_game_;
  std::string pending_variant_;
  bool loaded_with_slot_data_ = false;
  bool ids_resolved_ = false;
  mutable std::recursive_mutex state_mutex_;
  std::filesystem::path currentPackPath_;
  sol::state lua_;
  std::string currentGame_;
  std::vector<LocationLogic> locations_;
  std::vector<LocationLogic> allLocations_;
  std::map<int64_t, int64_t>
      idAliases_; // secondary AP ID → primary AP ID (for multi-item sections)
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
  int nextItemHandlerIndex_ = 1; // Global item handler index; persisted across
                                 // calls to match pack CUR_INDEX
  std::map<std::string, std::map<std::string, sol::function>> watches_;
  bool firstRun_ = true;
  bool accessibility_stale_ = false; // set when any TrackerObject state changes
  std::set<int64_t>
      current_checked_ids_; // current checkedLocationIds for lazy convergence
  // Progressive item stage code tracking.
  // Maps any code → list of (primaryCode, stageIdx, inheritCodes) entries so
  // ProviderCountForCode("progression_ticket") can consult the primary
  // TrackerObject's current stage to decide the count.
  struct StageCodeLink {
    std::string primaryCode;
    int stageIdx;
    bool inherit;
  };
  std::map<std::string, std::vector<StageCodeLink>> stageCodeLinks_;
  // LuaItems created via ScriptHost:CreateLuaItem(). Each is a Lua table with
  // optional ProvidesCodeFunc callback. ProviderCountForCode consults these
  // after built-in item lookup.
  std::vector<sol::table> luaItems_;

  void BindGlobals();
  void RunConvergenceOnce(); // lazy single-pass accessibility update
  void LoadLocationsFromPack(const std::filesystem::path &packPath);
  void ProcessLocationNode(const nlohmann::json &node,
                           const std::vector<std::string> &parentPath,
                           const std::string &parentRule,
                           std::unordered_map<std::string, int> &ruleToIdx);
  void LoadItemsFromPack(const std::filesystem::path &dir);
  void ProcessItemJson(const nlohmann::json &j);
  std::shared_ptr<TrackerObject> GetTrackerObject(const std::string &code);
  std::string TranspileRule(const std::string &rule);
};
