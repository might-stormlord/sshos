#pragma once

#include <string>

#include "render/surface.hpp"
#include "render/theme.hpp"

namespace sshos {

// La table des accords, affichée. §16 de la spec donne « la touche leader
// est peu découvrable pour qui vient d'un vrai bureau » comme risque, et
// trois parades ; celle-ci en est une, le rappel du panneau une autre.
//
// Elle s'ouvre TOUTE SEULE quand le leader reste armé sans suite : c'est
// exactement l'instant où l'utilisateur ne sait pas quoi taper. Un menu
// d'aide qu'il faut savoir demander ne servirait que ceux qui n'en ont pas
// besoin.
class Help {
 public:
  void open() { open_ = true; }
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  Rect rect(int cols, int rows) const;
  void layout(int cols, int rows);

  // `leader_label` est la touche telle qu'elle se tape aujourd'hui : elle
  // est configurable (spec §12), et une aide qui nommerait « Ctrl+A » en
  // dur mentirait au premier utilisateur qui en change.
  void draw(View v, const Theme& th, Border b, const std::string& leader_label,
            bool utf8) const;

 private:
  bool open_ = false;
  Rect rect_{};
};

}  // namespace sshos
