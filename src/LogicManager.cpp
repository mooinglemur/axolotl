#include "LogicManager.h"
#include "PackStore.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

static sol::object JsonToLua(sol::state_view &lua, const nlohmann::json &j) {
  if (j.is_null())
    return sol::lua_nil;
  if (j.is_boolean())
    return sol::make_object(lua, j.get<bool>());
  if (j.is_number()) {
    if (j.is_number_integer()) {
      return sol::make_object(lua, j.get<int64_t>());
    } else {
      double d = j.get<double>();
      if (d == std::floor(d)) {
        return sol::make_object(lua, static_cast<int64_t>(d));
      }
      return sol::make_object(lua, d);
    }
  }
  if (j.is_string())
    return sol::make_object(lua, j.get<std::string>());
  if (j.is_array()) {
    sol::table t = lua.create_table();
    int i = 1;
    for (const auto &el : j) {
      t[i++] = JsonToLua(lua, el);
    }
    return t;
  }
  if (j.is_object()) {
    sol::table t = lua.create_table();
    for (auto it = j.begin(); it != j.end(); ++it) {
      t[it.key()] = JsonToLua(lua, it.value());
    }
    return t;
  }
  return sol::lua_nil;
}

LogicManager::LogicManager() {
  lua_.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table,
                      sol::lib::string, sol::lib::math, sol::lib::bit32);
  BindGlobals();
}

LogicManager::~LogicManager() {}

void LogicManager::Reset() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  currentGame_ = "";
  locations_.clear();
  allLocations_.clear();
  accessibilityCache_.clear();
  ruleCache_.clear();
  uniqueRules_.clear();
  reportedFailedRules_.clear();
  nameToCode_.clear();
  lastItemNameCounts_.clear();
  lastItemCounts_.clear();
  lastCheckedLocationIds_.clear();
  lastMissingLocationIds_.clear();
  lastPlayerNumber_ = -1;
  firstRun_ = true;
  nextItemHandlerIndex_ = 1;
  itemHistory_.clear();
  itemHandlers_.clear();
  locationHandlers_.clear();
  clearHandlers_.clear();

  // Reset Lua state
  lua_ = sol::state();
  lua_.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table,
                      sol::lib::string, sol::lib::math, sol::lib::bit32);
  BindGlobals();
}

void LogicManager::SetDebugMode(bool debug) {
  debug_mode_ = debug;
  lua_["AUTOTRACKER_ENABLE_DEBUG_LOGGING_AP"] = debug;
}

bool LogicManager::LoadPack(const std::string &game) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (debug_mode_)
    std::cerr << "LogicManager: Starting load for game: " << game << std::endl;
  fs::path packPath = PackStore::GetPackPath(game);
  if (!fs::exists(packPath / "manifest.json"))
    return false;

  currentGame_ = game;
  currentPackPath_ = packPath;
  try {
    std::ifstream f(packPath / "manifest.json");
    json manifest = json::parse(f, nullptr, true, true);
    std::string entry = manifest.value("entry", "scripts/init.lua");

    std::string path = lua_["package"]["path"];
    path += ";" + (packPath / "scripts" / "?.lua").string();
    path += ";" + (packPath / "?.lua").string();
    lua_["package"]["path"] = path;

    lua_["GAME_NAME"] = game;
    lua_["CURRENT_GAME"] = game;

    itemDefaults_.clear();
    clearHandlers_.clear();
    itemHandlers_.clear();
    locationHandlers_.clear();
    trackerObjects_.clear();
    
    // Clear Lua-side handlers too if possible, but C++ side is enough
    // as we call them from the C++ vectors.

    LoadLocationsFromPack(packPath);

    if (debug_mode_)
      std::cout << "LogicManager: Loaded " << allLocations_.size()
              << " nodes and " << uniqueRules_.size() << " unique rules."
              << std::endl;

    // Marker for first logic pass
    firstRun_ = true;
    lastItemNameCounts_.clear();
    lastItemCounts_.clear();
    lastCheckedLocationIds_.clear();
    lastMissingLocationIds_.clear();
    lastPlayerNumber_ = -1;

    lua_.script_file((packPath / entry).string());
    return true;
  } catch (const std::exception &e) {
    if (debug_mode_)
      std::cerr << "LogicManager Load Error: " << e.what() << std::endl;
    return false;
  }
}

void LogicManager::LoadItemsFromPack(const fs::path &dir) {
  if (!fs::exists(dir))
    return;
  for (const auto &entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      try {
        std::ifstream f(entry.path());
        json j = json::parse(f, nullptr, true, true);
        if (j.is_array()) {
          for (const auto &item : j) {
            ProcessItemJson(item);
          }
        } else if (j.is_object()) {
          ProcessItemJson(j);
        }
      } catch (const std::exception &e) {
        if (debug_mode_)
          std::cerr << "LogicManager: Error parsing item JSON " << entry.path()
                    << ": " << e.what() << std::endl;
      }
    }
  }
}

