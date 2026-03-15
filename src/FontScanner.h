#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct AxolotlFontInfo {
  std::string name;
  std::string path;
  bool is_valid = true;
};

class FontScanner {
public:
  static std::vector<AxolotlFontInfo> GetAvailableFonts();
  static std::string FindFontPath(const std::string &font_name);
  static bool IsValidFontFile(const std::filesystem::path &path);
};
