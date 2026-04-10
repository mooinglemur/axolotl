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

static std::atomic<int> s_lm_id_counter{0};

LogicManager::LogicManager() {
  instance_id_ = ++s_lm_id_counter;
  lua_.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table,
                      sol::lib::string, sol::lib::math, sol::lib::bit32,
                      sol::lib::os);
  // Sandbox: remove dangerous os functions — packs only need time/date/clock
  lua_["os"]["execute"] = sol::lua_nil;
  lua_["os"]["exit"] = sol::lua_nil;
  lua_["os"]["remove"] = sol::lua_nil;
  lua_["os"]["rename"] = sol::lua_nil;
  lua_["os"]["tmpname"] = sol::lua_nil;
  BindGlobals();
}

LogicManager::~LogicManager() {
  if (load_thread_.joinable())
    load_thread_.join();
}

bool LogicManager::LoadPackAsync(const std::string &game,
                                 const nlohmann::json &slotData) {
  if (load_state_.load() == LoadState::Loading)
    return false;
  pending_game_ = game;
  load_state_ = LoadState::Loading;
  ids_resolved_ = false;
  if (load_thread_.joinable())
    load_thread_.join();
  load_thread_ = std::thread([this, game, slotData]() {
    bool ok = LoadPack(game, slotData);
    load_state_ = ok ? LoadState::Ready : LoadState::Error;
  });
  return true;
}

void LogicManager::ForceResync() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  firstRun_ = true;
  lastItemCounts_.clear();
  lastCheckedLocationIds_.clear();
  lastMissingLocationIds_.clear();
  lastPlayerNumber_ = -1;
  nextItemHandlerIndex_ = 1;
}

void LogicManager::Reset() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  currentGame_ = "";
  locations_.clear();
  allLocations_.clear();
  idAliases_.clear();
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
  ids_resolved_ = false;
  stageCodeLinks_.clear();
  luaItems_.clear();
  nextItemHandlerIndex_ = 1;
  itemHistory_.clear();
  itemHandlers_.clear();
  locationHandlers_.clear();
  clearHandlers_.clear();

  // Reset Lua state
  lua_ = sol::state();
  lua_.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table,
                      sol::lib::string, sol::lib::math, sol::lib::bit32,
                      sol::lib::os);
  // Sandbox: remove dangerous os functions — packs only need time/date/clock
  lua_["os"]["execute"] = sol::lua_nil;
  lua_["os"]["exit"] = sol::lua_nil;
  lua_["os"]["remove"] = sol::lua_nil;
  lua_["os"]["rename"] = sol::lua_nil;
  lua_["os"]["tmpname"] = sol::lua_nil;
  BindGlobals();
}

void LogicManager::SetDebugMode(bool debug) {
  debug_mode_ = debug;
  lua_["AUTOTRACKER_ENABLE_DEBUG_LOGGING_AP"] = debug;
}

bool LogicManager::LoadPack(const std::string &game,
                            const nlohmann::json &slotData) {
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

    // Select the best variant UID for logic tracking.
    // Priority: (1) ap-flagged variant whose name contains "map"
    // (case-insensitive)
    //           — these typically load the most complete logic (region access,
    //             location data); (2) any other ap-flagged variant; (3)
    //             "standard".
    std::string variantUID = "standard";
    std::string fallbackApVariant;
    if (manifest.contains("variants") && manifest["variants"].is_object()) {
      for (auto it = manifest["variants"].begin();
           it != manifest["variants"].end(); ++it) {
        const auto &v = it.value();
        if (!v.contains("flags") || !v["flags"].is_array())
          continue;
        bool isAp = false;
        for (const auto &flag : v["flags"])
          if (flag.is_string() && flag.get<std::string>() == "ap") {
            isAp = true;
            break;
          }
        if (!isAp)
          continue;

        std::string key = it.key();
        std::string keyLower = key;
        std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
                       ::tolower);
        if (keyLower.find("map") != std::string::npos) {
          variantUID = key;
          break; // map variant found — stop immediately
        }
        if (fallbackApVariant.empty())
          fallbackApVariant = key;
      }
      if (variantUID == "standard" && !fallbackApVariant.empty())
        variantUID = fallbackApVariant;

      // If slot data indicates entrance randomization, prefer a variant whose
      // key contains "entrance_rando" or "er_" over the base ap variant.
      if (!slotData.is_null() && slotData.is_object()) {
        bool hasEr = false;
        for (auto it2 = slotData.begin(); it2 != slotData.end(); ++it2) {
          const std::string &k = it2.key();
          if (k.rfind("randomize_", 0) == 0 && k.find("entrance") != std::string::npos) {
            auto &v = it2.value();
            if ((v.is_number() && v.get<int>() != 0) ||
                (v.is_boolean() && v.get<bool>())) {
              hasEr = true;
              break;
            }
          }
        }
        if (hasEr && manifest.contains("variants") &&
            manifest["variants"].is_object()) {
          for (auto it2 = manifest["variants"].begin();
               it2 != manifest["variants"].end(); ++it2) {
            const auto &vv = it2.value();
            if (!vv.contains("flags") || !vv["flags"].is_array())
              continue;
            bool isAp2 = false;
            for (const auto &f : vv["flags"])
              if (f.is_string() && f.get<std::string>() == "ap") {
                isAp2 = true;
                break;
              }
            if (!isAp2)
              continue;
            std::string k2 = it2.key();
            std::string k2l = k2;
            std::transform(k2l.begin(), k2l.end(), k2l.begin(), ::tolower);
            if (k2l.find("entrance_rando") != std::string::npos ||
                k2l.find("entrance-rando") != std::string::npos ||
                k2l.rfind("er_", 0) == 0 || k2l.rfind("_er", k2l.size()-3) != std::string::npos) {
              variantUID = k2;
              break;
            }
          }
        }
      }
    }
    pending_variant_ = variantUID;
    loaded_with_slot_data_ = slotData.is_object() && !slotData.empty();

    std::string path = lua_["package"]["path"];
    path += ";" + (packPath / "scripts" / "?.lua").string();
    path += ";" + (packPath / "?.lua").string();
    lua_["package"]["path"] = path;

    lua_["GAME_NAME"] = game;
    lua_["CURRENT_GAME"] = game;
    // Update Tracker.ActiveVariantUID now that we know which variant to use.
    lua_["Tracker"]["ActiveVariantUID"] = variantUID;
    if (debug_mode_)
      std::cerr << "LogicManager [DEBUG]: Using variant UID: " << variantUID
                << std::endl;

    itemDefaults_.clear();
    clearHandlers_.clear();
    itemHandlers_.clear();
    locationHandlers_.clear();
    trackerObjects_.clear();

    // Clear location state before the script runs so that packs which call
    // Tracker:AddLocations() from their init script populate a clean slate.
    // LoadLocationsFromPack() will later add any locations/ directory entries
    // without clearing again.
    allLocations_.clear();
    loaded_location_files_.clear();
    uniqueRules_.clear();
    compiledRules_.clear();
    ruleCache_.clear();
    stageCodeLinks_.clear();
    luaItems_.clear();

    // Marker for first logic pass
    firstRun_ = true;
    lastItemNameCounts_.clear();
    lastItemCounts_.clear();
    lastCheckedLocationIds_.clear();
    lastMissingLocationIds_.clear();
    lastPlayerNumber_ = -1;

    // Load items BEFORE the script so that any logic evaluation that happens
    // during script init (e.g. SoH's _compute_child_adult_only_regions) sees
    // correct item types and defaults rather than blank TrackerObjects.
    LoadItemsFromPack(packPath / "items");

    // Load the pack's entry script FIRST so all Lua functions are defined
    // before location JSON files are processed and rules are compiled.
    lua_.script_file((packPath / entry).string());

    // Create lowercase aliases for any capitalized global functions so that
    // packs with inconsistent casing (e.g. $bianco4 vs Bianco4) still work.
    lua_.safe_script(R"LUA(
      for k, v in pairs(_G) do
        if type(v) == "function" then
          local lower = string.lower(k)
          if lower ~= k and _G[lower] == nil then
            _G[lower] = v
          end
        end
      end
    )LUA");

    LoadLocationsFromPack(packPath);

    if (debug_mode_)
      std::cout << "LogicManager: Loaded " << allLocations_.size()
                << " nodes and " << uniqueRules_.size() << " unique rules."
                << std::endl;

    return true;
  } catch (const std::exception &e) {
    if (debug_mode_)
      std::cerr << "LogicManager Load Error: " << e.what() << std::endl;
    return false;
  }
}