void LogicManager::ProcessItemJson(const nlohmann::json &j) {
  std::vector<std::string> codes;
  if (j.contains("code")) {
    codes.push_back(j["code"].get<std::string>());
  }
  if (j.contains("codes")) {
    if (j["codes"].is_array()) {
      for (const auto &c : j["codes"])
        codes.push_back(c.get<std::string>());
    } else if (j["codes"].is_string()) {
      codes.push_back(j["codes"].get<std::string>());
    }
  }

  if (codes.empty())
    return;

  ItemDefault def;
  if (j.contains("initial_active_state")) {
    def.active = j["initial_active_state"].get<bool>();
  } else if (j.contains("active")) {
    def.active = j["active"].get<bool>();
  }

  if (j.contains("initial_stage_idx")) {
    def.stage = j["initial_stage_idx"].get<int>();
  } else if (j.contains("stage")) {
    def.stage = j["stage"].get<int>();
  }

  if (j.contains("count")) {
    def.count = j["count"].get<int>();
  }

  for (const auto &code : codes) {
    itemDefaults_[code] = def;
    auto obj = GetTrackerObject(code);
    if (obj) {
      obj->active = def.active;
      obj->stage = def.stage;
      obj->count = def.count;
    }
  }
}

void LogicManager::UpdateLogic(const std::map<int64_t, int> &itemCounts,
                               const nlohmann::json &slotData,
                               const std::set<int64_t> &checkedLocationIds,
                               const std::set<int64_t> &missingLocationIds,
                               int playerNumber) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);

  if (!firstRun_ && slotData == lastSlotData_ &&
      itemCounts == lastItemCounts_ &&
      checkedLocationIds == lastCheckedLocationIds_ &&
      missingLocationIds == lastMissingLocationIds_ &&
      playerNumber == lastPlayerNumber_) {
    return;
  }

  // Monkey-patch debug hooks if debug mode is active
  if (debug_mode_ && firstRun_) {
    lua_.safe_script(R"LUA(
        local old_areaReveal = _G.areaReveal
        _G.areaReveal = function()
            print("LogicManager [LUA DEBUG]: areaReveal() triggered")
            if not SLOT_DATA then 
                print("LogicManager [LUA DEBUG]: areaReveal ABORTED - SLOT_DATA is nil")
                return 
            end
            
            -- Diagnostic check for specific problematic entrances
            local bitdw_code = "@Bowser in the Dark World Entrance"
            local obj = Tracker:FindObjectForCode(bitdw_code)
            if obj then
                print(string.format("LogicManager [LUA DEBUG]: Diagnostic BitDW: Access=%d, DestStage=%d", 
                    obj.AccessibilityLevel, Tracker:FindObjectForCode("__er_BitDW_dst").CurrentStage))
            else
                print("LogicManager [LUA DEBUG]: Diagnostic ERROR: Could not find BitDW Entrance object!")
            end

            if old_areaReveal then old_areaReveal() end
        end

        local old_SetStage = _G.SetStage
        _G.SetStage = function(entrance, stage)
            print("LogicManager [LUA DEBUG]: SetStage(" .. tostring(entrance) .. " -> " .. tostring(stage) .. ")")
            if old_SetStage then old_SetStage(entrance, stage) end
        end
        
        print("LogicManager [LUA DEBUG]: Diagnostic Hooks Applied")
    )LUA");
  }

  bool isNewSession = firstRun_ || lastSlotData_ != slotData;
  // State updates moved to end of function to support incremental sync

  // Sync SLOT_DATA to Lua
  auto luaSlotData = JsonToLua(lua_, slotData);
  lua_["SLOT_DATA"] = luaSlotData;

  // Sync Archipelago global state
  sol::table archipelago = lua_["Archipelago"];
  archipelago["SlotData"] = luaSlotData;
  archipelago["PlayerNumber"] = playerNumber;

  // Define a robust Lua execution helper
  auto executeLuaHandler = [&](const std::string& name, sol::function func, auto... args) {
    if (!func.valid()) return;
    
    // Ensure all logic results are pushed to Lua objects before running handlers
    // (This allows obj.AccessibilityLevel to be correct inside areaReveal)
    for (auto const& [lid, obj] : trackerObjects_) {
        // No-op to trigger property getter refresh if needed (sol2 usually handles this)
    }

    auto res = func(args...);
    if (!res.valid()) {
      sol::error err = res;
      std::cerr << "LogicManager [LUA ERROR]: Error in " << name << ": " << err.what() << std::endl;
    }
  };

  // Silence Lua prints during sync to reduce noise
  sol::function oldPrint = lua_["print"];
  lua_["print"] = [](sol::variadic_args) {};

  if (isNewSession) {
    for (auto const &[code, def] : itemDefaults_) {
      auto obj = GetTrackerObject(code);
      if (obj) {
        obj->set_active(def.active);
        obj->set_stage(def.stage);
        obj->set_count(0); // Reset acquired count for new sync
        // Reset accessibility for all objects at start of session
        obj->accessibilityLevel = 0;
      }
    }

    // Explicitly mark checked locations as cleared (3) before replaying
    for (int64_t id : checkedLocationIds) {
      std::string lid = "__id_" + std::to_string(id);
      auto obj = GetTrackerObject(lid);
      if (obj) {
        obj->accessibilityLevel = 3;
      }
    }

    for (auto const &it : clearHandlers_) {
      executeLuaHandler("onClear", it.second, luaSlotData);
    }
  }

  // ITEM SYNC: Detect and replay any items received since last update.
  // The index passed to onItem must be a monotonically-increasing global counter
  // that persists across UpdateLogic() calls. Pack scripts (e.g. SM64) use a
  // CUR_INDEX guard to skip already-processed items, so resetting to 1 each call
  // would cause every incremental item to be silently dropped.
  {
    if (isNewSession)
      nextItemHandlerIndex_ = 1;
    int index = nextItemHandlerIndex_;
    for (auto const &ic : itemCounts) {
      int64_t id = ic.first;
      int count = ic.second;
      int previousCount = lastItemCounts_.count(id) ? lastItemCounts_.at(id) : 0;

      if (count > previousCount || isNewSession) {
          int itemsToReplay = isNewSession ? count : (count - previousCount);
          std::string itemName = "unnamed_item";
          for (const auto& l : allLocations_) {
              if (l.id == id) { itemName = l.name; break; }
          }

          for (int i = 0; i < itemsToReplay; ++i) {
             for (auto const &it : itemHandlers_) {
               executeLuaHandler("onItem", it.second, index++, id, itemName, playerNumber);
             }
          }
      }
    }
    nextItemHandlerIndex_ = index;
  }

  if (isNewSession) {
    // LOCATION REPLAY: Mark checked locations as cleared in Lua
    int locCount = 0;
    for (int64_t id : checkedLocationIds) {
      for (auto const &it : locationHandlers_) {
        std::string locName = "checked_location";
        for (const auto& l : allLocations_) {
            if (l.id == id) { locName = l.name; break; }
        }
        
        if (it.second.valid()) {
          auto res = it.second(id, locName);
          if (res.valid()) {
            locCount++;
            if (debug_mode_) {
                if (locCount < 5) std::cout << "LogicManager [DEBUG]: Replayed location " << id << " -> Success" << std::endl;
            }
          } else {
            sol::error err = res;
            std::cerr
                << "LogicManager [LUA ERROR]: Location handler failed for ID "
                << id << ": " << err.what() << std::endl;
          }
        }
      }
    }
    if (debug_mode_)
      std::cout << "LogicManager [DEBUG]: Replayed " << locCount
                << " location checks." << std::endl;
  }

  sol::table checkedTable = lua_.create_table();
  int idx = 1;
  for (int64_t id : checkedLocationIds)
    checkedTable[idx++] = id;
  archipelago["CheckedLocations"] = checkedTable;

  sol::table missingTable = lua_.create_table();
  idx = 1;
  for (int64_t id : missingLocationIds)
    missingTable[idx++] = id;
  archipelago["MissingLocations"] = missingTable;

  // Always mark every server-checked location as cleared (accessibilityLevel=3).
  // The convergence loop skips allLocations_ entries whose id is in checkedLocationIds,
  // so it never populates currentPassMax for those logicalIds and therefore never resets
  // their TrackerObject. Without this, a freshly-checked location retains its previous
  // accessibilityLevel (e.g. 2) and incorrectly remains visible in the UI.
  for (int64_t id : checkedLocationIds) {
    std::string lid = "__id_" + std::to_string(id);
    auto obj = GetTrackerObject(lid);
    if (obj && obj->accessibilityLevel != 3)
      obj->accessibilityLevel = 3;
  }

  auto evaluateRules = [this]() {
    auto rulesTable = lua_.create_table();
    for (size_t i = 0; i < compiledRules_.size(); ++i) {
      rulesTable[i + 1] = compiledRules_[i];
    }
    sol::function eval = lua_["__AxoEvaluateRules"];
    if (eval.valid()) {
        auto res = eval(rulesTable);
        if (res.valid()) return res.get<sol::table>();
    }
    return lua_.create_table();
  };

  auto tracker = lua_["Tracker"];

  // Restore Lua prints after sync
  lua_["print"] = oldPrint;

  // Multi-pass Convergence Loop
  bool logicChanged = true;
  int iterations = 0;

  std::map<std::string, int> maxAccessByLogicalId;
  std::map<std::string, std::string> maxPathByLogicalId;
  std::map<std::string, std::vector<std::string>> maxPathSegsById;

  while (logicChanged && iterations < 10) {
    logicChanged = false;
    iterations++;

    sol::table results = evaluateRules();

    // Reset best-access map for this pass
    std::map<std::string, int> currentPassMax;
    std::map<std::string, std::string> currentPassName;
    std::map<std::string, std::vector<std::string>> currentPassPath;

    for (const auto &loc : allLocations_) {
      // SKIP CLEARED: If the location is already checked on the server,
      // it should not appear in the progression list.
      if (checkedLocationIds.count(loc.id))
        continue;

      int v = 0;
      if (loc.ruleIndex != -1) {
        sol::object res = results[static_cast<size_t>(loc.ruleIndex + 1)];
        if (res.is<int>())
          v = res.as<int>();
        else if (res.is<bool>())
          v = res.as<bool>() ? 6 : 0;
      } else {
        v = 6; // PopTracker standard: No rules means Full Access
      }

      // Coerce 0-6 range to Tracker accessibility levels
      int access = 0;
      if (v >= 6)
        access = 2; // Full
      else if (v > 0)
        access = 1; // Partial/Sequence Break

      if (access >= currentPassMax[loc.logicalId]) {
        // Name Prioritization & Locking:
        // 1. Prefer higher accessibility.
        // 2. If accessibility is equal, prefer a meaningful name over boilerplate.
        // 3. If both are equal, LOCK the first name found (use strict > for quality check).
        
        bool isCurrentBoilerplate = loc.name.find("Entrance Accessibility") != std::string::npos ||
                                     loc.name.find("Unknown Stage") != std::string::npos ||
                                     (loc.name.find(" > ") == std::string::npos && loc.id <= 0);
        
        bool hasPriorName = currentPassName.count(loc.logicalId);
        bool existingIsBoilerplate = hasPriorName && (currentPassName[loc.logicalId].find("Entrance Accessibility") != std::string::npos || 
                                                     currentPassName[loc.logicalId].find("Unknown Stage") != std::string::npos ||
                                                     (currentPassName[loc.logicalId].find(" > ") == std::string::npos && currentPassMax[loc.logicalId] <= 0));

        bool betterAccess = access > currentPassMax[loc.logicalId];
        bool betterQuality = !isCurrentBoilerplate && existingIsBoilerplate;

        if (!hasPriorName || betterAccess || betterQuality) {
            currentPassMax[loc.logicalId] = access;
            if (!loc.name.empty()) {
                currentPassName[loc.logicalId] = loc.name;
                currentPassPath[loc.logicalId] = loc.path;
            }
        }
      }
    }

    // Apply pass results to TrackerObjects and check for changes
    for (auto it = currentPassMax.begin(); it != currentPassMax.end(); ++it) {
      const std::string &lid = it->first;
      int access = it->second;
      auto obj = GetTrackerObject(lid);
      if (obj) {
        // ONLY update if not already cleared (status 3)
        if (obj->accessibilityLevel < 3) {
          if (obj->accessibilityLevel != access) {
            obj->accessibilityLevel = access;
            logicChanged = true;
          }
        }
      }
    }

    // NATURAL REVELATION: Trigger settings watches after the first logic pass
    // so that areaReveal() can see the calculated accessibility levels.
    if (isNewSession && iterations == 1) {
      auto spoilReqs = GetTrackerObject("__setting_spoil_reqs");
      if (spoilReqs) {
        spoilReqs->set_stage(1);
        spoilReqs->set_active(true);
      }
      auto autoEnt = GetTrackerObject("__setting_auto_ent");
      if (autoEnt) {
        autoEnt->set_stage(1);
        autoEnt->set_active(true);
      }
    }

    // Force logic change if this is the first pass and items/locations changed
    if (iterations == 1) {
        bool itemsChanged = itemCounts != lastItemCounts_;
        bool checksChanged = checkedLocationIds != lastCheckedLocationIds_;
        if (itemsChanged || checksChanged || isNewSession) {
            logicChanged = true;
            if (debug_mode_) std::cout << "LogicManager [DEBUG]: Detected change in items/locations/session - Forcing convergence" << std::endl;
        }
    }

    if (!logicChanged || iterations == 10) {
      maxAccessByLogicalId = currentPassMax;
      maxPathByLogicalId = currentPassName;
      maxPathSegsById = currentPassPath;
    }
  }

  // Restore Lua prints for UI phase
  lua_["print"] = oldPrint;

  // 3. Finalize UI list after convergence
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    locations_.clear();
    std::set<std::string> addedLids;
    for (const auto &loc : allLocations_) {
      // ONLY show collectible location IDs that are actually accessible AND NOT
      // CLEARED (3)
      auto obj = GetTrackerObject(loc.logicalId);
      int access = (obj ? obj->accessibilityLevel : 0);

      // Only add items/regions that are accessible and NOT checked.
      // Filter out empty names or internal nodes
      if (access > 0 && access < 3 && !loc.name.empty() &&
          addedLids.find(loc.logicalId) == addedLids.end()) {
        
        // FILTER: Skip internal boilerplate logic nodes to keep the UI clean.
        // We only want to show meaningful Regions (Level Entrances) and actual Locations.
        // Safer ID check to capture any node that isn't a server-tracked location.
        if (loc.id <= 0) {
            std::string n = loc.name;
            // Broad search for boilerplate keywords in the breadcrumb path.
            if (n.find("Entrance Accessibility") != std::string::npos ||
                n.find("Unknown Stage") != std::string::npos ||
                n.find("Enter Stage") != std::string::npos ||
                n.find("[Z]") != std::string::npos ||
                n.find("Green:") != std::string::npos ||
                n.find("Yellow:") != std::string::npos ||
                n.find("Blue:") != std::string::npos ||
                n.find("Red:") != std::string::npos) {
                continue; 
            }
        }

        LocationLogic entry = loc;
        std::string finalPath = maxPathByLogicalId[loc.logicalId];
        if (finalPath.empty()) finalPath = loc.name; // Fallback to raw name if path tracking missed it
        entry.name = finalPath;

        auto pathIt = maxPathSegsById.find(loc.logicalId);
        if (pathIt != maxPathSegsById.end() && !pathIt->second.empty())
          entry.path = pathIt->second;
        // else: entry.path retains loc.path set during load (already a valid fallback)

        entry.accessibility = access;
        locations_.push_back(entry);
        addedLids.insert(loc.logicalId);
      }
    }

    // Update accessibilityCache_ for quick lookups
    accessibilityCache_.clear();
    for (const auto &[cid, obj] : trackerObjects_) {
      if (cid.starts_with("__id_")) {
        try {
          int64_t id = std::stoll(cid.substr(5));
          accessibilityCache_[id] = obj->accessibilityLevel;
        } catch (...) {
        }
      }
    }

    if (debug_mode_) {
      std::cout << "LogicManager [UI CONTENT]:" << std::endl;
      // Filter out redundant path segments. 
      // If we have "A > B" and "A > B > C", we only want to show "A > B > C" in the terminal report.
      std::vector<LocationLogic> filtered;
      for (size_t i = 0; i < locations_.size(); ++i) {
          bool isSubpath = false;
          for (size_t j = 0; j < locations_.size(); ++j) {
              if (i == j) continue;
              // If another entry starts with our name and is longer, we are a subpath.
              if (locations_[j].name.find(locations_[i].name + " > ") == 0) {
                  isSubpath = true;
                  break;
              }
          }
          if (!isSubpath) filtered.push_back(locations_[i]);
      }

      for (const auto &loc : filtered) {
        std::string status = "Unknown";
        if (loc.accessibility == 2) status = "Normal";
        else if (loc.accessibility == 1) status = "Sequence Break";
        else if (loc.accessibility == 3) status = "Cleared";
        
        std::string type = (loc.id <= 0) ? "[REGION]  " : "[LOCATION]";
        std::cout << "  - " << type << " " << loc.name << " (Level: " << status << ")" << std::endl;
      }
    }
  }

  if (debug_mode_) {
    auto starObj = GetTrackerObject("item__star");
    std::cout << "LogicManager [SUMMARY]: Pass " << iterations
              << " - Unique Accessible: " << locations_.size()
              << " (Stars Stage: " << (starObj ? starObj->stage : -1)
              << ", Acquired: " << (starObj ? starObj->count : -1) << ")"
              << std::endl;
  }

  // Finalize state update for next pass
  lastSlotData_ = slotData;
  lastItemCounts_ = itemCounts;
  lastCheckedLocationIds_ = checkedLocationIds;
  lastMissingLocationIds_ = missingLocationIds;
  lastPlayerNumber_ = playerNumber;
  firstRun_ = false;
}

