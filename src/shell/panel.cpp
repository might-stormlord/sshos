#include "shell/panel.hpp"

#include <algorithm>

#include "app/catalog.hpp"
#include "common/utf8.hpp"
#include "render/width.hpp"

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

// La coupure elle-même vit dans render/width.hpp : l'aide en a besoin aussi,
// pour la même raison et avec les mêmes pièges de largeur.
std::string elide(const std::string& s, int cells, bool utf8) {
  return elide_to_cells(s, cells, utf8 ? "…" : "~");
}

}  // namespace

// Une entrée par application du catalogue, dans l'ordre du catalogue, plus
// une par fenêtre qui ne vient d'aucune d'elles. L'ordre du catalogue est
// FIXE : c'est ce qui fait qu'une entrée ne saute pas d'un bout à l'autre de
// la barre au moment où on lance l'application, et qu'on peut viser la même
// cellule deux fois de suite.
std::vector<PanelEntry> Panel::build_entries(const WindowManager& wm) const {
  const auto& stack = wm.stack();
  const WindowId cur = wm.focused();
  std::vector<PanelEntry> out;

  // Remplit une entrée à partir des fenêtres que `belongs` retient. La pile
  // va du fond vers le dessus (WindowManager::raise fait tourner vers la
  // fin), donc la dernière retenue est celle du dessus.
  const auto gather = [&](PanelEntry& e, auto belongs) {
    std::vector<WindowId> ids;
    int at = -1;
    bool all_min = true;
    const Window* last = nullptr;
    for (const auto& up : stack) {
      const Window& w = *up;
      if (!belongs(w)) continue;
      if (w.id == cur) at = static_cast<int>(ids.size());
      if (w.mode != WinMode::Minimized) all_min = false;
      ids.push_back(w.id);
      last = &w;
    }
    e.count = static_cast<int>(ids.size());
    if (e.count == 0) return;
    e.focused = at >= 0;
    e.minimized = all_min;
    // Cliquer l'entrée d'un groupe déjà actif passe à la fenêtre suivante du
    // groupe ; un groupe d'une seule fenêtre se redésigne donc lui-même, ce
    // qui laisse Session appliquer sa règle « la réactive se réduit ».
    e.target = at >= 0 ? ids[static_cast<size_t>((at + 1) % e.count)] : ids.back();
    // Une seule fenêtre : son titre vaut mieux que le libellé du catalogue,
    // il porte l'état de l'application (« Bloc * » quand elle est modifiée).
    if (e.count == 1 && last != nullptr && !last->title.empty()) e.label = last->title;
  };

  int ci = 0;
  for (const auto& c : catalog()) {
    PanelEntry e;
    e.catalog_index = ci++;
    e.label = c.label;
    gather(e, [&](const Window& w) { return w.app_id == c.id; });
    out.push_back(std::move(e));
  }

  // Puis ce que le catalogue ne couvre pas : une fenêtre ouverte autrement
  // (ou sans identifiant d'application) garde son entrée à elle, à la suite.
  // Sans cette boucle elle disparaîtrait purement et simplement de la barre.
  for (const auto& up : stack) {
    const Window& w = *up;
    bool known = false;
    for (const auto& c : catalog()) known = known || w.app_id == c.id;
    if (known) continue;
    PanelEntry e;
    e.label = w.title;
    gather(e, [&](const Window& other) { return other.id == w.id; });
    out.push_back(std::move(e));
  }
  return out;
}

