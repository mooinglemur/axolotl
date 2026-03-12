#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct FontInfo {
  std::string name;
  std::string path;
  bool is_valid = true;
};

class FontScanner {
public:
  static std::vector<FontInfo> GetAvailableFonts();
  static std::string FindFontPath(const std::string &font_name);
  static bool IsValidFontFile(const std::filesystem::path &path);
};