int LogicManager::GetAccessibility(int64_t locationId) const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  std::string lid = "__id_" + std::to_string(locationId);
  auto it = trackerObjects_.find(lid);
  if (it != trackerObjects_.end()) {
    return it->second->accessibilityLevel;
  }
  return 0;
}

const std::vector<LocationLogic> &LogicManager::GetLocations() const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return locations_;
}

const std::string &LogicManager::GetCurrentGame() const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return currentGame_;
}

void LogicManager::BindGlobals() {
  lua_["PopVersion"] = "0.19.0";
  lua_.set_function("print", [this](sol::variadic_args args) {
    if (!debug_mode_)
      return;
    for (auto arg : args) {
      sol::function tostring = lua_["tostring"];
      if (tostring.valid()) {
        std::cout << tostring(arg).get<std::string>() << "\t";
      } else {
        std::cout << "[lua error]\t";
      }
    }
    std::cout << std::endl;
  });

  lua_.new_usertype<TrackerObject>(
      "TrackerObject", "Active",
      sol::property(&TrackerObject::get_active, &TrackerObject::set_active),
      "CurrentStage",
      sol::property(&TrackerObject::get_stage, &TrackerObject::set_stage),
      "AcquiredCount",
      sol::property(&TrackerObject::get_count, &TrackerObject::set_count),
      "Increment", &TrackerObject::increment, "ChestCount",
      &TrackerObject::chestCount, "AvailableChestCount",
      &TrackerObject::availableChestCount, "AccessibilityLevel",
      &TrackerObject::accessibilityLevel);

  auto tracker = lua_.create_table();
  tracker["ActiveVariantUID"] = "standard";
  tracker["FindObjectForCode"] = [this](sol::object self, std::string code) {
    return GetTrackerObject(code);
  };
  tracker["ProviderCountForCode"] = [this](sol::object self, std::string code) {
    auto obj = GetTrackerObject(code);
    return obj->count;
  };

  tracker["AddMaps"] = [](sol::variadic_args) {};
  tracker["AddItems"] = [](sol::variadic_args) {};
  tracker["AddLocations"] = [](sol::variadic_args) {};
  tracker["AddLayouts"] = [](sol::variadic_args) {};
  tracker["AddVariantHint"] = [](sol::variadic_args) {};
  lua_["Tracker"] = tracker;

  auto scriptHost = lua_.create_table();
  scriptHost["LoadScript"] = [this](sol::object self, std::string path) {
    fs::path fullPath = currentPackPath_ / path;
    if (fs::exists(fullPath)) {
      auto res = lua_.safe_script_file(fullPath.string());
      if (!res.valid()) {
        sol::error err = res;
        std::cerr << "LogicManager: Error in script " << path << ": "
                  << err.what() << std::endl;
      }
    }
  };
  scriptHost["AddMemoryWatch"] = [](sol::variadic_args) {};
  scriptHost["RegisterTimer"] = [](sol::variadic_args) {};
  scriptHost["AddWatchForCode"] = [this](sol::object self, std::string name,
                                        std::string code, sol::function func) {
    watches_[code][name] = func;
  };

  scriptHost["RemoveWatchForCode"] = [this](sol::object self, std::string name,
                                           sol::optional<std::string> code) {
    if (code) {
      auto it = watches_.find(*code);
      if (it != watches_.end()) {
        it->second.erase(name);
      }
    } else {
      // Search all codes for this name
      for (auto &it : watches_) {
        it.second.erase(name);
      }
    }
  };
  scriptHost["IsVisible"] = [](sol::variadic_args) { return false; };
  lua_["ScriptHost"] = scriptHost;

  auto accessibility =
      lua_.create_table_with("None", 0, "Partial", 1, "Inspect", 3,
                             "SequenceBreak", 5, "Normal", 6, "Cleared", 7);
  lua_["Accessibility"] = accessibility;
  lua_["AccessibilityLevel"] = accessibility;
  lua_["PopVersion"] = "0.18.0";

  auto ap_bridge = lua_.create_table();
  ap_bridge["PlayerNumber"] = -1;
  ap_bridge["CheckedLocations"] = lua_.create_table();
  ap_bridge["MissingLocations"] = lua_.create_table();
  ap_bridge["AddClearHandler"] = [this](sol::object self, std::string name,
                                       sol::function cb) {
    clearHandlers_[name] = cb;
  };
  ap_bridge["AddItemHandler"] = [this](sol::object self, std::string name,
                                      sol::function cb) {
    itemHandlers_[name] = cb;
  };
  ap_bridge["AddLocationHandler"] = [this](sol::object self, std::string name,
                                          sol::function cb) {
    locationHandlers_[name] = cb;
  };
  lua_["Archipelago"] = ap_bridge;

  lua_.safe_script(R"LUA(
      function dump_table(t, indent)
          if type(t) ~= "table" then return tostring(t) end
          indent = indent or ""
          local s = "{\n"
          for k, v in pairs(t) do
              s = s .. indent .. "  [" .. tostring(k) .. "] = " .. dump_table(v, indent .. "  ") .. ",\n"
          end
          return s .. indent .. "}"
      end
      _G.AccessibilityLevel = { None = 0, Partial = 1, Inspect = 3, SequenceBreak = 5, Normal = 6, Cleared = 7 }
      
      function __AxoAnd(...)
          local args = {...}
          local res = args[1]
          for i = 2, #args do
              local a, b = res, args[i]
              local ta, tb = type(a), type(b)
              if ta == "boolean" and tb == "boolean" then res = a and b
              elseif ta == "boolean" then res = a and b or 0
              elseif tb == "boolean" then res = b and a or 0
              else res = math.min(a or 0, b or 0) end
          end
          return res
      end

      function __AxoOr(...)
          local args = {...}
          local res = args[1]
          for i = 2, #args do
              local a, b = res, args[i]
              local ta, tb = type(a), type(b)
              if ta == "boolean" and tb == "boolean" then res = a or b
              elseif ta == "boolean" then res = a and 6 or b
              elseif tb == "boolean" then res = b and 6 or a
              else res = math.max(a or 0, b or 0) end
          end
          return res
      end

      function __AxoB(v)
          if type(v) == "boolean" then return v and 6 or 0 end
          if v == true then return 6 end
          if v == false then return 0 end
          local n = tonumber(v) or 0
          return n >= 6 and 6 or 0
      end

      function __AxoEvaluateRules(rules)
          local results = {}
          for i = 1, #rules do
              local r = rules[i]
              local v = 0
              if type(r) == "function" then
                  local status, res = pcall(r)
                  if status then
                      v = tonumber(res) or (res == true and 6 or 0)
                  else
                      -- Expose the raw Lua error for debugging
                      print("LogicManager [LUA ERROR]: " .. tostring(res))
                      v = 0
                  end
              elseif type(r) == "number" then
                  v = r
              elseif type(r) == "boolean" then
                  v = r and 6 or 0
              end
              results[i] = v or 0
          end
          return results
      end
  )LUA");

  auto archipelago = lua_.create_table();
  archipelago["AddClearHandler"] = [this](sol::object self, std::string name,
                                          sol::function cb) {
    clearHandlers_[name] = cb;
  };
  archipelago["AddItemHandler"] = [this](sol::object self, std::string name,
                                         sol::function cb) {
    itemHandlers_[name] = cb;
  };
  archipelago["AddLocationHandler"] = [this](sol::object self, std::string name,
                                             sol::function cb) {
    locationHandlers_[name] = cb;
  };
  archipelago["GetSlotData"] = [this]() {
    return JsonToLua(lua_, lastSlotData_);
  };
  archipelago["CheckedLocations"] = lua_.create_table();
  archipelago["MissingLocations"] = lua_.create_table();
  archipelago["PlayerNumber"] = -1;
  lua_["Archipelago"] = archipelago;
}

