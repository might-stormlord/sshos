#include "shell/snapassist.hpp"

#include <algorithm>

#include "render/width.hpp"

namespace sshos {
namespace {

// « Ancrer ici : » plus une ligne par fenêtre, le tout dans un cadre. Deux
// lignes de bordure, une de titre : en dessous de quatre lignes il ne reste
// plus une seule proposition à montrer.
constexpr int kChromeRows = 3;
constexpr int kMinCols = 12;
constexpr char kHeading[] = "Ancrer ici :";

}  // namespace

void SnapAssist::open(const Rect& free, std::vector<SnapCandidate> choices) {
  close();
  // Le cadre tient à ce qu'il montre, borné par la moitié libre : on ne
  // s'étale pas sur toute la hauteur pour trois entrées, et on ne déborde
  // jamais -- un demi-cadre clippé par la vue ne se cliquerait pas là où on
  // le voit.
  int widest = text_cells(kHeading);
  for (const SnapCandidate& c : choices) {
    widest = std::max(widest, text_cells(c.title));
  }
  const int w = std::min(free.w - 2, widest + 4);
  const int rows = std::min(static_cast<int>(choices.size()),
                            free.h - 2 - kChromeRows);
  // TROIS REFUS EN UNE LIGNE, et c'est voulu : rien à proposer (`rows` vaut
  // alors zéro), une moitié trop basse pour une seule ligne (il devient
  // négatif), une moitié trop étroite pour qu'on lise quoi que ce soit. Des
  // gardes séparées en tête ont été écrites, puis retirées -- la campagne
  // de mutation les a montrées INOBSERVABLES, celle-ci les couvrant toutes.
  if (rows <= 0 || w < kMinCols) return;
  choices.resize(static_cast<size_t>(rows));

  const int h = rows + kChromeRows;
  rect_ = Rect{free.x + (free.w - w) / 2, free.y + (free.h - h) / 2, w, h};
  choices_ = std::move(choices);
  open_ = true;
}

void SnapAssist::close() {
  open_ = false;
  choices_.clear();
  rect_ = Rect{};
}

int SnapAssist::row_of(size_t i) const {
  // Bordure haute, puis le titre, puis les choix.
  return rect_.y + 2 + static_cast<int>(i);
}

void SnapAssist::draw(View v, const Theme& th, Border b) const {
  if (!open_) return;

  Style st;
  st.bg = th.modal_bg;
  st.fg = th.modal_fg;
  v.fill(rect_, st);
  v.box(rect_, b, st);

  Style head = st;
  head.fg = th.accent;
  const int x = rect_.x + 2;
  // La place VRAIMENT disponible entre la marge et la bordure droite : la
  // vue clippe à la surface et non au cadre, et un titre trop long
  // mangerait la bordure au lieu d'être coupé.
  const int room = std::max(0, rect_.x + rect_.w - 1 - x);
  v.text(x, rect_.y + 1, elide_to_cells(kHeading, room, "…"), head);

  for (size_t i = 0; i < choices_.size(); ++i) {
    v.text(x, row_of(i), elide_to_cells(choices_[i].title, room, "…"), st);
  }
}

WindowId SnapAssist::hit(int x, int y) const {
  if (!open_) return 0;
  // LA BORDURE ET LE TITRE NE SONT PAS DES CHOIX : cliquer le cadre
  // ancrerait la première fenêtre de la liste sans que personne l'ait
  // désignée.
  if (x <= rect_.x || x >= rect_.x + rect_.w - 1) return 0;
  for (size_t i = 0; i < choices_.size(); ++i) {
    if (y == row_of(i)) return choices_[i].win;
  }
  return 0;
}

}  // namespace sshos
