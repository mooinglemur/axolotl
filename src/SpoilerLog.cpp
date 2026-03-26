#include "SpoilerLog.h"
#include <algorithm>
#include <fstream>
#include <regex>

SpoilerLog SpoilerLog::Parse(const std::string &path,
                             const std::vector<std::string> &player_names) {
  SpoilerLog log;
  std::ifstream file(path);
  if (!file.is_open())
    return log;

  std::vector<std::string> all_player_names = player_names;
  std::string line;
  std::vector<std::string> lines;

  // First pass: Read all lines and look for players
  while (std::getline(file, line)) {
    lines.push_back(line);

    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    // Parse "1: Player Name (Game Name)" in the Players: section
    // Note: We don't strictly require being in the "Players:" section because
    // this format is very specific to the player list at the top.
    // Parse "1: Player Name (Game Name)" or similar formats
    std::regex player_regex("^\\d+:?\\s*(.+?)\\s*\\(.+\\)$");
    std::smatch match;
    if (std::regex_match(trimmed, match, player_regex)) {
      all_player_names.push_back(match[1].str());
    }
  }

  // Deduplicate
  std::sort(all_player_names.begin(), all_player_names.end());
  all_player_names.erase(
      std::unique(all_player_names.begin(), all_player_names.end()),
      all_player_names.end());

  bool in_playthrough = false;
  Sphere current_sphere;
  current_sphere.number = -1;

  for (std::string &raw_line : lines) {
    line = raw_line;
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    line.erase(line.find_last_not_of(" \t\r\n") + 1);

    if (line.find("Playthrough:") == 0) {
      in_playthrough = true;
      continue;
    }

    if (!in_playthrough)
      continue;

    // Check for sphere start: "0: {" or "1: {" or "Sphere 0: {"
    std::regex sphere_regex("^(\\d+):\\s*\\{$");
    std::regex sphere_regex_alt("^Sphere\\s+(\\d+):\\s*\\{$");
    std::smatch match;
    if (std::regex_match(line, match, sphere_regex) ||
        std::regex_match(line, match, sphere_regex_alt)) {
      if (current_sphere.number != -1) {
        log.spheres_.push_back(current_sphere);
      }
      current_sphere = Sphere();
      current_sphere.number = std::stoi(match[1].str());
      continue;
    }

    if (line == "}") {
      // End of spheres usually happens when we hit an empty line after the last
      // } or another section. For now, we just keep adding.
      if (current_sphere.number != -1) {
        log.spheres_.push_back(current_sphere);
        current_sphere.number = -1;
      }
      continue;
    }

    if (current_sphere.number == 0) {
      bool matched = false;
      for (const auto &player : all_player_names) {
        std::string suffix = " (" + player + ")";
        if (line.size() >= suffix.size() &&
            line.substr(line.size() - suffix.size()) == suffix) {
          SpoilerItem item;
          item.name = line.substr(0, line.size() - suffix.size());
          item.player = player;
          current_sphere.items.push_back(item);
          matched = true;
          break;
        }
      }
      if (!matched && !line.empty() && line != "{") {
        // If we didn't match Sphere 0 items, and we have only one player,
        // assume it's them
        if (all_player_names.size() == 1) {
          SpoilerItem item;
          item.name = line;
          item.player = all_player_names[0];
          current_sphere.items.push_back(item);
        }
      }
    } else if (current_sphere.number > 0) {
      bool matched = false;
      for (const auto &player : all_player_names) {
        std::string mid = " (" + player + "): ";
        size_t pos = line.find(mid);
        if (pos != std::string::npos) {
          std::string rest = line.substr(pos + mid.size());
          for (const auto &item_player : all_player_names) {
            std::string item_suffix = " (" + item_player + ")";
            if (rest.size() >= item_suffix.size() &&
                rest.substr(rest.size() - item_suffix.size()) == item_suffix) {
              SpoilerLocation loc;
              loc.name = line.substr(0, pos);
              loc.player = player;
              loc.item.name = rest.substr(0, rest.size() - item_suffix.size());
              loc.item.player = item_player;
              current_sphere.locations.push_back(loc);
              matched = true;
              goto next_line;
            }
          }
        }
      }
      if (!matched && all_player_names.size() == 1 && !line.empty() &&
          line != "{") {
        size_t pos = line.find(": ");
        if (pos != std::string::npos) {
          SpoilerLocation loc;
          loc.name = line.substr(0, pos);
          loc.player = all_player_names[0];
          loc.item.name = line.substr(pos + 2);
          loc.item.player = all_player_names[0];
          current_sphere.locations.push_back(loc);
          matched = true;
        }
      }
      if (!matched && !line.empty() && line != "{") {
        // No debug print
      }
    }
  next_line:;
  }

  if (current_sphere.number != -1) {
    log.spheres_.push_back(current_sphere);
  }

  return log;
}