std::shared_ptr<TrackerObject>
LogicManager::GetTrackerObject(const std::string &code) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (trackerObjects_.count(code)) {
    return trackerObjects_[code];
  }

  auto obj = std::make_shared<TrackerObject>();
  obj->code = code;
  obj->on_change = [this](std::string c) {
    auto itw = watches_.find(c);
    if (itw != watches_.end()) {
      // Safety copy of the watch list to avoid iterator invalidation
      // if a watch adds/removes other watches.
      std::vector<sol::function> functions;
      for (auto it = itw->second.begin(); it != itw->second.end(); ++it) {
        functions.push_back(it->second);
      }

      for (auto &f : functions) {
        if (f.valid()) {
          auto res = f(c);
          if (!res.valid()) {
            sol::error err = res;
            std::cerr << "LogicManager [LUA ERROR]: Watch failed for " << c
                      << ": " << err.what() << std::endl;
          }
        }
      }
    }
  };
  if (itemDefaults_.count(code)) {
    auto def = itemDefaults_[code];
    obj->set_active(def.active);
    obj->set_count(def.count);
  }
  
  trackerObjects_[code] = obj;
  return obj;
}

std::string LogicManager::TranspileRule(const std::string &rule) {
  if (rule.empty())
    return "";
  if (ruleCache_.count(rule))
    return ruleCache_[rule];
  std::string res = rule;
  res = std::regex_replace(res, std::regex(R"(,)"), " & ");
  std::regex funcPattern(R"((\^?)\$([a-zA-Z0-9_]+)((?:\|[a-zA-Z0-9_/]+)*))");
  auto begin = std::sregex_iterator(res.begin(), res.end(), funcPattern);
  auto end = std::sregex_iterator();
  std::string finalRes = "";
  size_t lastPos = 0;
  for (auto i = begin; i != end; ++i) {
    std::smatch m = *i;
    finalRes += res.substr(lastPos, m.position() - lastPos);
    bool isNumerical = !m[1].str().empty();
    std::string func = m[2];
    std::string argsStr = m[3];
    std::vector<std::string> args;
    if (!argsStr.empty()) {
      std::stringstream ss(argsStr.substr(1));
      std::string s;
      while (std::getline(ss, s, '|'))
        args.push_back(s);
    }
    if (args.empty()) {
      finalRes += isNumerical ? func + "()" : "__AxoB(" + func + "())";
    } else {
      bool hasSlash = args[0].find('/') != std::string::npos;
      if (hasSlash && args.size() == 1) {
        std::string exp = "(";
        std::stringstream ss(args[0]);
        std::string sub;
        bool first = true;
        while (std::getline(ss, sub, '/')) {
          if (!first)
            exp += ", ";
          std::string call = func + "(\"" + sub + "\")";
          exp += isNumerical ? call : "__AxoB(" + call + ")";
          first = false;
        }
        finalRes += "__AxoOr" + exp + ")";
      } else {
        std::string exp = func + "(";
        for (size_t k = 0; k < args.size(); ++k) {
          if (k > 0)
            exp += ", ";
          exp += "\"" + args[k] + "\"";
        }
        exp += ")";
        finalRes += isNumerical ? exp : "__AxoB(" + exp + ")";
      }
    }
    lastPos = m.position() + m.length();
  }
  finalRes += res.substr(lastPos);
  finalRes =
      std::regex_replace(finalRes, std::regex(R"( __INTERNAL_OR__ )"), " | ");

  std::function<std::string(std::string)> processInfix;
  processInfix = [&](std::string s) -> std::string {
    s = std::regex_replace(s, std::regex(R"(^\s*|\s*$)"), "");
    if (s.empty())
      return "0";
    if (s.front() == '(' && s.back() == ')') {
      int balance = 0, i = 0;
      bool fullyWrapped = true;
      for (char c : s) {
        if (c == '(')
          balance++;
        else if (c == ')')
          balance--;
        if (balance == 0 && i < (int)s.size() - 1) {
          fullyWrapped = false;
          break;
        }
        i++;
      }
      if (fullyWrapped)
        return processInfix(s.substr(1, s.size() - 2));
    }
    auto findSplit = [&](char op) -> int {
      int balance = 0;
      for (int i = (int)s.size() - 1; i >= 0; --i) {
        if (s[i] == ')')
          balance++;
        else if (s[i] == '(')
          balance--;
        if (balance == 0 && s[i] == op)
          return i;
      }
      return -1;
    };
    int splitIdx = findSplit('|');
    if (splitIdx != -1)
      return "__AxoOr(" + processInfix(s.substr(0, splitIdx)) + ", " +
             processInfix(s.substr(splitIdx + 1)) + ")";
    splitIdx = findSplit('&');
    if (splitIdx != -1)
      return "__AxoAnd(" + processInfix(s.substr(0, splitIdx)) + ", " +
             processInfix(s.substr(splitIdx + 1)) + ")";
    return s;
  };
  finalRes = processInfix(finalRes);
  finalRes = std::regex_replace(finalRes, std::regex(R"(\[([^\]]+)\])"),
                                " (__AxoOr($1, 5)) ");
  finalRes =
      std::regex_replace(finalRes, std::regex(R"(\{([^\}]+)\})"), " (0) ");
  ruleCache_[rule] = finalRes;
  return finalRes;
}

