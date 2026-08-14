#include "wm/tile.hpp"

#include <algorithm>

namespace sshos {
namespace {

// Le nombre de colonnes pour `n` fenêtres : la racine carrée arrondie au
// -dessus. Elle donne 2 pour deux fenêtres -- deux moitiés côte à côte,
// ce que tout le monde attend -- et reste carrée ensuite.
int columns_for(int n) {
  int c = 1;
  while (c * c < n) ++c;
  return c;
}

// Découpe `total` en `parts` morceaux, le reste allant aux PREMIERS. Les
// donner aux derniers ferait glisser les fenêtres d'une cellule à chaque
// aller-retour de rangement.
std::vector<int> split(int total, int parts) {
  std::vector<int> out;
  if (parts <= 0) return out;
  const int base = total / parts;
  const int extra = total % parts;
  for (int i = 0; i < parts; ++i) {
    // Jamais zéro : une fenêtre de zéro colonne serait invisible et
    // impossible à rattraper à la souris.
    out.push_back(std::max(1, base + (i < extra ? 1 : 0)));
  }
  return out;
}

}  // namespace

std::vector<Rect> tile_rects(const Rect& work, int count) {
  std::vector<Rect> out;
  if (count <= 0 || work.w <= 0 || work.h <= 0) return out;
  out.reserve(static_cast<size_t>(count));

  const int cols = columns_for(count);
  // Les colonnes les plus à gauche prennent la ligne en trop : c'est le
  // même reste que pour les largeurs, et le garder du même côté rend la
  // disposition lisible.
  const std::vector<int> rows_per_col = split(count, cols);
  const std::vector<int> widths = split(work.w, cols);

  int x = work.x;
  for (int c = 0; c < cols; ++c) {
    const int w = widths[static_cast<size_t>(c)];
    const std::vector<int> heights =
        split(work.h, rows_per_col[static_cast<size_t>(c)]);
    int y = work.y;
    for (int h : heights) {
      out.push_back(Rect{x, y, w, h});
      y += h;
    }
    x += w;
  }
  return out;
}

Rect snap_rect(const Rect& work, SnapDir d) {
  // Le reste va a la moitie gauche (ou haute) : deux fenetres dos a dos
  // doivent se toucher sans colonne vide ni chevauchement.
  const int w1 = work.w - work.w / 2;
  const int h1 = work.h - work.h / 2;
  switch (d) {
    case SnapDir::Left:
      return Rect{work.x, work.y, w1, work.h};
    case SnapDir::Right:
      return Rect{work.x + w1, work.y, work.w - w1, work.h};
    case SnapDir::Up:
      return Rect{work.x, work.y, work.w, h1};
    case SnapDir::Down:
      return Rect{work.x, work.y + h1, work.w, work.h - h1};
  }
  return work;
}

Rect snap_opposite(const Rect& work, SnapDir d) {
  switch (d) {
    case SnapDir::Left:
      return snap_rect(work, SnapDir::Right);
    case SnapDir::Right:
      return snap_rect(work, SnapDir::Left);
    case SnapDir::Up:
      return snap_rect(work, SnapDir::Down);
    case SnapDir::Down:
      return snap_rect(work, SnapDir::Up);
  }
  return work;
}

}  // namespace sshos
