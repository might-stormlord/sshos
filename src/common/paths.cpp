#include "common/paths.hpp"

#include <cstdlib>

namespace sshos {

std::string user_data_dir() {
  if (const char* x = std::getenv("XDG_DATA_HOME")) {
    if (*x != '\0') return std::string(x) + "/termos";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  return std::string(home) + "/.local/share/termos";
}

}  // namespace sshos