void LogicManager::LoadLocationsFromPack(
    const std::filesystem::path &packPath) {
  currentPackPath_ = packPath;
  allLocations_.clear();
  uniqueRules_.clear();
  compiledRules_.clear();
  ruleCache_.clear();
  std::unordered_map<std::string, int> ruleToIdx;
  fs::path locDir = packPath / "locations";
  LoadItemsFromPack(packPath / "items");

  // Load Pack Scripts if available
  std::vector<std::string> entryScripts = {
      "scripts/init.lua", "scripts/autotracking/archipelago.lua",
      "scripts/logic.lua"};
  for (const auto &s : entryScripts) {
    fs::path p = packPath / s;
    if (fs::exists(p)) {
      if (debug_mode_)
        std::cout << "LogicManager [DEBUG]: Loading pack script: " << s
                  << std::endl;
      auto res = lua_.safe_script_file(p.string());
      if (!res.valid()) {
        sol::error err = res;
        std::cerr << "LogicManager [LUA ERROR]: Failed to load script " << s
                  << ": " << err.what() << std::endl;
      }
    }
  }
  if (fs::exists(locDir)) {
    int fileCount = 0;
    for (const auto &entry : fs::recursive_directory_iterator(locDir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        fileCount++;
        try {
          std::ifstream f(entry.path());
          json j = json::parse(f, nullptr, true, true);
          if (j.is_array()) {
            for (const auto &node : j)
              ProcessLocationNode(node, {}, "", ruleToIdx);
          } else {
            ProcessLocationNode(j, {}, "", ruleToIdx);
          }
        } catch (const std::exception &e) {
          std::cerr << "LogicManager [ERROR]: Error parsing location file "
                    << entry.path() << ": " << e.what() << std::endl;
        }
      }
    }
    if (debug_mode_) {
      std::cout << "LogicManager [DEBUG]: Loaded " << allLocations_.size()
                << " nodes and " << uniqueRules_.size() << " unique rules from "
                << fileCount << " files." << std::endl;
    }
  }
}

