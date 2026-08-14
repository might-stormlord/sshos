#include "shell/help.hpp"

#include <algorithm>

#include "common/utf8.hpp"
#include "input/shortcuts.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

// Cadre du haut, en-tête, ligne vide, ... , cadre du bas.
constexpr int kChrome = 4;
// Bordure, marge, colonne des touches, séparation, colonne de la marque
// « s'enchaîne », séparation, colonne des effets, marge, bordure.
constexpr int kGutters = 7;

// Mesurées une fois : la table est constante, et l'aide se redessine à
// chaque trame tant qu'elle est ouverte.
int keys_width() {
  static const int w = [] {
    int m = 0;
    for (const auto& r : binding_help()) m = std::max(m, text_cells(r.keys));
    return m;
  }();
  return w;
}

int what_width() {
  static const int w = [] {
    int m = 0;
    for (const auto& r : binding_help()) m = std::max(m, text_cells(r.what));
    return m;
  }();
  return w;
}

std::string maybe_fold(const char* s, bool utf8) {
  return utf8 ? std::string(s) : fold_to_ascii(s);
}

}  // namespace

Rect Help::rect(int cols, int rows) const {
  const int want_w = keys_width() + what_width() + kGutters;
  const int w = std::min(want_w, cols);
  const int want_h = static_cast<int>(binding_help().size()) + kChrome;
  const int h = std::min(want_h, rows);
  return Rect{(cols - w) / 2, (rows - h) / 2, w, h};
}

void Help::layout(int cols, int rows) { rect_ = rect(cols, rows); }

void Help::draw(View v, const Theme& th, Border b,
                const std::string& leader_label, bool utf8) const {
  if (!open_) return;

  Style st;
  st.bg = th.modal_bg;
  st.fg = th.modal_fg;
  v.fill(rect_, st);
  v.box(rect_, b, st);

  Style head = st;
  head.fg = th.accent;

  Style keys = st;
  keys.fg = th.accent;

  const int x = rect_.x + 2;
  const int head_room = std::max(0, rect_.x + rect_.w - 1 - x);
  v.text(x, rect_.y + 1,
         // L'ANCRAGE se dit ICI et non dans le tableau : celui-ci
         // documente ce que l'accord permet, et `Ctrl+fleche` s'en passe.
         // L'y mettre ferait mentir l'en-tete « puis : » -- et deux gardes
         // de la suite le refusent, a juste titre.
         elide_to_cells(leader_label + " puis :   (Ctrl+fleches : ancrer)",
                        head_room, utf8 ? "…" : "~"),
         head);

  // Ce qui ne tient pas est coupé plutôt que débordé : sur un terminal trop
  // court, une aide amputée reste plus utile qu'un cadre qui écrase le
  // bureau. Les accords les plus courants sont en tête de table, donc ce
  // sont les derniers à disparaître qui comptent le moins.
  const int room = std::max(0, rect_.h - kChrome);
  const int n = std::min(static_cast<int>(binding_help().size()), room);

  // Sur un terminal étroit, le cadre est plus court que la table. Les deux
  // colonnes sont donc coupées à ce qui reste VRAIMENT entre la marge et la
  // bordure droite : sans ce calcul, View::text clippe à la surface et non
  // au cadre, et le texte mange la bordure -- défaut vu à la sonde à 40x12,
  // pas par un test.
  const int right = rect_.x + rect_.w - 1;  // colonne de la bordure droite
  const int kw = std::min(keys_width(), std::max(0, right - x));
  // Une colonne à elle seule pour la marque « s'enchaîne » : l'accoler au
  // libellé élargirait le cadre de la plus longue ligne, et l'y élider
  // effacerait justement la marque.
  const int flag_x = x + kw + 1;
  const int what_x = x + kw + 3;
  const int what_room = std::max(0, right - what_x);
  const std::string mark = utf8 ? "…" : "~";
  const std::string chain = utf8 ? "∙" : "+";

  // L'en-tête porte la légende, au-dessus de la colonne qu'elle explique.
  if (what_room > 0) {
    v.text(flag_x, rect_.y + 1,
           elide_to_cells(chain + maybe_fold(" s'enchaîne", utf8),
                          what_room + 2, mark),
           st);
  }

  for (int i = 0; i < n; ++i) {
    const HelpRow& r = binding_help()[static_cast<size_t>(i)];
    const int y = rect_.y + 3 + i;
    if (y >= rect_.y + rect_.h - 1) break;
    v.text(x, y, elide_to_cells(maybe_fold(r.keys, utf8), kw, mark), keys);
    // Une ligne s'enchaîne si ce qu'elle documente s'enchaîne. La marque est
    // DÉRIVÉE, jamais recopiée : elle ne peut pas mentir sur la table.
    if (!r.actions.empty() && is_repeatable(r.actions.front())) {
      v.text(flag_x, y, chain, keys);
    }
    v.text(what_x, y, elide_to_cells(maybe_fold(r.what, utf8), what_room, mark),
           st);
  }
}

}  // namespace sshos
