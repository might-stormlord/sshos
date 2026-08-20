#include "render/gauge.hpp"

#include <algorithm>

namespace sshos {

std::string gauge_bar(int percent, int width, Border b) {
  const std::string full = b == Border::Unicode ? "█" : "#";
  const std::string empty = b == Border::Unicode ? "░" : "-";
  std::string out;
  const int filled = width * std::clamp(percent, 0, 100) / 100;
  for (int i = 0; i < width; ++i) out += (i < filled) ? full : empty;
  return out;
}

}  // namespace sshos
