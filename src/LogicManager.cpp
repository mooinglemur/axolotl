#include "LogicManager.h"
#include "PackStore.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
  if (j.is_number())
    return sol::make_object(lua, j.get<double>());
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

void LogicManager::SetDebugMode(bool debug) {
  debug_mode_ = debug;
  lua_["AUTOTRACKER_ENABLE_DEBUG_LOGGING_AP"] = debug;
}

bool LogicManager::LoadPack(const std::string &game) {
  if (debug_mode_)
    std::cerr << "LogicManager: Starting load for game: " << game << std::endl;
  fs::path packPath = PackStore::GetPackPath(game);
  if (!fs::exists(packPath / "manifest.json"))
    return false;

  currentGame_ = game;
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

    LoadLocationsFromPack(packPath);
    LoadItemsFromPack(packPath);
    clearHandlers_.clear();
    trackerObjects_.clear();

    // Pre-build unique rules table in Lua
    lua_.safe_script("_rules = {}");
    sol::table rulesTable = lua_["_rules"];
    for (size_t i = 0; i < uniqueRules_.size(); ++i) {
      std::string funcStr =
          "return function() return " + uniqueRules_[i] + " end";
      sol::load_result lr = lua_.load(funcStr);
      if (lr.valid()) {
        sol::protected_function pf = lr.get<sol::protected_function>();
        sol::protected_function_result res = pf();
        if (res.valid()) {
          rulesTable[i + 1] = res.get<sol::function>();
        }
      }
    }

    if (debug_mode_)
      std::cerr << "LogicManager: Loaded " << allLocations_.size()
                << " nodes and " << uniqueRules_.size() << " unique rules."
                << std::endl;

    firstRun_ = true;
    lastItemNameCounts_.clear();

    lua_.script_file((packPath / entry).string());
    return true;
  } catch (const std::exception &e) {
    if (debug_mode_)
      std::cerr << "LogicManager Load Error: " << e.what() << std::endl;
    return false;
  }
}

