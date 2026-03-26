#pragma once
#include <string>

namespace Platform {
void OpenURL(const std::string &url);
std::string GetOpenFileName(const std::string &filter = "");
}
