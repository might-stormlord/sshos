#include "shell/menu.hpp"

#include <algorithm>

#include "app/catalog.hpp"

namespace sshos {
namespace {

constexpr int kMenuWidth = 34;
// Une ligne de cadre en haut, la ligne de saisie, un séparateur, puis les
// entrées, puis la ligne de cadre du bas.
constexpr int kChrome = 4;

char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_fold(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

}  // namespace

void Menu::open() {
  open_ = true;
  anchored_ = false;
  query_.clear();
  sel_ = 0;

  all_.clear();
  for (const auto& e : catalog()) {
    all_.push_back({"app:" + e.id, e.label});
  }
  all_.push_back({"panel:top", "Panneau : haut"});
  all_.push_back({"panel:bottom", "Panneau : bas"});
  all_.push_back({"panel:left", "Panneau : gauche"});
  all_.push_back({"panel:right", "Panneau : droite"});
  all_.push_back({"cmd:beat", "Battement : battre"});
  all_.push_back({"cmd:cut", "Battement : couper la source"});
  all_.push_back({"session:quit", "Quitter la session"});
  refilter();
}

void Menu::open_at(int x, int y) {
  // Passe par open() pour que la TABLE reste construite en un seul endroit :
  // une entrée ajoutée au menu du panneau doit apparaître au clic droit
  // sans que personne ait à y penser.
  open();
  anchored_ = true;
  anchor_x_ = x;
  anchor_y_ = y;
}

void Menu::close() {
  open_ = false;
  query_.clear();
  shown_.clear();
  sel_ = 0;
}

void Menu::refilter() {
  shown_.clear();
  for (const auto& it : all_) {
    if (contains_fold(it.label, query_)) shown_.push_back(it);
  }
  // La sélection reste dans les bornes quoi qu'il arrive : filtrer sous les
  // pieds de l'utilisateur ne doit jamais désigner une entrée qui n'existe
  // plus.
  sel_ = std::max(0, std::min(sel_, static_cast<int>(shown_.size()) - 1));
}

void Menu::type(char32_t c) {
  if (c < 32 || c > 126) return;  // le filtre est ASCII, comme les libellés
  query_.push_back(static_cast<char>(c));
  refilter();
}

void Menu::backspace() {
  if (query_.empty()) return;
  query_.pop_back();
  refilter();
}

void Menu::move(int delta) {
  if (shown_.empty()) return;
  const int n = static_cast<int>(shown_.size());
  // En boucle : descendre depuis la dernière entrée revient à la première.
  sel_ = ((sel_ + delta) % n + n) % n;
}

const MenuItem* Menu::selected() const {
  if (!open_ || shown_.empty()) return nullptr;
  if (sel_ < 0 || sel_ >= static_cast<int>(shown_.size())) return nullptr;
  return &shown_[static_cast<size_t>(sel_)];
}

Rect Menu::rect(int cols, int rows) const {
  const int want = static_cast<int>(shown_.empty() ? all_.size() : shown_.size());
  int h = std::min(rows - 1, want + kChrome);
  h = std::max(h, kChrome + 1);
  const int w = std::min(cols, kMenuWidth);

  if (anchored_) {
    // Le coin haut-gauche AU CURSEUR, puis ramené dans l'écran. Un menu
    // contextuel qui déborde par la droite est le défaut le plus banal du
    // genre, et le seul que personne ne pardonne : la moitié des entrées
    // devient illisible.
    int x = anchor_x_;
    int y = anchor_y_;
    if (x + w > cols) x = cols - w;
    if (y + h > rows) y = rows - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    return Rect{x, y, w, h};
  }

  // Ancré au bouton de menu du panneau, et poussé vers le haut : le menu
  // pousse depuis son bouton, il ne le recouvre pas.
  int y = rows - 1 - h;
  if (y < 0) y = 0;
  return Rect{0, y, w, h};
}

void Menu::layout(int cols, int rows) { rect_ = rect(cols, rows); }

void Menu::draw(View v, const Theme& th, Border b) const {
  if (!open_) return;

  Style st;
  st.bg = th.modal_bg;
  st.fg = th.modal_fg;
  v.fill(rect_, st);
  v.box(rect_, b, st);

  const int inner_x = rect_.x + 1;
  const int inner_w = rect_.w - 2;
  if (inner_w <= 0) return;

  Style sel = st;
  sel.bg = th.accent;
  sel.fg = th.modal_bg;

  v.text(inner_x, rect_.y + 1, "> " + query_, st);
  for (int i = 0; i < inner_w; ++i) {
    v.put(inner_x + i, rect_.y + 2, U'-', st);
  }

  const int rows_for_items = rect_.h - kChrome;
  // La fenêtre de défilement suit la sélection : elle ne peut jamais sortir
  // de l'écran, quel que soit le nombre d'entrées.
  int first = 0;
  if (sel_ >= rows_for_items) first = sel_ - rows_for_items + 1;
  for (int i = 0; i < rows_for_items; ++i) {
    const int idx = first + i;
    if (idx >= static_cast<int>(shown_.size())) break;
    v.text(inner_x, rect_.y + 3 + i, shown_[static_cast<size_t>(idx)].label,
           idx == sel_ ? sel : st);
  }
}

MenuHitResult Menu::hit(int x, int y) const {
  if (!open_ || !rect_.contains(x, y)) return MenuHitResult{};
  if (y == rect_.y + 1) return MenuHitResult{MenuHit::Search, -1};

  const int rows_for_items = rect_.h - kChrome;
  int first = 0;
  if (sel_ >= rows_for_items) first = sel_ - rows_for_items + 1;
  const int row = y - (rect_.y + 3);
  if (row >= 0 && row < rows_for_items) {
    const int idx = first + row;
    if (idx < static_cast<int>(shown_.size())) {
      return MenuHitResult{MenuHit::Item, idx};
    }
  }
  return MenuHitResult{MenuHit::Body, -1};
}

}  // namespace sshos
