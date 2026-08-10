#include "render/diff.hpp"

#include "common/utf8.hpp"

namespace sshos {
namespace {

std::string cup(int x, int y) {
  return "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

Style style_of(const Cell& c) { return Style{c.fg, c.bg, c.attrs}; }

// Seuil de rentabilité de `\033[K` (3 octets) face à N espaces littérales.
// Dans le cas le plus favorable aux espaces — le pinceau est déjà au style
// par défaut en entrant dans la queue, donc aucune transition SGR n'est
// nécessaire ni pour l'effacement ni pour les espaces — N espaces coûtent
// N octets : égalité à N=3, l'effacement gagne strictement à partir de
// N=4. Ce que ce seuil garantit réellement : l'effacement n'est jamais
// pire que les espaces littérales — une égalité reste possible dès que le
// pinceau porte un fond ou des attributs non défaut. Exemple à N=4 :
// pinceau Bold + fond non défaut. Les espaces doivent rejoindre Style{},
// et aucun code SGR n'éteint Bold isolément (cf. sgr_transition), donc
// `\033[0m` (4 octets) puis 4 espaces : 8 octets. L'effacement ne
// réinitialise que le fond, `\033[49m` (5 octets) puis `\033[K` (3
// octets) : 8 octets aussi — l'octet d'écart entre les deux resets
// compense exactement la marge du seuil. On ne réinitialise que le fond,
// pas tout via `\033[0m` (un octet de moins ici) : un reset complet
// effacerait aussi fg et les attributs, que le reste de la frame devrait
// ensuite rétablir par une transition SGR — l'économie locale se
// paierait ailleurs.
constexpr int kMinErasableTail = 4;

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
    // La queue effaçable : le plus long suffixe de cellules égales à
    // Cell{} (cell.hpp — un blanc au sens strict : largeur 1, style par
    // défaut, aucun attribut). Cette égalité totale exclut aussi bien les
    // cellules de continuation (largeur 0, jamais == Cell{}) que les
    // espaces qui portent Reverse ou Underline : ceux-là restent visibles
    // malgré leur glyphe vide et ne doivent pas être sacrifiés à un CSI K.
    int tail_start = cur.w();
    while (tail_start > 0 && cur.at(tail_start - 1, y) == Cell{}) --tail_start;
    const int tail_len = cur.w() - tail_start;
    // Sous le seuil, la queue est traitée comme des cellules ordinaires :
    // pas d'effacement, pour ne pas payer 3 octets pour économiser moins.
    const bool erase_tail = tail_len >= kMinErasableTail;
    const int content_end = erase_tail ? tail_start : cur.w();

    int x = 0;
    while (x < content_end) {
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
      // Bornée à `content_end` : un run ne doit jamais mordre sur la queue
      // effaçable, sinon il réémettrait en clair les espaces qu'on cherche
      // justement à remplacer par un CSI K.
      int end = x + 1;
      while (end < content_end) {
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

    if (erase_tail) {
      // N'effacer que si la queue en a réellement besoin : sinon un bureau
      // au repos ne serait plus silencieux (diff_emits_nothing_when_...).
      bool tail_differs = full;
      for (int c = tail_start; !tail_differs && c < cur.w(); ++c) {
        tail_differs = !(cur.at(c, y) == prev_.at(c, y));
      }
      if (tail_differs) {
        if (!pos_known || px != tail_start || py != y) {
          body += cup(tail_start, y);
          pos_known = true;
          px = tail_start;
          py = y;
        }
        // CSI K efface avec le FOND SGR courant, pas le fond par défaut du
        // terminal : sur un fond non défaut ce serait une barre colorée
        // jusqu'en bordure droite. On ramène donc le pinceau au fond par
        // défaut avant d'effacer, et on met `pen` à jour en conséquence
        // pour que la prochaine transition SGR de la frame parte d'une
        // prémisse exacte. fg et les attributs n'ont pas besoin d'être
        // touchés : CSI K ne dessine aucun glyphe, ils sont sans effet.
        if (!(pen.bg == Color::def())) {
          Style bg_default = pen;
          bg_default.bg = Color::def();
          body += sgr_transition(pen, bg_default, profile_);
          pen = bg_default;
        }
        body += "\033[K";
        // CSI K ne déplace pas le curseur : px/py restent au point de
        // départ de l'effacement, déjà positionnés ci-dessus.
        any = true;
      }
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