void LogicManager::UpdateLogic(const std::map<int64_t, int> &itemCounts,
                               const nlohmann::json &slotData,
                               const std::set<int64_t> &checkedLocationIds,
                               const std::set<int64_t> &missingLocationIds,
                               int playerNumber) {
  if (currentGame_.empty())
    return;

  if (!firstRun_ && slotData == lastSlotData_ &&
      itemCounts == lastItemCounts_ &&
      checkedLocationIds == lastCheckedLocationIds_ &&
      missingLocationIds == lastMissingLocationIds_ &&
      playerNumber == lastPlayerNumber_) {
    return;
  }

  // Sync Archipelago global state to Lua
  sol::table archipelago = lua_["Archipelago"];
  archipelago["PlayerNumber"] = playerNumber;

  sol::table checkedTable = lua_.create_table();
  int i = 1;
  for (int64_t id : checkedLocationIds) {
    checkedTable[i++] = id;
  }
  archipelago["CheckedLocations"] = checkedTable;

  sol::table missingTable = lua_.create_table();
  i = 1;
  for (int64_t id : missingLocationIds) {
    missingTable[i++] = id;
  }
  archipelago["MissingLocations"] = missingTable;

  // Update slot data in Lua
  if (slotData != lastSlotData_) {
    if (debug_mode_) {
      std::cout << "LogicManager [DEBUG]: New Slot Data received: "
                << slotData.dump(2) << std::endl;
    }
    lastSlotData_ = slotData;
    lua_["SLOT_DATA"] = JsonToLua(lua_, slotData);
    for (auto &cb : clearHandlers_) {
      cb(lua_["SLOT_DATA"]);
    }
  }
  lastItemCounts_ = itemCounts;
  lastCheckedLocationIds_ = checkedLocationIds;
  lastMissingLocationIds_ = missingLocationIds;
  lastPlayerNumber_ = playerNumber;

  // Sanitization and tracking helper
  lua_.safe_script(R"LUA(
      if SLOT_DATA and SLOT_DATA.AreaRando then
          for k, v in pairs(SLOT_DATA.AreaRando) do
              if type(v) == "number" then SLOT_DATA.AreaRando[k] = string.format("%d", v) end
          end
      end
      -- Restore critical autotracking settings
      local s = Tracker:FindObjectForCode("__setting_auto_ent")
      if s then s.CurrentStage = 1 end
      local r = Tracker:FindObjectForCode("__setting_spoil_reqs")
      if r then r.CurrentStage = 1 end

      -- Safe Slot Data sync loop for various pack settings
      if SLOT_DATA then
          for k, v in pairs(SLOT_DATA) do
              local code = "__setting_" .. k
              local obj = Tracker:FindObjectForCode(code)
              if obj then
                  if type(v) == "number" then obj.CurrentStage = v
                  elseif type(v) == "boolean" then obj.Active = v end
              end
          end
      end

      function SyncArchipelagoItems(id_counts)
          if not ITEM_MAPPING then return end
          local key_bits = 0
          for id, count in pairs(id_counts) do
              local num_id = tonumber(id)
              local v = ITEM_MAPPING[num_id]
              if v then
                  local obj = Tracker:FindObjectForCode(v[1])
                  if obj then
                      if v[1] == "item__key" then
                          -- Aggregate key bits (1=Basement, 2=Upstairs)
                          if v[2] == 0 then -- Progressive
                              if count >= 1 then key_bits = key_bits | 1 end
                              if count >= 2 then key_bits = key_bits | 2 end
                          else -- Distinct bits
                              if count > 0 then key_bits = key_bits | v[2] end
                          end
                      elseif v[2] == "toggle" then
                          obj.Active = count > 0
                          obj.CurrentStage = count
                          obj.AcquiredCount = count
                      elseif v[2] == "progressive" or v[2] == "consumable" then
                          obj.Active = count > 0
                          obj.CurrentStage = count
                          obj.AcquiredCount = count
                      else
                          obj.Active = count > 0
                          obj.CurrentStage = count
                          obj.AcquiredCount = count
                      end
                      if AUTOTRACKER_ENABLE_DEBUG_LOGGING_AP then
                          print(string.format("LogicManager [DEBUG]: Syncing item %s (id %s), count %d, result=%s", 
                                              v[1], tostring(num_id), count, tostring(obj.Active or obj.CurrentStage)))
                      end
                  end
              end
          end
          -- Apply aggregated key bits
          local k = Tracker:FindObjectForCode("item__key")
          if k then 
              k.CurrentStage = key_bits 
              if AUTOTRACKER_ENABLE_DEBUG_LOGGING_AP then
                  print(string.format("LogicManager [DEBUG]: Final key_bits = %d", key_bits))
              end
          end
      end
  )LUA");

  // Sync items
  sol::table idCounts = lua_.create_table();
  bool starCountChanged = false;
  for (auto const &[id, count] : itemCounts) {
    idCounts[id] = count;
    if (id == 3626000)
      starCountChanged = true;
  }

  sol::function syncItems = lua_["SyncArchipelagoItems"];
  if (syncItems.valid()) {
    syncItems(idCounts);
  }

  firstRun_ = false;

  // Define rule evaluation helper
  auto evaluateRules = [this]() {
    lua_.safe_script(R"LUA(
      _results = {}
      for i = 1, _num_rules do
          local rule = _rules[i]
          if rule then
              local ok, res = pcall(rule)
              if ok then
                  if type(res) == "boolean" then _results[i] = res and 2 or 0
                  elseif type(res) == "number" then _results[i] = res
                  else _results[i] = 0 end
              else _results[i] = 0 end
          else _results[i] = 0 end
      end
    )LUA");
  };

  lua_["_num_rules"] = uniqueRules_.size();
  auto tracker = lua_["Tracker"];

  // Pass 1: Discovery (Set all to 2 to trigger reveal)
  evaluateRules();
  sol::table results1 = lua_["_results"];
  for (auto &loc : allLocations_) {
    int access = 2; // Default to Full for discovery/reveal in pass 1
    if (loc.ruleIndex != -1) {
      sol::object res = results1[loc.ruleIndex + 1];
      if (res.is<int>()) {
        access = res.as<int>();
      }
    }
    sol::table obj = tracker["FindObjectForCode"](tracker, "@" + loc.name);
    obj["AccessibilityLevel"] = access;
  }

  // Trigger areaReveal (now that we have discovery accessibility)
  sol::protected_function areaReveal = lua_["areaReveal"];
  if (areaReveal.valid()) {
    areaReveal();
  }

  // Pass 2: Strict check (After discovery)
  evaluateRules(); // Re-evaluate now that areas are discovered
  sol::table results2 = lua_["_results"];
  std::map<int64_t, int> maxAccess;
  std::map<int64_t, std::string> bestName;

  for (auto &loc : allLocations_) {
    int access = 0;
    if (loc.ruleIndex != -1) {
      sol::object res = results2[loc.ruleIndex + 1];
      if (res.is<int>()) {
        access = res.as<int>();
      }
    } else {
      access = 2;
    }

    // Sync back to Lua for final representation
    sol::table obj = tracker["FindObjectForCode"](tracker, "@" + loc.name);
    obj["AccessibilityLevel"] = access;

    if (loc.id != 0 && access > maxAccess[loc.id]) {
      maxAccess[loc.id] = access;
      bestName[loc.id] = loc.name;
    }
  }

  // 3. Populate final results filtered by active session IDs
  locations_.clear();
  accessibilityCache_.clear();
  for (const auto &[id, access] : maxAccess) {
    if (missingLocationIds.count(id)) {
      if (access > 0) {
        locations_.push_back({bestName[id], id, "", "", -1, access});
      }
      accessibilityCache_[id] = access;
    }
  }
}

