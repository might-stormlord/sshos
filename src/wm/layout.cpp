#include "wm/layout.hpp"

namespace sshos {

Rect work_area(int cols, int rows, PanelEdge edge, int thickness) {
  switch (edge) {
    case PanelEdge::Top:
      return Rect{0, thickness, cols, rows - thickness};
    case PanelEdge::Bottom:
      return Rect{0, 0, cols, rows - thickness};
    case PanelEdge::Left:
      return Rect{thickness, 0, cols - thickness, rows};
    case PanelEdge::Right:
      return Rect{0, 0, cols - thickness, rows};
  }
  return Rect{0, 0, cols, rows};
}

Rect display_rect_for(const Window& w, const Rect& work, int cols, int rows) {
  switch (w.mode) {
    case WinMode::Maximized:
      return work;
    case WinMode::Fullscreen:
      // Tout l'écran, panneau compris : c'est exactement ce qui le
      // distingue du maximisé.
      return Rect{0, 0, cols, rows};
    case WinMode::Minimized:
    case WinMode::Normal:
      break;
  }
  return clamp_to(w.user_rect, work, frame_min(*w.app));
}

void relayout(WindowManager& wm, const Rect& work, int cols, int rows) {
  // display_rect seulement. user_rect est la mémoire de la disposition :
  // l'écrire ici est le seul moyen de rendre un rétrécissement du terminal
  // irréversible.
  for (const auto& w : wm.stack()) {
    w->display_rect = display_rect_for(*w, work, cols, rows);
  }
}

}  // namespace sshos
