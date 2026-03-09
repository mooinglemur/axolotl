#pragma once
#include <string>
#include <vector>

struct FontInfo {
  std::string name;
  std::string path;
};

class FontScanner {
public:
  static std::vector<FontInfo> GetAvailableFonts();
  static std::string FindFontPath(const std::string &font_name);
};
