#include "render/diff.hpp"

namespace sshos {
namespace {

std::string cup(int x, int y) {
  return "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

Style style_of(const Cell& c) { return Style{c.fg, c.bg, c.attrs}; }

}  // namespace

std::string Differ::frame(const Surface& cur, std::optional<Pos> cursor) {
  if (prev_.w() != cur.w() || prev_.h() != cur.h()) {
    prev_.resize(cur.w(), cur.h());
    valid_ = false;
  }

  const bool full = !valid_;
  bool any = false;
  std::string body;

  Style pen;          // état SGR courant, valable sur TOUTE la frame
  bool pos_known = false;
  int px = 0;
  int py = 0;

  for (int y = 0; y < cur.h(); ++y) {
    int x = 0;
    while (x < cur.w()) {
      const bool differs = full || !(cur.at(x, y) == prev_.at(x, y));
      if (!differs) {
        ++x;
        continue;
      }

      // Règle 1 : ne jamais démarrer sur une cellule de continuation, on
      // remonte à la cellule de tête et on réémet la paire entière.
      int start = x;
      while (start > 0 && cur.at(start, y).width == 0) --start;

      // L'extension part de la première cellule DIFFÉRENTE, pas de `start` :
      // partir de `start` ferait sortir immédiatement quand la cellule de
      // tête est identique, et le segment serait vide — boucle infinie.
      int end = x + 1;
      while (end < cur.w()) {
        const bool d = full || !(cur.at(end, y) == prev_.at(end, y));
        const bool cont = cur.at(end, y).width == 0;
        if (!d && !cont) break;
        ++end;
      }

      for (int c = start; c < end; ++c) {
        const Cell& cell = cur.at(c, y);
        if (cell.width == 0) continue;  // couverte par sa cellule de tête
        if (!pos_known || px != c || py != y) {
          body += cup(c, y);
          pos_known = true;
          px = c;
          py = y;
        }
        body += sgr_transition(pen, style_of(cell), profile_);
        pen = style_of(cell);
        body += encode_utf8(cell.ch);
        px += cell.width;
        // Règle 3 : après un graphème non-ASCII la position implicite du
        // curseur n'est pas fiable — la cellule suivante se réancre au CUP.
        if (cell.ch >= 0x80) pos_known = false;
      }

      any = true;
      x = end;
    }
  }

  const Pos target = cursor.value_or(Pos{0, 0});
  const bool shown = cursor.has_value();
  const bool cursor_changed =
      first_ || !(target == last_target_) || shown != last_shown_;
  if (!any && !full && !cursor_changed) return "";

  std::string out = "\033[?25l\033[0m";
  out += body;
  out += cup(target.x, target.y);
  if (cursor.has_value()) out += "\033[?25h";

  for (int y = 0; y < cur.h(); ++y) {
    for (int x = 0; x < cur.w(); ++x) prev_.at(x, y) = cur.at(x, y);
  }
  valid_ = true;
  last_target_ = target;
  last_shown_ = shown;
  first_ = false;
  return out;
}

}  // namespace sshos
