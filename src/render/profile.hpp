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

// encode_utf8() a déménagé dans common/utf8.hpp, aux côtés d'utf8_decode()
// qu'elle complète (voir le commentaire là-bas pour la raison). Ce
// header ne la ré-exporte pas : un appelant qui l'obtenait ici
// transitivement doit désormais inclure common/utf8.hpp explicitement.

}  // namespace sshos
