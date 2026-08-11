#pragma once

#include <string>

#include "common/platform.hpp"

namespace sshos {

// L'heure du panneau, plus le drapeau qui dit si elle a bougé. Ce drapeau
// est tout l'intérêt de la classe : sans lui le bureau se repeindrait à
// chaque réveil du démon, trente fois par seconde, pour redessiner deux
// chiffres identiques.
class Clock {
 public:
  // Rend true si le TEXTE affiché a changé -- pas si l'instant a changé.
  bool update(const Platform& plat);

  const std::string& text() const { return text_; }
  const std::string& date() const { return date_; }

 private:
  std::string text_;
  std::string date_;
  bool primed_ = false;
};

}  // namespace sshos
