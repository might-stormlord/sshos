#include "common/platform.hpp"

#include <fstream>
#include <sstream>

namespace sshos {

std::string RealPlatform::read_file(std::string_view path) const {
  std::ifstream in{std::string(path)};
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

}  // namespace sshos