// La marque dit l'état en une cellule : pleine pour l'active, creuse pour
// l'ouverte en arrière-plan, soulignée pour la réduite, vide pour ce qui
// n'est pas lancé. Le compteur n'apparaît qu'à partir de deux fenêtres --
// écrire « 1 » partout ne renseigne personne et coûte deux cellules.
std::string Panel::entry_text(const PanelEntry& e, int label_cells) const {
  std::string mark = " ";
  if (e.count > 0) {
    // Réduite AVANT active : une fenêtre réduite garde la main dans le
    // gestionnaire, et des deux états c'est le sien qui renseigne -- montrer
    // « active » d'une fenêtre qu'on ne voit nulle part à l'écran serait un
    // mensonge de barre des tâches.
    if (e.minimized) {
      mark = "_";
    } else if (e.focused) {
      mark = utf8_ ? "●" : "*";
    } else {
      mark = utf8_ ? "○" : "o";
    }
  }
  std::string t = mark + elide(e.label, label_cells, utf8_);
  // Parenthèses dans les deux profils : « Bloc(2) » se lit comme un compte
  // sous n'importe quel jeu de caractères, là où un « Blocx2 » se lit comme
  // un nom.
  if (e.count > 1) t += "(" + std::to_string(e.count) + ")";
  return t;
}

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
  const int menu_w = text_cells(menu);
  items_.push_back(
      {PanelHit::MenuButton, -1, 0, false, Rect{x, y, menu_w, 1}, menu});
  x += menu_w + 1;

  const std::vector<PanelEntry> entries = build_entries(wm);
  const int total = static_cast<int>(entries.size());
  const int start_x = x;

  // Combien d'entrées tiennent avant `end`. Appelée deux fois : une fois
  // sans réserve, puis, si tout ne tient pas, avec la place du compteur de
  // repli mise de côté.
  const auto fits = [&](int end) {
    int cx = start_x;
    int n = 0;
    for (const auto& e : entries) {
      const int w = text_cells(entry_text(e, kLabelCells));
      if (cx + w > end) break;
      cx += w + 1;
      ++n;
    }
    return n;
  };

  // Le rappel du leader se réserve sa place AVANT les tâches, et seulement
  // s'il ne coûte aucune entrée : on mesure la barre en le supposant là, et
  // on ne le garde que si tout tient encore. Une barre pleine appartient à
  // quelqu'un qui n'a plus besoin qu'on lui rappelle la touche.
  //
  // La garde décide donc à elle seule : quand elle passe, toutes les entrées
  // tiennent déjà dans l'espace réduit, et `end` réduit ou non donne le même
  // dessin. Une mutation qui oublierait la réduction serait indiscernable --
  // c'est voulu, pas un trou : la place ne peut pas manquer là où l'on vient
  // de vérifier qu'elle ne manque pas.
  const int hint_w = hint_.empty() ? 0 : text_cells(hint_);
  const bool show_hint = hint_w > 0 && fits(clock_x - 1 - hint_w - 1) == total;
  int end = show_hint ? clock_x - 1 - hint_w - 1 : clock_x - 1;

  int shown = fits(end);
  if (shown < total) {
    // Les libellés sont déjà à leur minimum : ce qui dépasse se replie sur
    // un compteur, plutôt que de disparaître sans le dire. Le rappel a
    // forcément cédé la place à ce stade -- sa garde exige que TOUT tienne.
    const int over_w = 1 + static_cast<int>(std::to_string(total).size());
    end = clock_x - 1 - over_w - 1;
    shown = fits(end);
  }

  for (int i = 0; i < shown; ++i) {
    const PanelEntry& e = entries[static_cast<size_t>(i)];
    const std::string t = entry_text(e, kLabelCells);
    const int cw = text_cells(t);
    // Rien d'ouvert : le clic doit LANCER, et Session a besoin du rang au
    // catalogue pour savoir quoi. Dès qu'une fenêtre existe, c'est elle que
    // le clic vise, et le rang cède la place à son identifiant.
    const bool live = e.count > 0;
    items_.push_back({live ? PanelHit::Task : PanelHit::Pinned,
                      live ? i : e.catalog_index, e.target, e.focused,
                      Rect{x, y, cw, 1}, t});
    x += cw + 1;
  }

  const int hidden = total - shown;
  if (hidden > 0) {
    const std::string t = (utf8_ ? "»" : ">") + std::to_string(hidden);
    const int w = text_cells(t);
    items_.push_back({PanelHit::Overflow, hidden, 0, false,
                      Rect{clock_x - 1 - w, y, w, 1}, t});
  }

  if (show_hint) {
    items_.push_back({PanelHit::Hint, -1, 0, false,
                      Rect{clock_x - 1 - hint_w, y, hint_w, 1}, hint_});
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
  const std::vector<PanelEntry> entries = build_entries(wm);
  const int total = static_cast<int>(entries.size());
  int room = std::max(0, bottom - clock_h - y);

  // Même règle qu'à l'horizontale, en lignes plutôt qu'en colonnes : le
  // rappel prend la ligne juste au-dessus de l'horloge tant qu'il ne coûte
  // aucune entrée.
  const int hint_w = hint_.empty() ? 0 : text_cells(hint_);
  const bool show_hint = hint_w > 0 && hint_w <= w && total <= room - 1;
  if (show_hint) --room;

  const bool folds = total > room;
  if (folds) room = std::max(0, room - 1);  // une ligne pour le compteur

  const int shown = std::min(total, room);
  for (int i = 0; i < shown; ++i) {
    const PanelEntry& e = entries[static_cast<size_t>(i)];
    const bool live = e.count > 0;
    items_.push_back({live ? PanelHit::Task : PanelHit::Pinned,
                      live ? i : e.catalog_index, e.target, e.focused,
                      Rect{x, y, w, 1}, entry_text(e, label_cells)});
    ++y;
  }

  const int hidden = total - shown;
  if (hidden > 0) {
    const std::string t = (utf8_ ? "»" : ">") + std::to_string(hidden);
    items_.push_back({PanelHit::Overflow, hidden, 0, false, Rect{x, y, w, 1}, t});
  }

  if (show_hint) {
    items_.push_back({PanelHit::Hint, -1, 0, false,
                      Rect{x, bottom - clock_h - 1, w, 1}, hint_});
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
        const int pad = std::max(0, kClockCells - text_cells(clock_text));
        v.text(it.r.x + pad, it.r.y, clock_text, base);
      }
      continue;
    }
    // Le rappel porte l'accent sans être une tâche : il doit se remarquer
    // d'un bureau vide, où il est la seule chose à lire, sans se disputer la
    // lecture avec la fenêtre active quand la barre se remplit.
    const bool highlit = it.focused || it.what == PanelHit::Hint;
    v.text(it.r.x, it.r.y, it.text, highlit ? hot : base);
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
