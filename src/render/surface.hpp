#pragma once

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/utf8.hpp"
#include "render/cell.hpp"

namespace sshos {

class View;

class Surface {
 public:
  Surface(int w, int h);

  int w() const { return w_; }
  int h() const { return h_; }

  void resize(int w, int h);
  void clear(Style s);

  // Les vérifications de bornes sont en assert() plutôt que lancées en temps
  // d'exécution, car at() est appelée serrée dans la boucle du diffeur sur
  // chaque cellule de chaque trame (des millions de fois par seconde). Une
  // vérification permanente serait un goulot. En Release (NDEBUG), elle
  // disparaît ; en Debug sous AddressSanitizer, elle détecte les accès
  // illégaux au stade du développement.
  const Cell& at(int x, int y) const {
    assert(x >= 0 && x < w_ && y >= 0 && y < h_);
    return cells_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)];
  }
  Cell& at(int x, int y) {
    assert(x >= 0 && x < w_ && y >= 0 && y < h_);
    return cells_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)];
  }

  View root();

  // Ligne rendue en UTF-8, cellules de continuation omises. Support des
  // assertions de propriété : `CHECK(s.text_row(3).find("Terminal") != npos)`
  // résiste à un changement de thème, un golden d'octets non.
  std::string text_row(int y) const;

 private:
  int w_ = 0;
  int h_ = 0;
  std::vector<Cell> cells_;
};

// Rectangle clippé et translaté sur une Surface. Une application ne reçoit
// jamais autre chose que ça.
class View {
 public:
  View(Surface& s, Rect clip) : s_(&s), clip_(clip) {}

  int w() const { return clip_.w; }
  int h() const { return clip_.h; }

  void put(int x, int y, char32_t ch, Style st);
  int text(int x, int y, std::string_view utf8, Style st);
  void fill(Rect r, Style st);
  View sub(Rect r) const;

 private:
  bool map(int x, int y, int& ox, int& oy) const;
  void cleanup_orphan(int ox, int oy);

  Surface* s_;
  Rect clip_;
};

// utf8_decode() vit désormais dans common/utf8.hpp (partagé avec input/,
// cf. son commentaire) ; il reste visible ici sous sshos::utf8_decode via
// l'include ci-dessus, sans changement pour les appelants de ce header.

}  // namespace sshos
