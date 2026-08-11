#include "wm/window.hpp"

#include <algorithm>

namespace sshos {

Rect client_rect(const Rect& frame) {
  Rect c{frame.x + 1, frame.y + 1, frame.w - 2, frame.h - 2};
  c.w = std::max(0, c.w);
  c.h = std::max(0, c.h);
  return c;
}

Size frame_min(const App& app) {
  const Size m = app.min_size();
  // min_size() porte sur la zone cliente, le plancher du projet sur le
  // cadre : on ajoute les décorations avant de comparer.
  return Size{std::max(m.w + 2, 16), std::max(m.h + 2, 5)};
}

Rect clamp_to(Rect frame, const Rect& work, Size min) {
  frame.w = std::max(min.w, std::min(frame.w, work.w));
  frame.h = std::max(min.h, std::min(frame.h, work.h));
  // Si le plancher dépasse la zone de travail, les deux max() ci-dessous
  // collent la fenêtre en haut à gauche et la laissent déborder. C'est
  // voulu : une fenêtre qui déborde reste utilisable, une fenêtre écrasée
  // sous son minimum ne l'est pas.
  frame.x = std::max(work.x, std::min(frame.x, work.x + work.w - frame.w));
  frame.y = std::max(work.y, std::min(frame.y, work.y + work.h - frame.h));
  return frame;
}

}  // namespace sshos