int LogicManager::GetAccessibility(int64_t locationId) const {
  auto it = accessibilityCache_.find(locationId);
  return (it != accessibilityCache_.end()) ? it->second : 0;
}

void LogicManager::BindGlobals() {
  lua_.set_function("print", [this](sol::variadic_args args) {
    if (!debug_mode_)
      return;
    for (auto arg : args) {
      std::cout << arg.as<std::string>() << "\t";
    }
    std::cout << std::endl;
  });

  auto tracker = lua_.create_table();
  tracker["FindObjectForCode"] = [this](sol::object self, std::string code) {
    auto it = trackerObjects_.find(code);
    if (it != trackerObjects_.end())
      return it->second;
    sol::table t = lua_.create_table_with("CurrentStage", 0, "Active", false,
                                          "AccessibilityLevel", 0);
    trackerObjects_[code] = t;
    return t;
  };
  tracker["ProviderCountForCode"] = [this](sol::object self, std::string code) {
    auto it = trackerObjects_.find(code);
    return it != trackerObjects_.end() ? it->second["CurrentStage"].get<int>()
                                       : 0;
  };

  // Add stubs for common loading functions
  tracker["AddMaps"] = [](sol::variadic_args) {};
  tracker["AddItems"] = [](sol::variadic_args) {};
  tracker["AddLocations"] = [](sol::variadic_args) {};
  tracker["AddLayouts"] = [](sol::variadic_args) {};
  lua_["Tracker"] = tracker;

  auto accessibility =
      lua_.create_table_with("None", 0, "Partial", 1, "Full", 2);
  lua_["Accessibility"] = accessibility;
  lua_["AccessibilityLevel"] = accessibility;
  lua_["PopVersion"] = "0.19.0";

  auto scriptHost = lua_.create_table();
  scriptHost["AddMemoryWatch"] = [](sol::variadic_args) {};
  scriptHost["RegisterTimer"] = [](sol::variadic_args) {};
  scriptHost["AddWatchForCode"] = [](sol::variadic_args) {};
  scriptHost["RemoveWatchForCode"] = [](sol::variadic_args) {};
  scriptHost["LoadScript"] = [this](sol::object self, std::string path) {
    fs::path p = PackStore::GetPackPath(this->currentGame_) / path;
    try {
      this->lua_.script_file(p.string());
    } catch (...) {
    }
  };
  lua_["ScriptHost"] = scriptHost;

  auto archipelago = lua_.create_table();
  archipelago["AddClearHandler"] = [this](sol::object self, std::string name,
                                          sol::function cb) {
    clearHandlers_.push_back(cb);
  };
  archipelago["AddItemHandler"] = [](sol::variadic_args) {};
  archipelago["AddLocationHandler"] = [](sol::variadic_args) {};
  archipelago["GetSlotData"] = [this]() { return lua_["SLOT_DATA"]; };
  archipelago["CheckedLocations"] = lua_.create_table();
  archipelago["MissingLocations"] = lua_.create_table();
  archipelago["PlayerNumber"] = -1;
  lua_["Archipelago"] = archipelago;
}

