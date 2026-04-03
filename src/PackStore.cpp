#include "PackStore.h"
#include "Config.h"
#include <iostream>
#include <zip.h>
#include <fstream>
#include <vector>

fs::path PackStore::GetCacheDir() { return Config::GetCacheDir() / "packs"; }

bool PackStore::HasPack(const std::string &game) {
  if (game.empty())
    return false;
  return fs::exists(GetPackPath(game) / "manifest.json");
}

fs::path PackStore::GetPackPath(const std::string &game) {
  return GetCacheDir() / SanitizeFileName(game);
}

bool PackStore::ImportPack(const std::string &game,
                           const fs::path &sourcePath) {
  if (game.empty() || sourcePath.empty())
    return false;

  fs::path target = GetPackPath(game);
  try {
    if (fs::exists(target)) {
      fs::remove_all(target);
    }
    fs::create_directories(target.parent_path());

    if (fs::is_directory(sourcePath)) {
      fs::copy(sourcePath, target, fs::copy_options::recursive);
      return true;
    } else if (sourcePath.extension() == ".zip" ||
               sourcePath.extension() == ".track") {
      int err = 0;
      zip_t *z = zip_open(sourcePath.string().c_str(), 0, &err);
      if (!z) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        std::cerr << "PackStore: Failed to open zip archive " << sourcePath
                  << ": " << zip_error_strerror(&error) << std::endl;
        zip_error_fini(&error);
        return false;
      }

      fs::create_directories(target);

      zip_int64_t num_entries = zip_get_num_entries(z, 0);
      for (zip_int64_t i = 0; i < num_entries; i++) {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(z, i, 0, &st) != 0)
          continue;

        std::string name = st.name;
        // Basic zip-slip protection
        if (name.find("..") != std::string::npos)
          continue;

        fs::path entryPath = target / name;

        if (name.back() == '/') {
          fs::create_directories(entryPath);
        } else {
          fs::create_directories(entryPath.parent_path());
          zip_file_t *zf = zip_fopen_index(z, i, 0);
          if (!zf)
            continue;

          std::ofstream fout(entryPath, std::ios::binary);
          if (fout.is_open()) {
            std::vector<char> buffer(8192);
            zip_int64_t n;
            while ((n = zip_fread(zf, buffer.data(), buffer.size())) > 0) {
              fout.write(buffer.data(), n);
            }
            fout.close();
          }
          zip_fclose(zf);
        }
      }
      zip_close(z);
      return true;
    }
  } catch (const fs::filesystem_error &e) {
    std::cerr << "PackStore Error: " << e.what() << std::endl;
  }
  return false;
}

std::string PackStore::SanitizeFileName(const std::string &name) {
  std::string sanitized = name;
  for (char &c : sanitized) {
    if (!isalnum(c) && c != '-' && c != '_') {
      c = '_';
    }
  }
  return sanitized;
}
