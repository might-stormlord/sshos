#pragma once

#include "render/cell.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/window.hpp"

namespace sshos {

// Toute la géométrie des décorations, en coordonnées ABSOLUES. Fonction
// pure, séparée du dessin : hit_window() (tâche 5) doit en être l'inverse
// exact, et deux calculs enfouis dans deux boucles de rendu divergeraient
// à la première retouche.
struct DecorMetrics {
  Rect title_bar;   // ligne du haut, toute la largeur du cadre
  Rect title_text;  // là où le titre a le droit de s'écrire
  Rect buttons;     // w == 0 quand ils sont tous élidés
  int button_count = 0;
  Rect client;
};

DecorMetrics decor_metrics(const Rect& frame);

// `v` est la vue RACINE de l'écran : draw_decor peint en coordonnées
// absolues, à partir de w.display_rect.
void draw_decor(View v, const Window& w, bool focused, const Theme& th, Border b);

}  // namespace sshos