std::string LogicManager::TranspileRule(const std::string &rule) {
  if (rule.empty())
    return "";
  if (ruleCache_.count(rule))
    return ruleCache_[rule];

  std::string res = std::regex_replace(rule, std::regex(R"([\{\}])"), "");
  res = std::regex_replace(res, std::regex(","), " and ");
  res = std::regex_replace(res, std::regex(R"(\^)"), " or ");

  std::regex funcPattern(R"(\$([a-zA-Z0-9_]+)((?:\|[a-zA-Z0-9_/]+)*))");
  auto begin = std::sregex_iterator(res.begin(), res.end(), funcPattern);
  auto end = std::sregex_iterator();
  std::string finalRes = "";
  size_t lastPos = 0;
  for (auto i = begin; i != end; ++i) {
    std::smatch m = *i;
    finalRes += res.substr(lastPos, m.position() - lastPos);
    std::string func = m[1], argsStr = m[2];
    std::vector<std::string> args;
    if (!argsStr.empty()) {
      std::stringstream ss(argsStr.substr(1));
      std::string s;
      while (std::getline(ss, s, '|'))
        args.push_back(s);
    }
    if (args.empty())
      finalRes += func + "()";
    else {
      bool hasSlash = args[0].find('/') != std::string::npos;
      if (hasSlash && args.size() == 1) {
        std::string exp = "(";
        std::stringstream ss(args[0]);
        std::string sub;
        bool first = true;
        while (std::getline(ss, sub, '/')) {
          if (!first)
            exp += " or ";
          exp += func + "(\"" + sub + "\")";
          first = false;
        }
        finalRes += exp + ")";
      } else {
        finalRes += func + "(";
        for (size_t j = 0; j < args.size(); ++j)
          finalRes +=
              "\"" + args[j] + "\"" + (j == args.size() - 1 ? "" : ", ");
        finalRes += ")";
      }
    }
    lastPos = m.position() + m.length();
  }
  finalRes += res.substr(lastPos);
  finalRes = std::regex_replace(finalRes, std::regex(R"(\(\s*and\s+)"), "(");
  finalRes = std::regex_replace(finalRes, std::regex(R"(\s+and\s*\))"), ")");
  finalRes =
      std::regex_replace(finalRes, std::regex(R"(^\s*and\s+|\s+and\s*$)"), "");

  ruleCache_[rule] = finalRes;
  return finalRes;
}