void LogicManager::ProcessLocationNode(
    const json &node, const std::vector<std::string> &parentPath,
    const std::string &parentRule,
    std::unordered_map<std::string, int> &ruleToIdx) {
  if (!node.is_object())
    return;

  std::string nodeName = node.value("name", "");
  std::vector<std::string> fullPath = parentPath;
  if (!nodeName.empty())
    fullPath.push_back(nodeName);

  // Build the flat breadcrumb string from path segments (for logging/compat)
  std::string name;
  for (size_t i = 0; i < fullPath.size(); ++i) {
    if (i > 0) name += " > ";
    name += fullPath[i];
  }

  // Parse Access Rules
  std::string nodeRule = "";
  if (node.contains("access_rules") && node["access_rules"].is_array() &&
      !node["access_rules"].empty()) {
    std::vector<std::string> parts;
    for (const auto &r : node["access_rules"]) {
      if (r.is_string())
        parts.push_back("(" + r.get<std::string>() + ")");
    }
    if (!parts.empty()) {
      nodeRule = parts[0];
      for (size_t i = 1; i < parts.size(); ++i)
        nodeRule += " | " + parts[i];
    }
  }

  // Parse Visibility Rules (Critical for SM64 ER)
  std::string visibilityRule = "";
  if (node.contains("visibility_rules")) {
    if (node["visibility_rules"].is_string()) {
      visibilityRule = node["visibility_rules"];
    } else if (node["visibility_rules"].is_array() &&
               !node["visibility_rules"].empty()) {
      std::vector<std::string> parts;
      for (const auto &r : node["visibility_rules"]) {
        if (r.is_string())
          parts.push_back("(" + r.get<std::string>() + ")");
      }
      if (!parts.empty()) {
        visibilityRule = parts[0];
        for (size_t i = 1; i < parts.size(); ++i)
          visibilityRule += " & " + parts[i];
      }
    }
  }

  // Combine rules: (Parent) AND (Visibility) AND (NodeAccess)
  std::string combinedRule = parentRule;
  if (!visibilityRule.empty()) {
    if (combinedRule.empty())
      combinedRule = visibilityRule;
    else
      combinedRule = "(" + combinedRule + ") & (" + visibilityRule + ")";
  }
  if (!nodeRule.empty()) {
    if (combinedRule.empty())
      combinedRule = nodeRule;
    else
      combinedRule = "(" + combinedRule + ") & (" + nodeRule + ")";
  }

  // Handle ID and Location Entry
  int64_t id = 0;
  if (node.contains("hosted_item") && node["hosted_item"].is_string()) {
    std::string h = node["hosted_item"];
    if (h.find("__location_item_") == 0) {
      try {
        id = std::stoll(h.substr(16));
      } catch (...) {
      }
    }
  }

  if (!name.empty()) {
    LocationLogic ll;
    ll.name = name;
    ll.path = fullPath;
    ll.id = id;
    if (id != 0) {
      ll.logicalId = "__id_" + std::to_string(id);
    } else {
      ll.logicalId = "@" + name;
    }

    if (combinedRule.empty()) {
      ll.ruleIndex = -1;
    } else {
      if (ruleToIdx.count(combinedRule)) {
        ll.ruleIndex = ruleToIdx[combinedRule];
      } else {
        ll.ruleIndex = (int)uniqueRules_.size();
        uniqueRules_.push_back(combinedRule);
        ruleToIdx[combinedRule] = ll.ruleIndex;

        std::string luaCode =
            "return function() return " + TranspileRule(combinedRule) + " end";
        // Diagnostic logging removed for final stabilization
        try {
          auto res = lua_.load(luaCode);
          if (res.valid()) {
            sol::protected_function wrapper = res;
            sol::protected_function_result pfr = wrapper();
            if (pfr.valid()) {
              compiledRules_.push_back(pfr.get<sol::object>());
            } else {
              compiledRules_.push_back(sol::make_object(lua_, 0));
            }
          } else {
            compiledRules_.push_back(sol::make_object(lua_, 0));
          }
        } catch (...) {
          compiledRules_.push_back(sol::make_object(lua_, 0));
        }
      }
    }
    allLocations_.push_back(ll);
  }

  // Handle children nodes
  if (node.contains("children") && node["children"].is_array()) {
    for (const auto &child : node["children"]) {
      ProcessLocationNode(child, fullPath, combinedRule, ruleToIdx);
    }
  }

  // Handle sections (Check nodes in PopTracker)
  if (node.contains("sections") && node["sections"].is_array()) {
    for (const auto &section : node["sections"]) {
      ProcessLocationNode(section, fullPath, combinedRule, ruleToIdx);
    }
  }
}