void LogicManager::ResolveLocationIds(
    const std::map<std::string, int64_t> &nameToId) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (ids_resolved_)
    return;
  ids_resolved_ = true;

  int resolved = 0;

  // --- Strategy A: LOCATION_MAPPING from Lua (standard PopTracker AP packs)
  // --- Many packs define a global LOCATION_MAPPING table: [ap_id] =
  // {"@Area/Sec", ...} We can use this directly instead of name-guessing.
  {
    sol::object locMapObj = lua_["LOCATION_MAPPING"];
    if (locMapObj.is<sol::table>()) {
      // Build a path-string → &LocationLogic index for O(log n) lookup.
      // Joins path segments with "/" to match the "@Area/Section" format.
      std::map<std::string, LocationLogic *> pathToLoc;
      for (auto &loc : allLocations_) {
        if (loc.id != 0 || loc.path.empty())
          continue;
        std::string joined;
        for (size_t i = 0; i < loc.path.size(); ++i) {
          if (i)
            joined += '/';
          joined += loc.path[i];
        }
        pathToLoc.emplace(joined, &loc); // first match wins on duplicates
      }

      sol::table locMap = locMapObj.as<sol::table>();
      if (debug_mode_) {
        int tableSize = 0;
        for (auto &kv : locMap)
          (void)kv, ++tableSize;
        std::cerr << "LogicManager [DEBUG] #" << instance_id_
                  << ": LOCATION_MAPPING has " << tableSize
                  << " entries, pathToLoc has " << pathToLoc.size()
                  << " entries\n";
      }
      for (auto &kv : locMap) {
        // Lua table keys for AP IDs are numbers (doubles in Lua 5.1/5.2,
        // integers in 5.3+). Use sol::type::number check + double conversion
        // to avoid int32 overflow for large AP IDs (e.g. 8112000000 > INT_MAX).
        if (kv.first.get_type() != sol::type::number)
          continue;
        int64_t apId = (int64_t)kv.first.as<double>();
        if (apId == 0)
          continue;

        if (!kv.second.is<sol::table>())
          continue;
        sol::table entry = kv.second.as<sol::table>();
        sol::object pathObj = entry[1];
        // Some packs double-nest: {{"@Area/Section"}} — unwrap one level.
        if (pathObj.is<sol::table>())
          pathObj = pathObj.as<sol::table>()[1];
        if (!pathObj.is<std::string>())
          continue;

        std::string popPath = pathObj.as<std::string>();
        if (popPath.size() < 2 || popPath[0] != '@')
          continue;
        popPath = popPath.substr(1); // strip leading '@'

        auto it = pathToLoc.find(popPath);
        if (it != pathToLoc.end()) {
          LocationLogic *loc2 = it->second;
          if (loc2->id == 0) {
            // First ID for this section — assign as primary
            loc2->id = apId;
            loc2->logicalId = "__id_" + std::to_string(apId);
            ++resolved;
          } else if (loc2->id != apId) {
            // Additional ID for the same section (multi-item) — record alias
            idAliases_[apId] = loc2->id;
          }
        }
      }
    }
  }

  // --- Propagation pass: before Strategy B, propagate a resolved child ID up
  // to its parent container node (id=0) when that container has exactly one
  // direct child with a resolved id.  This prevents Strategy B from
  // mistakenly assigning a same-named but different-area ID to the container
  // (e.g. "Sign" in Sirena Beach getting the Pianta Village "Sign" id).
  for (auto &loc : allLocations_) {
    if (loc.id != 0 || loc.path.empty())
      continue;
    int64_t childId = 0;
    int childCount = 0;
    for (const auto &child : allLocations_) {
      if (child.id == 0)
        continue;
      if (child.path.size() != loc.path.size() + 1)
        continue;
      bool isChild = true;
      for (size_t i = 0; i < loc.path.size(); ++i) {
        if (child.path[i] != loc.path[i]) {
          isChild = false;
          break;
        }
      }
      if (isChild) {
        childId = child.id;
        ++childCount;
      }
    }
    if (childCount == 1) {
      loc.id = childId;
      loc.logicalId = "__id_" + std::to_string(childId);
    }
  }

  // --- Strategy B: name-based matching (fallback for packs without
  // LOCATION_MAPPING) --- Build an index: leaf_name (after last " - ") → list
  // of (id, full_name)
  std::map<std::string, std::vector<std::pair<int64_t, std::string>>> leafIdx;
  for (auto &[full_name, id] : nameToId) {
    // Index by full name
    leafIdx[full_name].push_back({id, full_name});
    // Index by leaf (part after last " - ")
    auto dash = full_name.rfind(" - ");
    if (dash != std::string::npos) {
      std::string leaf = full_name.substr(dash + 3);
      if (leaf != full_name)
        leafIdx[leaf].push_back({id, full_name});
    }
  }

  for (auto &loc : allLocations_) {
    if (loc.id != 0 || loc.path.empty())
      continue;

    const std::string &leaf = loc.path.back();

    // 1. Exact full name match
    auto it = nameToId.find(leaf);
    if (it != nameToId.end()) {
      loc.id = it->second;
      loc.logicalId = "__id_" + std::to_string(loc.id);
      ++resolved;
      continue;
    }

    // Helper: resolve from a candidate list using parent path scoring
    auto resolveFromCandidates =
        [&](const std::vector<std::pair<int64_t, std::string>> &candidates)
        -> bool {
      if (candidates.size() == 1) {
        loc.id = candidates[0].first;
        loc.logicalId = "__id_" + std::to_string(loc.id);
        ++resolved;
        return true;
      }
      int64_t best_id = 0;
      int best_score = -1;
      for (auto &[cid, cname] : candidates) {
        int score = 0;
        for (size_t i = 0; i + 1 < loc.path.size(); ++i) {
          if (cname.find(loc.path[i]) != std::string::npos)
            ++score;
        }
        if (score > best_score) {
          best_score = score;
          best_id = cid;
        }
      }
      if (best_id) {
        loc.id = best_id;
        loc.logicalId = "__id_" + std::to_string(loc.id);
        ++resolved;
        return true;
      }
      return false;
    };

    // 2. Leaf suffix match using the display name
    auto lit = leafIdx.find(leaf);
    if (lit != leafIdx.end()) {
      if (resolveFromCandidates(lit->second))
        continue;
    }

    // 3. Try path[-2] — the parent node name. Common in packs where the leaf is
    //    a generic name like "Blue Coin" or "Star" and the AP name matches the
    //    parent instead (e.g. "River End > Blue Coin" → AP "... - River End").
    if (loc.path.size() >= 2) {
      const std::string &parent = loc.path[loc.path.size() - 2];
      if (parent != leaf) {
        auto plit = leafIdx.find(parent);
        if (plit != leafIdx.end()) {
          if (resolveFromCandidates(plit->second))
            continue;
        }
      }
    }

    // 4. Fallback: try the refLeaf (second-to-last segment of a PopTracker
    //    "ref" path), which often matches the AP data package leaf name when
    //    the display name differs.
    if (!loc.refLeaf.empty() && loc.refLeaf != leaf) {
      auto rlit = leafIdx.find(loc.refLeaf);
      if (rlit != leafIdx.end())
        resolveFromCandidates(rlit->second);
    }
  }

  if (debug_mode_)
    std::cerr << "LogicManager #" << instance_id_ << ": Resolved " << resolved
              << " location IDs from data package.\n";

  // Rebuild the id→accessibility cache with the new IDs
  accessibilityCache_.clear();
  // Force a fresh convergence pass next UpdateLogic call
  firstRun_ = true;
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
      // Codes may be comma-separated (e.g. "gifts,gifts_on") — split them.
      std::stringstream css(j["codes"].get<std::string>());
      std::string ctok;
      while (std::getline(css, ctok, ',')) {
        auto cs = ctok.find_first_not_of(" \t");
        auto ce = ctok.find_last_not_of(" \t");
        if (cs != std::string::npos)
          codes.push_back(ctok.substr(cs, ce - cs + 1));
      }
    }
  }

  if (codes.empty() && !(j.contains("stages") && j["stages"].is_array()))
    return;

  // For progressive items, build stage code links so ProviderCountForCode
  // can check stage-specific codes against the primary object's current stage.
  std::string type = j.value("type", "toggle");
  if (type == "progressive" && j.contains("stages") && j["stages"].is_array()) {
    bool defaultInherit = j.value("inherit_codes", true);
    std::string primaryCode;

    int stageIdx = 0;
    for (const auto &stage : j["stages"]) {
      bool inherit = stage.value("inherit_codes", defaultInherit);
      std::vector<std::string> sc;
      auto splitAndAppend = [&sc](const std::string &s) {
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
          auto start = tok.find_first_not_of(" \t");
          auto end = tok.find_last_not_of(" \t");
          if (start != std::string::npos)
            sc.push_back(tok.substr(start, end - start + 1));
        }
      };
      if (stage.contains("codes") && stage["codes"].is_string())
        splitAndAppend(stage["codes"].get<std::string>());
      else if (stage.contains("code") && stage["code"].is_string())
        sc.push_back(stage["code"].get<std::string>());

      if (!sc.empty() && primaryCode.empty())
        primaryCode = sc[0];

      for (const auto &c : sc)
        stageCodeLinks_[c].push_back({primaryCode, stageIdx, inherit});

      ++stageIdx;
    }

    // Ensure primary object exists with defaults, and tag its type.
    if (!primaryCode.empty()) {
      auto pobj = GetTrackerObject(primaryCode);
      if (pobj && pobj->type.empty())
        pobj->type = "progressive";
    }
  }

  if (codes.empty())
    return;

  ItemDefault def;
  if (j.contains("initial_active_state")) {
    auto &v = j["initial_active_state"];
    if (v.is_boolean())
      def.active = v.get<bool>();
    else if (v.is_string())
      def.active = (v.get<std::string>() == "true");
  } else if (j.contains("active")) {
    auto &v = j["active"];
    if (v.is_boolean())
      def.active = v.get<bool>();
    else if (v.is_string())
      def.active = (v.get<std::string>() == "true");
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
      if (obj->type.empty())
        obj->type = type;
      obj->active = def.active;
      obj->stage = def.stage;
      obj->count = def.count;
    }
  }
}

