#include "shell/panel.hpp"

#include <algorithm>

#include "app/catalog.hpp"

namespace sshos {
namespace {

// Un libellé plus long que ça est coupé. Huit cellules laissent « Battemen »
// lisible, et c'est la limite au-delà de laquelle une barre de 80 colonnes
// ne montre plus assez de fenêtres pour servir à quelque chose.
constexpr int kLabelCells = 8;

// « HH:MM » : toujours cinq cellules, ce qui permet à layout() de réserver
// la place de l'horloge sans connaître son texte -- draw() ne le reçoit
// qu'ensuite.
constexpr int kClockCells = 5;

constexpr int kVerticalCols = 16;

// Compte les cellules d'affichage d'une chaîne UTF-8 en comptant les octets
// qui NE sont PAS des continuations. Exact ici parce que tout ce que le
// panneau écrit fait une cellule de large : libellés ASCII (les deux
// applications du catalogue posent des titres ASCII), plus ●, », … et ☰,
// tous de largeur 1 sous la chasse ambiguë étroite que le démon
// installe à chaque attache (daemon.cpp, set_ambiguous_wide(false)).
int cells_of(const std::string& s) {
  int n = 0;
  for (const char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

// Coupe à `cells` cellules au plus, jamais au milieu d'une séquence UTF-8,
// et marque la coupure.
std::string elide(const std::string& s, int cells, bool utf8) {
  if (cells_of(s) <= cells) return s;
  const int keep = cells - 1;  // une cellule pour la marque de coupure
  int seen = 0;
  size_t i = 0;
  while (i < s.size()) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      if (seen == keep) break;
      ++seen;
    }
    ++i;
  }
  return s.substr(0, i) + (utf8 ? "…" : "~");
}

}  // namespace

int Panel::thickness() const { return horizontal() ? 1 : kVerticalCols; }

Rect Panel::rect(int cols, int rows) const {
  switch (edge_) {
    case PanelEdge::Top:
      return Rect{0, 0, cols, 1};
    case PanelEdge::Bottom:
      return Rect{0, rows - 1, cols, 1};
    case PanelEdge::Left:
      return Rect{0, 0, kVerticalCols, rows};
    case PanelEdge::Right:
      return Rect{cols - kVerticalCols, 0, kVerticalCols, rows};
  }
  return Rect{0, rows - 1, cols, 1};
}

void Panel::layout(const WindowManager& wm, int cols, int rows, bool utf8) {
  utf8_ = utf8;
  rect_ = rect(cols, rows);
  items_.clear();
  if (rect_.w <= 0 || rect_.h <= 0) return;
  if (horizontal()) {
    layout_horizontal(wm);
  } else {
    layout_vertical(wm);
  }
}

void Panel::layout_horizontal(const WindowManager& wm) {
  const int y = rect_.y;
  const int right = rect_.x + rect_.w;

  // L'horloge est collée à l'autre bout et sa place est réservée d'abord :
  // tout le reste se dispose dans ce qu'elle laisse.
  const int clock_x = right - kClockCells - 1;
  items_.push_back({PanelHit::Clock, -1, 0, false,
                    Rect{clock_x, y, kClockCells, 1}, std::string()});

  int x = rect_.x + 1;
  // Le bouton de menu porte la marque du projet. En UTF-8 il gagne son
  // glyphe ; sans UTF-8 le mot seul reste lisible, là où un point
  // d'interrogation ne dirait rien.
  const std::string menu = utf8_ ? "☰ ssh_os" : "ssh_os";
  const int menu_w = cells_of(menu);
  items_.push_back(
      {PanelHit::MenuButton, -1, 0, false, Rect{x, y, menu_w, 1}, menu});
  x += menu_w + 1;

  // Les épinglées : le catalogue au complet, dans son ordre.
  int pi = 0;
  for (const auto& e : catalog()) {
    const std::string t = "[" + elide(e.label, kLabelCells, utf8_) + "]";
    const int w = cells_of(t);
    if (x + w > clock_x - 1) break;
    items_.push_back({PanelHit::Pinned, pi, 0, false, Rect{x, y, w, 1}, t});
    x += w + 1;
    ++pi;
  }

  const auto& stack = wm.stack();
  const int total = static_cast<int>(stack.size());
  const int start_x = x;

  // Combien d'entrées tiennent avant `end`. Appelée deux fois : une fois
  // sans réserve, puis, si tout ne tient pas, avec la place du compteur de
  // repli mise de côté.
  const auto fits = [&](int end) {
    int cx = start_x;
    int n = 0;
    for (const auto& up : stack) {
      const std::string t = elide(up->title, kLabelCells, utf8_);
      const int w = 1 + cells_of(t);
      if (cx + w > end) break;
      cx += w + 1;
      ++n;
    }
    return n;
  };

  int end = clock_x - 1;
  int shown = fits(end);
  int over_w = 0;
  if (shown < total) {
    // Les libellés sont déjà à leur minimum : ce qui dépasse se replie sur
    // un compteur, plutôt que de disparaître sans le dire.
    over_w = 1 + static_cast<int>(std::to_string(total).size());
    end = clock_x - 1 - over_w - 1;
    shown = fits(end);
  }

  for (int i = 0; i < shown; ++i) {
    const Window& w = *stack[static_cast<size_t>(i)];
    const bool focused = w.id == wm.focused();
    std::string mark = " ";
    if (w.mode == WinMode::Minimized) {
      mark = "_";
    } else if (focused) {
      mark = utf8_ ? "●" : "*";
    }
    const std::string t = mark + elide(w.title, kLabelCells, utf8_);
    const int cw = cells_of(t);
    items_.push_back({PanelHit::Task, i, w.id, focused, Rect{x, y, cw, 1}, t});
    x += cw + 1;
  }

  const int hidden = total - shown;
  if (hidden > 0) {
    const std::string t = (utf8_ ? "»" : ">") + std::to_string(hidden);
    const int w = cells_of(t);
    items_.push_back({PanelHit::Overflow, hidden, 0, false,
                      Rect{clock_x - 1 - w, y, w, 1}, t});
  }
}

void Panel::layout_vertical(const WindowManager& wm) {
  const int x = rect_.x;
  const int w = rect_.w;
  const int bottom = rect_.y + rect_.h;

  // L'horloge occupe les deux dernières lignes : la date au-dessus de
  // l'heure. Les deux répondent Clock au hit-test, c'est le même objet.
  const int clock_h = rect_.h >= 3 ? 2 : 1;
  items_.push_back({PanelHit::Clock, -1, 0, false,
                    Rect{x, bottom - clock_h, w, clock_h}, std::string()});

  int y = rect_.y;
  const std::string menu = utf8_ ? "☰ ssh_os" : "ssh_os";
  items_.push_back(
      {PanelHit::MenuButton, -1, 0, false, Rect{x, y, w, 1}, menu});
  ++y;

  const int label_cells = w - 2;
  int pi = 0;
  for (const auto& e : catalog()) {
    if (y >= bottom - clock_h) break;
    const std::string t = "[" + elide(e.label, label_cells, utf8_) + "]";
    items_.push_back({PanelHit::Pinned, pi, 0, false, Rect{x, y, w, 1}, t});
    ++y;
    ++pi;
  }

  const auto& stack = wm.stack();
  const int total = static_cast<int>(stack.size());
  int room = std::max(0, bottom - clock_h - y);
  const bool folds = total > room;
  if (folds) room = std::max(0, room - 1);  // une ligne pour le compteur

  const int shown = std::min(total, room);
  for (int i = 0; i < shown; ++i) {
    const Window& win = *stack[static_cast<size_t>(i)];
    const bool focused = win.id == wm.focused();
    std::string mark = " ";
    if (win.mode == WinMode::Minimized) {
      mark = "_";
    } else if (focused) {
      mark = utf8_ ? "●" : "*";
    }
    const std::string t = mark + elide(win.title, label_cells, utf8_);
    items_.push_back({PanelHit::Task, i, win.id, focused, Rect{x, y, w, 1}, t});
    ++y;
  }

  const int hidden = total - shown;
  if (hidden > 0) {
    const std::string t = (utf8_ ? "»" : ">") + std::to_string(hidden);
    items_.push_back({PanelHit::Overflow, hidden, 0, false, Rect{x, y, w, 1}, t});
  }
}

void Panel::draw(View v, const Theme& th, const std::string& clock_text,
                 const std::string& date_text) const {
  Style base;
  base.bg = th.panel_bg;
  base.fg = th.panel_fg;
  v.fill(rect_, base);

  Style hot = base;
  hot.fg = th.accent;

  for (const auto& it : items_) {
    if (it.what == PanelHit::Clock) {
      if (it.r.h >= 2) {
        v.text(it.r.x + 1, it.r.y, date_text, base);
        v.text(it.r.x + 1, it.r.y + 1, clock_text, base);
      } else {
        // Calé à droite dans la place que layout() lui a réservée.
        const int pad = std::max(0, kClockCells - cells_of(clock_text));
        v.text(it.r.x + pad, it.r.y, clock_text, base);
      }
      continue;
    }
    v.text(it.r.x, it.r.y, it.text, it.focused ? hot : base);
  }
}

PanelHitResult Panel::hit(int x, int y) const {
  if (!rect_.contains(x, y)) return PanelHitResult{};
  for (const auto& it : items_) {
    if (it.r.contains(x, y)) {
      return PanelHitResult{it.what, it.index, it.win};
    }
  }
  // Toute cellule du panneau appartient au panneau. Body est une réponse,
  // pas un échec : c'est ce qui empêche un clic de traverser vers le bureau.
  return PanelHitResult{PanelHit::Body, -1, 0};
}

}  // namespace sshos