void LogicManager::LoadLocationsFromPack(
    const std::filesystem::path &packPath) {
  allLocations_.clear();
  uniqueRules_.clear();
  ruleCache_.clear();
  std::unordered_map<std::string, int> ruleToIdx;
  fs::path locDir = packPath / "locations";
  auto parseRuleSet = [](const json &j) -> std::string {
    if (j.is_string())
      return j.get<std::string>();
    if (j.is_array()) {
      std::string c = "";
      for (const auto &r : j) {
        std::string s =
            r.is_string()
                ? r.get<std::string>()
                : (r.is_array() && !r.empty() ? r[0].get<std::string>() : "");
        if (!s.empty()) {
          if (!c.empty())
            c += "^";
          c += "(" + s + ")";
        }
      }
      return c;
    }
    return "";
  };
  auto combine = [](const std::string &r1, const std::string &r2) {
    if (r1.empty())
      return r2;
    if (r2.empty())
      return r1;
    return "(" + r1 + ") and (" + r2 + ")";
  };

  std::function<void(const json &, const std::string &, const std::string &)>
      processNode;
  processNode = [&](const json &node, const std::string &prefix,
                    const std::string &parentRule) {
    std::string name = node.value("name", "");
    if (name.empty())
      return;
    std::string full = prefix.empty() ? name : prefix + " - " + name;
    std::string local = "";
    if (node.contains("visibility_rules"))
      local = combine(local, parseRuleSet(node["visibility_rules"]));
    if (node.contains("access_rules"))
      local = combine(local, parseRuleSet(node["access_rules"]));
    std::string rule = combine(parentRule, local);
    std::string transpiled = TranspileRule(rule);
    int nodeID = 0;
    if (node.contains("hosted_item")) {
      std::string h = node["hosted_item"].get<std::string>();
      if (h.find("__location_item_") == 0)
        try {
          nodeID = std::stoll(h.substr(16));
        } catch (...) {
        }
    }
    int idx = -1;
    if (!transpiled.empty()) {
      if (ruleToIdx.count(transpiled))
        idx = ruleToIdx[transpiled];
      else {
        idx = uniqueRules_.size();
        ruleToIdx[transpiled] = idx;
        uniqueRules_.push_back(transpiled);
      }
    }
    allLocations_.push_back({full, nodeID, rule, transpiled, idx, 0});

    if (node.contains("sections")) {
      for (const auto &sec : node["sections"]) {
        int id = 0;
        if (sec.contains("hosted_item")) {
          std::string h = sec["hosted_item"].get<std::string>();
          if (h.find("__location_item_") == 0)
            try {
              id = std::stoll(h.substr(16));
            } catch (...) {
            }
        }
        std::string sRule = "";
        if (sec.contains("visibility_rules"))
          sRule = combine(sRule, parseRuleSet(sec["visibility_rules"]));
        if (sec.contains("access_rules"))
          sRule = combine(sRule, parseRuleSet(sec["access_rules"]));
        std::string total = combine(rule, sRule);
        std::string sTrans = TranspileRule(total);
        int sIdx = -1;
        if (!sTrans.empty()) {
          if (ruleToIdx.count(sTrans))
            sIdx = ruleToIdx[sTrans];
          else {
            sIdx = uniqueRules_.size();
            ruleToIdx[sTrans] = sIdx;
            uniqueRules_.push_back(sTrans);
          }
        }
        allLocations_.push_back({full + " - " + sec.value("name", "Unknown"),
                                 id, total, sTrans, sIdx, 0});
      }
    }
    if (node.contains("children"))
      for (const auto &c : node["children"])
        processNode(c, full, rule);
  };

  for (const auto &entry : fs::recursive_directory_iterator(locDir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      try {
        std::ifstream f(entry.path());
        json j = json::parse(f, nullptr, true, true);
        if (j.is_array())
          for (const auto &n : j)
            processNode(n, "", "");
      } catch (...) {
      }
    }
  }
}

void LogicManager::LoadItemsFromPack(const std::filesystem::path &packPath) {
  nameToCode_.clear();
  fs::path itemsDir = packPath / "items";
  if (!fs::exists(itemsDir))
    return;
  for (const auto &entry : fs::recursive_directory_iterator(itemsDir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      try {
        std::ifstream f(entry.path());
        json j = json::parse(f, nullptr, true, true);
        auto proc = [&](const json &i) {
          std::string n = i.value("name", ""), c = i.value("code", "");
          if (c.empty() && i.contains("codes")) {
            std::string cs = i["codes"].get<std::string>();
            c = cs.substr(0, cs.find(','));
          }
          if (!n.empty() && !c.empty())
            nameToCode_[n] = c;
        };
        if (j.is_array())
          for (const auto &i : j)
            proc(i);
        else
          proc(j);
      } catch (...) {
      }
    }
  }
}
