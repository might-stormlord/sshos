#pragma once

#include <optional>
#include <string>

#include "render/profile.hpp"
#include "render/surface.hpp"

namespace sshos {

class Differ {
 public:
  explicit Differ(OutputProfile p) : profile_(p), prev_(0, 0) {}

  // Jette l'état supposé du terminal : la frame suivante est complète.
  void invalidate() { valid_ = false; }

  // Rend les octets à envoyer, ou une chaîne vide si rien n'a bougé.
  std::string frame(const Surface& cur, std::optional<Pos> cursor);

 private:
  OutputProfile profile_;
  Surface prev_;
  bool valid_ = false;
  // Suivi du curseur en champs séparés : un std::optional comparé à
  // lui-même ne distingue pas « pas encore de frame » de « curseur caché ».
  Pos last_target_{0, 0};
  bool last_shown_ = false;
  bool first_ = true;
};

}  // namespace sshos
