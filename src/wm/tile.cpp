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

}  // namespace sshos
