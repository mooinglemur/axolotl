#include "FontScanner.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#ifdef __linux__
#include <fontconfig/fontconfig.h>
#endif

std::vector<AxolotlFontInfo> FontScanner::GetAvailableFonts() {
  std::vector<AxolotlFontInfo> fonts;

#ifdef __linux__
  FcConfig *config = FcInitLoadConfigAndFonts();
  FcPattern *pat = FcPatternCreate();
  FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_FILE, (char *)0);
  FcFontSet *fs = FcFontList(config, pat, os);

  for (int i = 0; fs && i < fs->nfont; ++i) {
    FcPattern *font = fs->fonts[i];
    FcChar8 *file, *family;
    if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch) {
      std::string filename = (char *)file;
      std::string ext = std::filesystem::path(filename).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

      if (ext == ".ttf" || ext == ".otf") {
        fonts.push_back({(char *)family, filename});
      }
    }
  }

  if (fs)
    FcFontSetDestroy(fs);
  if (os)
    FcObjectSetDestroy(os);
  if (pat)
    FcPatternDestroy(pat);
  if (config)
    FcFini();
#elif defined(__APPLE__)
  // Basic fallback for macOS
  std::vector<std::string> search_paths = {
      "/Library/Fonts", "/System/Library/Fonts", "~/Library/Fonts"};
  for (const auto &path : search_paths) {
    if (std::filesystem::exists(path)) {
      for (const auto &entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".ttf" ||
            entry.path().extension() == ".otf") {
          fonts.push_back(
              {entry.path().stem().string(), entry.path().string()});
        }
      }
    }
  }
#elif defined(_WIN32)
  // Basic fallback for Windows
  std::filesystem::path win_fonts = "C:\\Windows\\Fonts";
  if (std::filesystem::exists(win_fonts)) {
    for (const auto &entry : std::filesystem::directory_iterator(win_fonts)) {
      if (entry.path().extension() == ".ttf" ||
          entry.path().extension() == ".otf") {
        fonts.push_back({entry.path().stem().string(), entry.path().string()});
      }
    }
  }
#endif

  // Sort fonts by name
  std::sort(fonts.begin(), fonts.end(),
            [](const AxolotlFontInfo &a, const AxolotlFontInfo &b) {
              return a.name < b.name;
            });

  // Remove duplicates (fontconfig can return multiple styles for the same
  // family)
  fonts.erase(
      std::unique(fonts.begin(), fonts.end(),
                  [](const AxolotlFontInfo &a, const AxolotlFontInfo &b) {
                    return a.name == b.name;
                  }),
      fonts.end());

  return fonts;
}

std::string FontScanner::FindFontPath(const std::string &font_name) {
  if (font_name.empty())
    return "";

  // Check if it's already an absolute path
  if (std::filesystem::exists(font_name))
    return font_name;

  auto fonts = GetAvailableFonts();
  for (const auto &font : fonts) {
    if (font.name == font_name) {
      return font.path;
    }
  }
  return "";
}
bool FontScanner::IsValidFontFile(const std::filesystem::path &path) {
  if (!std::filesystem::is_regular_file(path))
    return false;

  // Basic sanity check: fonts are usually > 1KB
  uintmax_t size = std::filesystem::file_size(path);
  if (size < 1024)
    return false;

  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;

  // Read the whole file for deep validation
  // While slightly slow, it's the only way to be 100% sure stb_truetype won't
  // crash later
  std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());

  if (buffer.empty())
    return false;

  stbtt_fontinfo font;
  // stbtt_InitFont returns 1 on success, 0 on failure
  if (stbtt_InitFont(&font, buffer.data(), 0)) {
    return true;
  }

  return false;
}