void LogicManager::RunConvergenceOnce() {
  // Clear the stale flag BEFORE running to prevent re-entrant calls
  // (rule evaluation is read-only and won't re-trigger on_change).
  accessibility_stale_ = false;

  if (compiledRules_.empty())
    return;

  auto rulesTable = lua_.create_table();
  for (size_t i = 0; i < compiledRules_.size(); ++i)
    rulesTable[i + 1] = compiledRules_[i];
  sol::function eval = lua_["__AxoEvaluateRules"];
  if (!eval.valid())
    return;
  auto res = eval(rulesTable);
  if (!res.valid())
    return;
  sol::table results = res.get<sol::table>();

  for (const auto &loc : allLocations_) {
    if (current_checked_ids_.count(loc.id))
      continue;

    int v = 0;
    if (loc.ruleIndex != -1) {
      sol::object r = results[static_cast<size_t>(loc.ruleIndex + 1)];
      if (r.is<int>())
        v = r.as<int>();
      else if (r.is<bool>())
        v = r.as<bool>() ? 6 : 0;
    } else {
      v = 6;
    }

    int access = (v >= 6) ? 2 : (v > 0 ? 1 : 0);
    auto obj = GetTrackerObject(loc.logicalId);
    if (obj && obj->accessibilityLevel < 3)
      obj->accessibilityLevel = access;
  }
}

