#pragma once
#include <string>

namespace Platform {
void OpenURL(const std::string &url);
std::string PickOpenFileName(const std::string &filter = "");
}
