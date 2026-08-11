#include "wm/hittest.hpp"

#include "wm/decor.hpp"

namespace sshos {

WinHitResult hit_window(const Window& w, int x, int y) {
  const Rect f = w.display_rect;
  WinHitResult r;
  if (!f.contains(x, y)) return r;

  r.win = w.id;
  const DecorMetrics m = decor_metrics(f);

  // Le coin d'abord : il appartient à la fois au bord droit et au bord
  // bas, et c'est lui qui redimensionne dans les deux sens à la fois.
  if (x == f.x + f.w - 1 && y == f.y + f.h - 1 && f.h >= 2) {
    r.what = WinHit::CornerBR;
    return r;
  }

  if (y == m.title_bar.y) {
    if (m.button_count > 0 && x >= m.buttons.x && x < m.buttons.x + m.buttons.w) {
      const int slot = (x - m.buttons.x) / 3;       // 0..button_count-1
      const int which = 3 - m.button_count + slot;  // 0 réduire, 2 fermer
      r.what = (which == 0)   ? WinHit::ButtonMinimize
               : (which == 1) ? WinHit::ButtonMaximize
                              : WinHit::ButtonClose;
      return r;
    }
    r.what = WinHit::TitleBar;
    return r;
  }

  if (x == f.x + f.w - 1) {
    r.what = WinHit::EdgeRight;
    return r;
  }
  if (y == f.y + f.h - 1) {
    r.what = WinHit::EdgeBottom;
    return r;
  }

  const Rect c = m.client;
  if (c.contains(x, y)) {
    r.what = WinHit::Client;
    r.lx = x - c.x;
    r.ly = y - c.y;
    return r;
  }

  // Ce qui reste : la bordure gauche.
  r.what = WinHit::Frame;
  return r;
}

}  // namespace sshos