void LogicManager::UpdateLogic(const std::map<int64_t, int> &itemCounts,
                               const nlohmann::json &slotData,
                               const std::set<int64_t> &checkedLocationIds,
                               const std::set<int64_t> &missingLocationIds,
                               int playerNumber) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);

  if (load_state_.load() != LoadState::Ready)
    return;

  if (!firstRun_ && slotData == lastSlotData_ &&
      itemCounts == lastItemCounts_ &&
      checkedLocationIds == lastCheckedLocationIds_ &&
      missingLocationIds == lastMissingLocationIds_ &&
      playerNumber == lastPlayerNumber_) {
    return;
  }

  bool isNewSession = firstRun_ || lastSlotData_ != slotData;
  current_checked_ids_ = checkedLocationIds;

  // Sync SLOT_DATA to Lua
  auto luaSlotData = JsonToLua(lua_, slotData);
  lua_["SLOT_DATA"] = luaSlotData;

  // Sync Archipelago global state
  sol::table archipelago = lua_["Archipelago"];
  archipelago["SlotData"] = luaSlotData;
  archipelago["PlayerNumber"] = playerNumber;
  archipelago["TeamNumber"] = 0;

  // Define a robust Lua execution helper
  auto executeLuaHandler = [&](const std::string &name, sol::function func,
                               auto... args) {
    if (!func.valid())
      return;

    // Ensure all logic results are pushed to Lua objects before running
    // handlers (This allows obj.AccessibilityLevel to be correct inside
    // areaReveal)
    for (auto const &[lid, obj] : trackerObjects_) {
      // No-op to trigger property getter refresh if needed (sol2 usually
      // handles this)
    }

    auto res = func(args...);
    if (!res.valid()) {
      sol::error err = res;
      std::cerr << "LogicManager [LUA ERROR]: Error in " << name << ": "
                << err.what() << std::endl;
    }
  };

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

    // Populate CheckedLocations and MissingLocations before firing onClear
    // so that pack scripts (e.g. SM64) can read them inside their handler.
    {
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
    }

    for (auto const &it : clearHandlers_) {
      executeLuaHandler("onClear", it.second, luaSlotData);
    }
  }

  // ITEM SYNC: Detect and replay any items received since last update.
  // The index passed to onItem must be a monotonically-increasing global
  // counter that persists across UpdateLogic() calls. Pack scripts (e.g. SM64)
  // use a CUR_INDEX guard to skip already-processed items, so resetting to 1
  // each call would cause every incremental item to be silently dropped.
  {
    if (isNewSession)
      nextItemHandlerIndex_ = 1;
    int index = nextItemHandlerIndex_;
    for (auto const &ic : itemCounts) {
      int64_t id = ic.first;
      int count = ic.second;
      int previousCount =
          lastItemCounts_.count(id) ? lastItemCounts_.at(id) : 0;

      if (count > previousCount || isNewSession) {
        int itemsToReplay = isNewSession ? count : (count - previousCount);
        std::string itemName = "unnamed_item";
        for (const auto &l : allLocations_) {
          if (l.id == id) {
            itemName = l.name;
            break;
          }
        }

        for (int i = 0; i < itemsToReplay; ++i) {
          for (auto const &it : itemHandlers_) {
            executeLuaHandler("onItem", it.second, index++, id, itemName,
                              playerNumber);
          }
        }
      }
    }
    nextItemHandlerIndex_ = index;
  }

  // LOCATION SYNC: Detect and replay any locations checked since last update.
  {
    int locCount = 0;
    for (int64_t id : checkedLocationIds) {
      if (isNewSession || lastCheckedLocationIds_.count(id) == 0) {
        std::string locName = "checked_location";
        for (const auto &l : allLocations_) {
          if (l.id == id) {
            locName = l.name;
            break;
          }
        }

        for (auto const &it : locationHandlers_) {
          executeLuaHandler("onLocation", it.second, id, locName);
          locCount++;
        }
      }
    }
    if (debug_mode_ && locCount > 0)
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

  // Always mark every server-checked location as cleared
  // (accessibilityLevel=3). The convergence loop skips allLocations_ entries
  // whose id is in checkedLocationIds, so it never populates currentPassMax for
  // those logicalIds and therefore never resets their TrackerObject. Without
  // this, a freshly-checked location retains its previous accessibilityLevel
  // (e.g. 2) and incorrectly remains visible in the UI.
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
      if (res.valid())
        return res.get<sol::table>();
    }
    return lua_.create_table();
  };

  auto tracker = lua_["Tracker"];

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
        // 2. If accessibility is equal, prefer a meaningful name over
        // boilerplate.
        // 3. If both are equal, LOCK the first name found (use strict > for
        // quality check).

        bool isCurrentBoilerplate =
            loc.name.find("Entrance Accessibility") != std::string::npos ||
            loc.name.find("Unknown Stage") != std::string::npos ||
            (loc.name.find(" > ") == std::string::npos && loc.id <= 0);

        bool hasPriorName = currentPassName.count(loc.logicalId);
        bool existingIsBoilerplate =
            hasPriorName &&
            (currentPassName[loc.logicalId].find("Entrance Accessibility") !=
                 std::string::npos ||
             currentPassName[loc.logicalId].find("Unknown Stage") !=
                 std::string::npos ||
             (currentPassName[loc.logicalId].find(" > ") == std::string::npos &&
              currentPassMax[loc.logicalId] <= 0));

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
    if (iterations == 1) {
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
        if (debug_mode_)
          std::cout << "LogicManager [DEBUG]: Detected change in "
                       "items/locations/session - Forcing convergence"
                    << std::endl;
      }
    }

    if (!logicChanged || iterations == 10) {
      maxAccessByLogicalId = currentPassMax;
      maxPathByLogicalId = currentPassName;
      maxPathSegsById = currentPassPath;
    }
  }

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
        // We only want to show meaningful Regions (Level Entrances) and actual
        // Locations. Safer ID check to capture any node that isn't a
        // server-tracked location.
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
        if (finalPath.empty())
          finalPath =
              loc.name; // Fallback to raw name if path tracking missed it
        entry.name = finalPath;

        auto pathIt = maxPathSegsById.find(loc.logicalId);
        if (pathIt != maxPathSegsById.end() && !pathIt->second.empty())
          entry.path = pathIt->second;
        // else: entry.path retains loc.path set during load (already a valid
        // fallback)

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
    // Mirror accessibility from primary to alias IDs (multi-item sections)
    for (const auto &[aliasId, primaryId] : idAliases_) {
      auto it = accessibilityCache_.find(primaryId);
      if (it != accessibilityCache_.end())
        accessibilityCache_[aliasId] = it->second;
    }

    if (debug_mode_) {
      std::cout << "LogicManager [UI CONTENT]:" << std::endl;
      // Filter out redundant path segments.
      // If we have "A > B" and "A > B > C", we only want to show "A > B > C" in
      // the terminal report.
      std::vector<LocationLogic> filtered;
      for (size_t i = 0; i < locations_.size(); ++i) {
        bool isSubpath = false;
        for (size_t j = 0; j < locations_.size(); ++j) {
          if (i == j)
            continue;
          // If another entry starts with our name and is longer, we are a
          // subpath.
          if (locations_[j].name.find(locations_[i].name + " > ") == 0) {
            isSubpath = true;
            break;
          }
        }
        if (!isSubpath)
          filtered.push_back(locations_[i]);
      }

      for (const auto &loc : filtered) {
        std::string status = "Unknown";
        if (loc.accessibility == 2)
          status = "Normal";
        else if (loc.accessibility == 1)
          status = "Sequence Break";
        else if (loc.accessibility == 3)
          status = "Cleared";

        std::string type = (loc.id <= 0) ? "[REGION]  " : "[LOCATION]";
        std::cout << "  - " << type << " " << loc.name << " (Level: " << status
                  << ")" << std::endl;
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

  // Coercing setters: PopTracker packs mix booleans and numbers freely for
  // Active (bool), CurrentStage (int), and AcquiredCount (int). Sol2 with
  // safeties on rejects type mismatches, so we coerce via sol::object lambdas.
  auto lua_to_bool = [](sol::object v) -> bool {
    if (v.is<bool>())
      return v.as<bool>();
    if (v.is<int>())
      return v.as<int>() != 0;
    if (v.is<double>())
      return v.as<double>() != 0.0;
    return false;
  };
  auto lua_to_int = [](sol::object v) -> int {
    if (v.is<int>())
      return v.as<int>();
    if (v.is<double>())
      return (int)v.as<double>();
    if (v.is<bool>())
      return v.as<bool>() ? 1 : 0;
    if (v.is<std::string>()) {
      try { return std::stoi(v.as<std::string>()); } catch (...) {}
    }
    return 0;
  };
  lua_.new_usertype<TrackerObject>(
      "TrackerObject", "Active",
      sol::property(&TrackerObject::get_active,
                    [lua_to_bool](TrackerObject &obj, sol::object v) {
                      obj.set_active(lua_to_bool(v));
                    }),
      "CurrentStage",
      sol::property(&TrackerObject::get_stage,
                    [lua_to_int](TrackerObject &obj, sol::object v) {
                      obj.set_stage(lua_to_int(v));
                    }),
      "AcquiredCount",
      sol::property(&TrackerObject::get_count,
                    [lua_to_int](TrackerObject &obj, sol::object v) {
                      obj.set_count(lua_to_int(v));
                    }),
      "Increment", &TrackerObject::increment, "ChestCount",
      &TrackerObject::chestCount, "AvailableChestCount",
      &TrackerObject::availableChestCount, "AccessibilityLevel",
      sol::property(
          [this](TrackerObject &obj) -> int {
            // Lazy evaluation: if any item state changed since last
            // convergence, run one pass now so pack scripts (e.g. areaReveal)
            // see current values.
            if (accessibility_stale_)
              RunConvergenceOnce();
            // Return PopTracker-scale accessibility (0/5/6) so that Lua pack
            // scripts comparing against AccessibilityLevel.Partial (1) or
            // AccessibilityLevel.Normal (6) work correctly. Our internal scale
            // is 0=None, 1=Partial, 2=Full, 3=Cleared.
            switch (obj.accessibilityLevel) {
              case 1:  return 5; // Partial / SequenceBreak
              case 2:  return 6; // Full / Normal
              case 3:  return 7; // Cleared
              default: return 0; // None
            }
          },
          [](TrackerObject &obj, int v) {
            switch (v) {
              case 5: obj.accessibilityLevel = 1; break; // SequenceBreak
              case 6: obj.accessibilityLevel = 2; break; // Normal/Full
              case 7: obj.accessibilityLevel = 3; break; // Cleared
              default: obj.accessibilityLevel = 0; break; // None
            }
          }),
      "Highlight",
      sol::property([](TrackerObject &) { return 0; },
                    [](TrackerObject &, sol::object) {}),
      "SetOverlay",           [](TrackerObject &, sol::variadic_args) {},
      "SetOverlayBackground", [](TrackerObject &, sol::variadic_args) {},
      "SetOverlayColor",      [](TrackerObject &, sol::variadic_args) {},
      "SetOverlayFontSize",   [](TrackerObject &, sol::variadic_args) {},
      "SetOverlayAlign",      [](TrackerObject &, sol::variadic_args) {},
      "BadgeText",
      sol::property([](TrackerObject &) -> std::string { return ""; },
                    [](TrackerObject &, sol::object) {}),
      "Type", sol::property([](TrackerObject &obj) -> std::string {
        return obj.type.empty() ? "toggle" : obj.type;
      }),
      sol::meta_function::new_index,
      [](TrackerObject &obj, sol::this_state L, std::string key,
         sol::object val) {
        sol::state_view sv(L);
        if (!obj.extra_props_.valid())
          obj.extra_props_ = sv.create_table();
        obj.extra_props_[key] = val;
      },
      sol::meta_function::index,
      [](TrackerObject &obj, sol::this_state L, std::string key) -> sol::object {
        sol::state_view sv(L);
        if (!obj.extra_props_.valid())
          obj.extra_props_ = sv.create_table();
        sol::object val = obj.extra_props_[key];
        if (val.valid() && val.get_type() != sol::type::lua_nil)
          return val;
        // Auto-create a proxy table for sub-field access (e.g. item.ItemState.x=1).
        // __AxoProxy() returns a table whose unknown reads return further proxies
        // and whose # operator returns 0, preventing "attempt to get length of nil"
        // when packs check #obj.SomeField on a field that was never explicitly set.
        sol::function proxy_fn = sv["__AxoProxy"];
        if (!proxy_fn.valid())
          return sol::make_object(L, sol::lua_nil);
        sol::table t = proxy_fn();
        obj.extra_props_[key] = t;
        return t;
      });

  auto tracker = lua_.create_table();
  tracker["ActiveVariantUID"] = "standard";
  tracker["FindObjectForCode"] = [this](sol::object self, std::string code) {
    // Route stage-alias codes to their primary object so that packs that call
    // FindObjectForCode("progression_ticket") get the right TrackerObject to
    // read/write the stage on.
    auto it = stageCodeLinks_.find(code);
    if (it != stageCodeLinks_.end() && !it->second.empty())
      return GetTrackerObject(it->second[0].primaryCode);
    return GetTrackerObject(code);
  };
  tracker["ProviderCountForCode"] = [this](sol::object self,
                                           std::string code) -> int {
    // Check if this is a stage code for a progressive item
    auto it = stageCodeLinks_.find(code);
    if (it != stageCodeLinks_.end() && !it->second.empty()) {
      int total = 0;
      for (const auto &link : it->second) {
        auto primary = GetTrackerObject(link.primaryCode);
        int currentStage = primary->stage;
        if (link.inherit) {
          // inherit_codes: true — code is provided by all stages up to current
          if (currentStage >= link.stageIdx)
            ++total;
        } else {
          // inherit_codes: false — code is provided only at exactly this stage
          if (currentStage == link.stageIdx)
            ++total;
        }
      }
      return total;
    }
    // Check LuaItems (custom items created via ScriptHost:CreateLuaItem())
    int luaTotal = 0;
    for (auto &item : luaItems_) {
      sol::object pcf = item["ProvidesCodeFunc"];
      if (pcf.is<sol::function>()) {
        auto res = pcf.as<sol::function>()(item, code);
        if (res.valid()) {
          sol::object val = res;
          if (val.is<int>())
            luaTotal += val.as<int>();
          else if (val.is<bool>() && val.as<bool>())
            ++luaTotal;
        }
      }
    }
    if (luaTotal > 0)
      return luaTotal;

    // Not a registered stage code — use the object directly.
    // Toggle items use active; consumables use count.
    auto obj = GetTrackerObject(code);
    return obj->count > 0 ? obj->count : (obj->active ? 1 : 0);
  };

  tracker["AddMaps"] = [](sol::variadic_args) {};
  tracker["AddItems"] = [this](sol::object /*self*/, std::string relPath) {
    fs::path fullPath = currentPackPath_ / relPath;
    if (!fs::exists(fullPath)) return;
    try {
      std::ifstream f(fullPath);
      json j = json::parse(f, nullptr, true, true);
      auto processArray = [&](const json &arr) {
        for (const auto &item : arr) {
          if (item.is_object())
            ProcessItemJson(item);
          else if (item.is_array())
            for (const auto &sub : item)
              if (sub.is_object()) ProcessItemJson(sub);
        }
      };
      if (j.is_array()) processArray(j);
      else if (j.is_object()) ProcessItemJson(j);
    } catch (const std::exception &e) {
      std::cerr << "LogicManager [ERROR]: AddItems " << relPath << ": "
                << e.what() << std::endl;
    }
  };
  tracker["AddLocations"] = [this](sol::object /*self*/, std::string relPath) {
    // Some packs call Tracker:AddLocations("path/to/file.json") at init time
    // instead of (or in addition to) having a locations/ directory.
    std::unordered_map<std::string, int> ruleToIdx;
    // Re-use the existing rule index if locations were already loaded.
    for (size_t i = 0; i < uniqueRules_.size(); ++i)
      ruleToIdx[uniqueRules_[i]] = (int)i;

    fs::path fullPath = currentPackPath_ / relPath;
    if (!fs::exists(fullPath))
      return;
    try {
      std::string canonical = fs::canonical(fullPath).string();
      loaded_location_files_.insert(canonical);
      std::ifstream f(fullPath);
      json j = json::parse(f, nullptr, true, true);
      if (j.is_array()) {
        for (const auto &node : j)
          ProcessLocationNode(node, {}, "", ruleToIdx);
      } else {
        ProcessLocationNode(j, {}, "", ruleToIdx);
      }
    } catch (const std::exception &e) {
      std::cerr << "LogicManager [ERROR]: AddLocations " << relPath << ": "
                << e.what() << std::endl;
    }
  };
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
  scriptHost["AddOnFrameHandler"] = [](sol::variadic_args) {};
  scriptHost["RemoveOnFrameHandler"] = [](sol::variadic_args) {};
  scriptHost["AddOnLocationSectionChangedHandler"] = [](sol::variadic_args) {};
  scriptHost["CreateLuaItem"] = [this](sol::object self) {
    sol::table item = lua_.create_table();
    // Property bag backed by a plain Lua table (no undo support needed)
    sol::table props = lua_.create_table();
    item["_props"] = props;
    item["Set"] = [](sol::object tbl_self, std::string key,
                     sol::object val) -> bool {
      sol::table t = tbl_self.as<sol::table>();
      sol::table p = t["_props"];
      p[key] = val;
      // Fire PropertyChangedFunc if present
      sol::object pcf = t["PropertyChangedFunc"];
      if (pcf.is<sol::function>())
        pcf.as<sol::function>()(tbl_self, key, val);
      return true;
    };
    item["Get"] = [](sol::object tbl_self, std::string key) -> sol::object {
      sol::table t = tbl_self.as<sol::table>();
      sol::table p = t["_props"];
      return p[key];
    };
    item["SetOverlay"] = [](sol::variadic_args) {};
    item["SetOverlayFontSize"] = [](sol::variadic_args) {};
    item["SetOverlayAlign"] = [](sol::variadic_args) {};
    item["SetOverlayBackground"] = [](sol::variadic_args) {};
    item["SetOverlayColor"] = [](sol::variadic_args) {};
    luaItems_.push_back(item);
    return item;
  };
  lua_["ScriptHost"] = scriptHost;

  // Stub for hardware/SNES autotracking — we never connect to an emulator.
  // GetConnectionState returning 0 (disconnected) causes packs to skip
  // hardware-dependent code paths gracefully.
  auto autoTracker = lua_.create_table();
  autoTracker["GetConnectionState"] = [](sol::variadic_args) { return 0; };
  autoTracker["ReadU8"] = [](sol::variadic_args) { return 0; };
  lua_["AutoTracker"] = autoTracker;

  // Stub for ImageReference used by packs that create custom item icons
  sol::table imageRef = lua_.create_table();
  imageRef["FromPackRelativePath"] = [](sol::variadic_args) {
    return sol::lua_nil;
  };
  imageRef["FromImageReference"] = [](sol::variadic_args) {
    return sol::lua_nil;
  };
  lua_["ImageReference"] = imageRef;

  auto accessibility =
      lua_.create_table_with("None", 0, "Partial", 1, "Inspect", 3,
                             "SequenceBreak", 5, "Normal", 6, "Cleared", 7);
  lua_["Accessibility"] = accessibility;
  lua_["AccessibilityLevel"] = accessibility;

  auto highlight =
      lua_.create_table_with("Unspecified", 0, "NoPriority", 10, "Avoid", 20,
                             "Priority", 30, "None", 40);
  lua_["Highlight"] = highlight;

  lua_["PopVersion"] = "0.18.0";


  lua_.safe_script(R"LUA(
      -- PopTracker built-in: has(code [, amount]) checks ProviderCountForCode.
      -- Packs expect this to exist globally without defining it themselves.
      function has(code, amount)
          local count = Tracker:ProviderCountForCode(code)
          if amount ~= nil then
              return count >= tonumber(amount)
          end
          return count > 0
      end

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

      -- Returns a recursive proxy table: unknown reads return another proxy,
      -- #proxy returns 0, writes store into the table normally.
      -- Used so pack scripts that check `if #obj.SomeField > N` don't error
      -- when the field was never explicitly set.
      local __AxoProxyMeta
      __AxoProxyMeta = {
          __index = function(t, k)
              local v = rawget(t, k)
              if v ~= nil then return v end
              local p = setmetatable({}, __AxoProxyMeta)
              rawset(t, k, p)
              return p
          end,
          __len = function() return 0 end,
          __newindex = function(t, k, v) rawset(t, k, v) end,
      }
      function __AxoProxy()
          return setmetatable({}, __AxoProxyMeta)
      end

      -- @Path/To/Section rule reference: AccessibilityLevel now returns
      -- PopTracker-scale (0/5/6), so pass it through directly.
      function __AxoPath(path)
          local obj = Tracker:FindObjectForCode(path)
          if obj == nil then return 0 end
          return obj.AccessibilityLevel
      end

      __AxoSeenErrors = {}
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
                      -- Print each unique rule error only once to avoid spam
                      local msg = tostring(res)
                      if not __AxoSeenErrors[msg] then
                          __AxoSeenErrors[msg] = true
                          print("LogicManager [LUA ERROR]: " .. msg)
                      end
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
  // Stubs for PopTracker data-storage API — not used for logic tracking
  archipelago["AddSetReplyHandler"] = [](sol::variadic_args) {};
  archipelago["AddRetrievedHandler"] = [](sol::variadic_args) {};
  archipelago["AddBouncedHandler"] = [](sol::variadic_args) {};
  archipelago["SetNotify"] = [](sol::variadic_args) {};
  archipelago["Get"] = [](sol::variadic_args) {};
  archipelago["Set"] = [](sol::variadic_args) {};
  archipelago["GetSlotData"] = [this]() {
    return JsonToLua(lua_, lastSlotData_);
  };
  archipelago["GetPlayerAlias"] = [](sol::variadic_args) -> std::string {
    return "";
  };
  archipelago["CheckedLocations"] = lua_.create_table();
  archipelago["MissingLocations"] = lua_.create_table();
  archipelago["PlayerNumber"] = -1;
  archipelago["TeamNumber"] = 0;
  lua_["Archipelago"] = archipelago;
}

std::shared_ptr<TrackerObject>
LogicManager::GetTrackerObject(const std::string &code) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (trackerObjects_.count(code)) {
    return trackerObjects_[code];
  }

  // PopTracker uses case-insensitive item code matching. Fall back to a
  // case-insensitive search through registered objects (excluding internal
  // __id_* keys) before creating a new entry.
  if (!code.empty() && code[0] != '@' && code.rfind("__", 0) != 0) {
    std::string codeLower = code;
    std::transform(codeLower.begin(), codeLower.end(), codeLower.begin(), ::tolower);
    for (auto &kv : trackerObjects_) {
      if (kv.first.rfind("__", 0) == 0) continue;
      std::string kLower = kv.first;
      std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
      if (kLower == codeLower) {
        trackerObjects_[code] = kv.second; // cache alias
        return kv.second;
      }
    }
  }

  // PopTracker uses "@Area/Section" (slash) for path lookups; we store
  // TrackerObjects keyed by breadcrumb "@Area > Section" (space-gt-space).
  // Normalize so Lua pack scripts can find our entries by either format.
  if (code.size() > 1 && code[0] == '@' && code.find('/') != std::string::npos) {
    std::string normalized = "@";
    for (size_t i = 1; i < code.size(); ++i) {
      if (code[i] == '/')
        normalized += " > ";
      else
        normalized += code[i];
    }
    auto it2 = trackerObjects_.find(normalized);
    if (it2 != trackerObjects_.end()) {
      trackerObjects_[code] = it2->second; // cache alias
      return it2->second;
    }
  }

  auto obj = std::make_shared<TrackerObject>();
  obj->code = code;
  obj->on_change = [this](std::string c) {
    accessibility_stale_ = true; // mark accessibility cache stale
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
  // Store first so that re-entrant FindObjectForCode calls (e.g. from a watch
  // fired by set_active below) find the existing object rather than creating
  // a second copy, which would cause infinite recursion.
  trackerObjects_[code] = obj;

  if (itemDefaults_.count(code)) {
    auto def = itemDefaults_[code];
    obj->set_active(def.active);
    obj->set_count(def.count);
  }

  return obj;
}

std::string LogicManager::TranspileRule(const std::string &rule) {
  if (rule.empty())
    return "";
  if (ruleCache_.count(rule))
    return ruleCache_[rule];
  // Strip {…} "check-but-not-collect" rule segments entirely.
  // These mark sections as blue (checkable but not collectible) in PopTracker
  // and have no bearing on our accessibility logic.
  std::string res = rule;
  res = std::regex_replace(res, std::regex(R"(\{[^}]*\})"), "");
  // Clean up any orphaned commas or whitespace left by stripping
  res = std::regex_replace(res, std::regex(R"(,\s*,+)"), ",");
  res = std::regex_replace(res, std::regex(R"(^\s*,+\s*|,+\s*$)"), "");
  res = std::regex_replace(res, std::regex(R"(^\s+|\s+$)"), "");
  if (res.empty()) {
    ruleCache_[rule] = "6";
    return "6";
  }

  res = std::regex_replace(res, std::regex(R"(,)"), " & ");
  // #item_code → has("item_code") boolean check (PopTracker "count" syntax)
  res =
      std::regex_replace(res, std::regex(R"(#([a-zA-Z0-9_]+))"), "has(\"$1\")");
  // Strip brackets from [$func|arg] patterns before $func expansion.
  // PopTracker uses [$stars|1] as a shorthand for $stars|1 (numerical call).
  // After $func expansion the brackets would produce invalid Lua like
  // [__AxoB(stars("1"))], so remove them first.
  res = std::regex_replace(
      res, std::regex(R"(\[(\^?\$[a-zA-Z0-9_]+(?:\|[a-zA-Z0-9_/]+)*)\])"),
      "$1");

  // Args may contain spaces, @, hyphens, apostrophes (e.g. "AT - Aga1",
  // "@Palace of Darkness/Boss/Boss Item"). Stop at |, &, or ( ) so that
  // rule-grouping parentheses are never consumed into an argument.
  // Each arg is trimmed of whitespace after splitting.
  std::regex funcPattern(R"((\^?)\$([a-zA-Z0-9_]+)((?:\|[^|&()]+)*))");
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
      while (std::getline(ss, s, '|')) {
        // Trim whitespace from each arg
        auto a = s.find_first_not_of(" \t");
        auto b = s.find_last_not_of(" \t");
        if (a != std::string::npos)
          args.push_back(s.substr(a, b - a + 1));
      }
    }
    if (args.empty()) {
      finalRes += isNumerical ? func + "()" : "__AxoB(" + func + "())";
    } else {
      // If any arg starts with '@', it's a full location path — pass all args
      // as quoted strings without splitting on '/'.
      bool hasAtPath = !args.empty() && !args[0].empty() && args[0][0] == '@';
      bool hasSlash = !hasAtPath && args[0].find('/') != std::string::npos;
      if (hasSlash && args.size() == 1) {
        // $func|a/b/c — treat slash-separated parts as OR alternatives
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
  // Convert PopTracker [item:N] count syntax → has("item", N)
  // and [item] presence syntax → has("item")
  // Must happen before processInfix so the resulting has() calls are
  // treated as normal leaf expressions rather than raw identifiers.
  finalRes = std::regex_replace(finalRes,
                                std::regex(R"(\[([a-zA-Z0-9_]+):([0-9]+)\])"),
                                R"(has("$1", $2))");
  finalRes = std::regex_replace(finalRes, std::regex(R"(\[([a-zA-Z0-9_]+)\])"),
                                R"(has("$1"))");
  // Unbracketed item:N (e.g. "StarPiece:1") not inside has() already.
  finalRes = std::regex_replace(
      finalRes, std::regex(R"(\b([a-zA-Z_][a-zA-Z0-9_]*):([0-9]+)\b)"),
      R"(has("$1", $2))");

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
    // Leaf: bare identifier without $ → PopTracker item code, call has("code").
    // $-prefixed names were already converted to func() calls above.
    // Lua keywords and numeric literals pass through as-is.
    static const std::regex bare_ident(R"(^[a-zA-Z_][a-zA-Z0-9_]*$)");
    static const std::unordered_set<std::string> lua_kw = {
        "true", "false",  "nil",    "and",   "or",       "not",    "if",
        "then", "else",   "elseif", "end",   "do",       "while",  "for",
        "in",   "return", "break",  "local", "function", "repeat", "until"};
    if (std::regex_match(s, bare_ident) && !lua_kw.count(s))
      return "__AxoB(has(\"" + s + "\"))";
    // @Path/To/Section → section accessibility lookup
    if (!s.empty() && s[0] == '@')
      return "__AxoPath(\"" + s + "\")";
    return s;
  };
  finalRes = processInfix(finalRes);
  ruleCache_[rule] = finalRes;
  return finalRes;
}

void LogicManager::LoadLocationsFromPack(
    const std::filesystem::path &packPath) {
  // State was already cleared in LoadPack() before the entry script ran.
  // This function only adds locations from the pack's locations/ directory
  // (if present); packs that use Tracker:AddLocations() from their script
  // will have already populated allLocations_ before we get here.
  std::unordered_map<std::string, int> ruleToIdx;
  for (size_t i = 0; i < uniqueRules_.size(); ++i)
    ruleToIdx[uniqueRules_[i]] = (int)i;
  fs::path locDir = packPath / "locations";
  // Items already loaded before script ran in LoadPack().
  // Scripts are loaded by LoadPack() before this function is called.
  if (fs::exists(locDir)) {
    int fileCount = 0;
    for (const auto &entry : fs::recursive_directory_iterator(locDir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        fileCount++;
        try {
          // Skip files already loaded via Tracker:AddLocations() to avoid
          // duplicating location nodes (which breaks ID propagation).
          std::string canonical = fs::canonical(entry.path()).string();
          if (loaded_location_files_.count(canonical))
            continue;
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

  // Nodes with a "ref" field are view-only aliases of canonical entries defined
  // elsewhere in the pack (e.g. totals_screen.json references blue_coins.json
  // and locations.json). The canonical entries already carry the correct access
  // rules and IDs, so we skip ref nodes entirely to avoid duplicates with
  // missing/wrong rule inheritance.
  if (node.contains("ref"))
    return;

  // Skip subtrees that contain no direct location content — only ref aliases.
  // This avoids phantom region entries from secondary display files (e.g.
  // totals_screen.json container nodes whose sections are all refs).
  // We check recursively so that container nodes with ref-only children are
  // also pruned. This is pack-agnostic: any node whose entire subtree is
  // ref-only carries no rules or IDs of its own.
  std::function<bool(const json &)> hasDirectContent =
      [&](const json &n) -> bool {
    if (!n.is_object())
      return false;
    if (n.contains("ref"))
      return false;
    if (n.contains("item_count") || n.contains("hosted_item"))
      return true;
    // Leaf section node: has a name but no sub-sections or children.
    // This is the common DK64/standard PopTracker format where sections are
    // just {"name": "Location Name"} entries with no further nesting.
    bool hasSub = (n.contains("sections") && n["sections"].is_array() &&
                   !n["sections"].empty()) ||
                  (n.contains("children") && n["children"].is_array() &&
                   !n["children"].empty());
    if (!hasSub && n.contains("name") && n["name"].is_string() &&
        !n["name"].get<std::string>().empty())
      return true;
    for (const char *key : {"sections", "children"}) {
      if (n.contains(key) && n[key].is_array()) {
        for (const auto &child : n[key])
          if (hasDirectContent(child))
            return true;
      }
    }
    return false;
  };
  if (!hasDirectContent(node))
    return;

  std::string nodeName = node.value("name", "");
  std::vector<std::string> fullPath = parentPath;
  if (!nodeName.empty())
    fullPath.push_back(nodeName);

  // Build the flat breadcrumb string from path segments (for logging/compat)
  std::string name;
  for (size_t i = 0; i < fullPath.size(); ++i) {
    if (i > 0)
      name += " > ";
    name += fullPath[i];
  }

  // Parse Access Rules
  std::string nodeRule = "";
  auto trimStr = [](const std::string &in) -> std::string {
    auto s = in.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    auto e = in.find_last_not_of(" \t\r\n");
    return in.substr(s, e - s + 1);
  };
  if (node.contains("access_rules")) {
    const auto &ar = node["access_rules"];
    if (ar.is_string()) {
      // Plain string — single rule (no OR alternatives)
      std::string rStr = trimStr(ar.get<std::string>());
      if (!rStr.empty())
        nodeRule = "(" + rStr + ")";
    } else if (ar.is_array() && !ar.empty()) {
      std::vector<std::string> parts;
      for (const auto &r : ar) {
        if (r.is_string()) {
          // Flat string element — one OR alternative
          std::string rStr = trimStr(r.get<std::string>());
          if (!rStr.empty())
            parts.push_back("(" + rStr + ")");
        } else if (r.is_array()) {
          // Inner array — elements are AND'd together (comma-joined)
          std::string andExpr;
          for (const auto &inner : r) {
            if (!inner.is_string()) continue;
            std::string rStr = trimStr(inner.get<std::string>());
            if (rStr.empty()) continue;
            if (!andExpr.empty()) andExpr += ", ";
            andExpr += rStr;
          }
          if (!andExpr.empty())
            parts.push_back("(" + andExpr + ")");
        }
      }
      if (!parts.empty()) {
        nodeRule = parts[0];
        for (size_t i = 1; i < parts.size(); ++i)
          nodeRule += " | " + parts[i];
      }
    }
  }

  // Parse Visibility Rules (Critical for SM64 ER)
  std::string visibilityRule = "";
  if (node.contains("visibility_rules")) {
    if (node["visibility_rules"].is_string()) {
      std::string vr = node["visibility_rules"].get<std::string>();
      if (vr.find_first_not_of(" \t\r\n") != std::string::npos)
        visibilityRule = vr;
    } else if (node["visibility_rules"].is_array() &&
               !node["visibility_rules"].empty()) {
      std::vector<std::string> parts;
      for (const auto &r : node["visibility_rules"]) {
        if (!r.is_string())
          continue;
        std::string rStr = r.get<std::string>();
        auto s = rStr.find_first_not_of(" \t\r\n");
        if (s == std::string::npos)
          continue; // blank, skip
        parts.push_back("(" + rStr + ")");
      }
      if (!parts.empty()) {
        visibilityRule = parts[0];
        for (size_t i = 1; i < parts.size(); ++i)
          visibilityRule += " | " + parts[i];
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

  // Extract refLeaf: the second-to-last segment of a PopTracker "ref" path.
  // e.g. ref="Bianco Hills Blue Coins/River End/Blue Coin" → refLeaf="River
  // End" This provides a fallback display-name → AP-name match during ID
  // resolution.
  std::string refLeaf;
  if (node.contains("ref") && node["ref"].is_string()) {
    std::string ref = node["ref"].get<std::string>();
    size_t lastSlash = ref.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      size_t prevSlash = ref.rfind('/', lastSlash - 1);
      if (prevSlash != std::string::npos)
        refLeaf = ref.substr(prevSlash + 1, lastSlash - prevSlash - 1);
      else
        refLeaf = ref.substr(0, lastSlash);
    }
  }

  if (!name.empty()) {
    LocationLogic ll;
    ll.name = name;
    ll.path = fullPath;
    ll.id = id;
    ll.refLeaf = refLeaf;
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
