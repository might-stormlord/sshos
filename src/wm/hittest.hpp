#pragma once

#include "wm/window.hpp"

namespace sshos {

// Une valeur par zone cliquable d'une fenêtre. Frame couvre ce qui
// appartient à la fenêtre sans être une poignée -- la bordure gauche : un
// clic dessus la met au premier plan, rien de plus.
enum class WinHit {
  None,
  Frame,
  TitleBar,
  ButtonMinimize,
  ButtonMaximize,
  ButtonClose,
  EdgeRight,
  EdgeBottom,
  CornerBR,
  Client,
};

struct WinHitResult {
  WinHit what = WinHit::None;
  WindowId win = 0;
  int lx = 0;  // coordonnées locales à la zone cliente, valables si Client
  int ly = 0;
};

// L'inverse exact de decor_metrics() + draw_decor(). L'ordre des tests
// ci-dessous suit celui du dessin, du plus spécifique au plus général :
// c'est ce qui garantit qu'ils ne divergent pas.
WinHitResult hit_window(const Window& w, int x, int y);

}  // namespace sshos
