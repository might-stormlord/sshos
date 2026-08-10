#pragma once

#include <string>
#include <string_view>

#include "render/cell.hpp"

namespace sshos {

enum class ColorDepth { Mono16, Indexed256, TrueColor };

struct OutputProfile {
  ColorDepth depth = ColorDepth::Mono16;
  bool utf8 = false;

  static OutputProfile detect(std::string_view term, std::string_view colorterm,
                              bool utf8);
};

// Séquence minimale pour passer du style `from` au style `to`. Le diffeur
// suit l'état courant sur toute la frame : une ligne uniforme ne coûte
// qu'un seul SGR.
std::string sgr_transition(const Style& from, const Style& to,
                           const OutputProfile& p);

std::string encode_utf8(char32_t cp);

}  // namespace sshos
