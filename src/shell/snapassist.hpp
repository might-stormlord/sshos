#pragma once

#include <string>
#include <vector>

#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/window.hpp"

namespace sshos {

// L'ASSISTANCE À L'ANCRAGE. Une fenêtre vient de prendre une moitié de
// l'écran ; l'autre est libre, et vide. Plutôt que de laisser l'utilisateur
// aller chercher la seconde fenêtre pour refaire le geste dans l'autre
// sens, on la lui propose LÀ OÙ ELLE IRAIT, et un clic l'y met.
//
// C'est une PROPOSITION, pas un dialogue : elle ne prend pas le clavier,
// elle disparaît à la première touche comme à un clic à côté, et le geste
// garde son effet. Un bureau qui exige qu'on réponde à ce qu'on n'a pas
// demandé se paie deux fois.
struct SnapCandidate {
  WindowId win = 0;
  std::string title;
};

class SnapAssist {
 public:
  // Ouvre la proposition dans `free`, en coordonnées d'écran. Une liste
  // vide ou une moitié trop petite ne l'ouvre pas : mieux vaut rien qu'un
  // cadre qui déborde ou qu'on ne peut pas lire.
  void open(const Rect& free, std::vector<SnapCandidate> choices);
  void close();

  bool is_open() const { return open_; }
  const Rect& rect() const { return rect_; }
  const std::vector<SnapCandidate>& choices() const { return choices_; }

  void draw(View v, const Theme& th, Border b) const;

  // La fenêtre proposée sous ce point, ou 0 -- ce qui vaut « pas sur la
  // proposition ». Comme partout ailleurs dans ce projet, le dessin et le
  // clic lisent la MÊME géométrie : `rect_` est posée une seule fois, à
  // l'ouverture.
  WindowId hit(int x, int y) const;

 private:
  // La ligne d'écran d'un choix, et l'inverse.
  int row_of(size_t i) const;

  bool open_ = false;
  Rect rect_{};
  std::vector<SnapCandidate> choices_;
};

}  // namespace sshos
