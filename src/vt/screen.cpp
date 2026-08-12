#include "vt/screen.hpp"

#include <algorithm>

#include "render/width.hpp"

namespace sshos {
namespace {

constexpr int kTabStop = 8;

// L'encodage UTF-8 d'un point de code, pour line_text(). Écrit à la main
// plutôt qu'emprunté au rendu : l'écran ne doit rien devoir à la couche
// d'affichage, c'est ce qui permet de le tester seul.
void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

Screen::Screen(int cols, int rows)
    : cols_(std::max(1, cols)), rows_(std::max(1, rows)) {
  grid_.assign(static_cast<size_t>(cols_) * static_cast<size_t>(rows_), ScreenCell{});
  reset_tabs();
}

void Screen::reset_tabs() {
  tabs_.assign(static_cast<size_t>(cols_), false);
  // La colonne 0 ne reçoit PAS de taquet, et la différence n'est pas
  // observable aujourd'hui : tab() ne cherche qu'en avant, depuis cx_ + 1,
  // donc un taquet en 0 ne serait jamais atteint. Mutation équivalente,
  // volontairement non couverte. Elle redeviendra porteuse le jour où un
  // retour arrière de tabulation (CBT, CSI Z) lira les taquets vers la
  // gauche -- il butera alors sur la colonne 0, taquet ou pas.
  for (int x = kTabStop; x < cols_; x += kTabStop) {
    tabs_[static_cast<size_t>(x)] = true;
  }
}

ScreenCell& Screen::cell(int x, int y) {
  return grid_[static_cast<size_t>(y) * static_cast<size_t>(cols_) +
               static_cast<size_t>(x)];
}

const ScreenCell& Screen::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return blank_;
  return grid_[static_cast<size_t>(y) * static_cast<size_t>(cols_) +
               static_cast<size_t>(x)];
}

void Screen::clear_wide_at(int x, int y) {
  const ScreenCell& c = at(x, y);
  if (c.width == 2 && x + 1 < cols_) {
    cell(x + 1, y) = ScreenCell{};
  } else if (c.width == 0 && x > 0) {
    cell(x - 1, y) = ScreenCell{};
  }
}

void Screen::scroll_up() {
  // Rotation plutôt que recopie ligne à ligne : une seule passe, et la
  // ligne recyclée est celle qui sortait de l'écran.
  std::rotate(grid_.begin(), grid_.begin() + cols_, grid_.end());
  std::fill(grid_.end() - cols_, grid_.end(), ScreenCell{});
}

void Screen::scroll_down() {
  std::rotate(grid_.begin(), grid_.end() - cols_, grid_.end());
  std::fill(grid_.begin(), grid_.begin() + cols_, ScreenCell{});
}

void Screen::print(char32_t cp) {
  const int w = std::max(0, char_width(cp));
  if (w == 0) return;  // combinant ou non imprimable : tâche ultérieure

  // Le retour DIFFÉRÉ se consomme ICI, à l'arrivée du caractère suivant, et
  // pas au moment où la dernière colonne a été écrite. C'est toute la
  // différence entre une ligne de largeur pleine qui s'affiche droite et
  // une qui saute une ligne sur deux.
  if (wrap_pending_) {
    wrap_pending_ = false;
    cx_ = 0;
    line_feed();
  }

  // Un caractère pleine chasse ne se coupe jamais en deux : s'il ne tient
  // plus, il descend ENTIER.
  if (w == 2 && cx_ + 1 >= cols_) {
    cx_ = 0;
    line_feed();
  }

  // Les deux effacements AVANT la moindre écriture. Les faire après
  // reviendrait à demander à clear_wide_at() de distinguer la cellule
  // qu'on vient de poser de celle qui était là : elle ne le peut pas, et
  // effacerait le caractère neuf en croyant nettoyer un orphelin.
  clear_wide_at(cx_, cy_);
  if (w == 2 && cx_ + 1 < cols_) clear_wide_at(cx_ + 1, cy_);

  ScreenCell& c = cell(cx_, cy_);
  c.ch = cp;
  c.width = static_cast<uint8_t>(w);
  if (w == 2 && cx_ + 1 < cols_) {
    ScreenCell& tail = cell(cx_ + 1, cy_);
    tail.ch = U' ';
    tail.width = 0;
  }

  cx_ += w;
  if (cx_ >= cols_) {
    // On RESTE sur la dernière colonne : le curseur ne sort pas de la
    // grille, et le drapeau se souvient qu'il y a un retour en attente.
    cx_ = cols_ - 1;
    wrap_pending_ = true;
  }
}

void Screen::line_feed() {
  wrap_pending_ = false;
  if (cy_ + 1 >= rows_) {
    scroll_up();
  } else {
    ++cy_;
  }
}

void Screen::carriage_return() {
  wrap_pending_ = false;
  cx_ = 0;
}

void Screen::backspace() {
  // Un CUB reçu entre l'écriture de la dernière colonne et le caractère
  // suivant lève le drapeau SANS avoir retourné, et sans bouger : le
  // curseur est déjà sur la dernière colonne, c'est là qu'il doit rester.
  if (wrap_pending_) {
    wrap_pending_ = false;
    return;
  }
  if (cx_ > 0) --cx_;
}

void Screen::tab() {
  wrap_pending_ = false;
  for (int x = cx_ + 1; x < cols_; ++x) {
    if (tabs_[static_cast<size_t>(x)]) {
      cx_ = x;
      return;
    }
  }
  // Aucun taquet devant : on s'arrête à la dernière colonne, on ne
  // retourne pas.
  cx_ = cols_ - 1;
}

void Screen::index() { line_feed(); }

void Screen::reverse_index() {
  wrap_pending_ = false;
  if (cy_ == 0) {
    scroll_down();
  } else {
    --cy_;
  }
}

void Screen::next_line() {
  carriage_return();
  line_feed();
}

void Screen::move_to(int x, int y) {
  wrap_pending_ = false;
  cx_ = std::clamp(x, 0, cols_ - 1);
  cy_ = std::clamp(y, 0, rows_ - 1);
}

void Screen::move_up(int n) { move_to(cx_, cy_ - std::max(1, n)); }
void Screen::move_down(int n) { move_to(cx_, cy_ + std::max(1, n)); }
void Screen::move_right(int n) { move_to(cx_ + std::max(1, n), cy_); }

void Screen::move_left(int n) {
  // CUB se comporte comme BS face à un retour en attente : le curseur est
  // logiquement UNE cellule au-delà de la dernière colonne, donc le premier
  // pas vers la gauche ne fait que le ramener dessus. Les suivants
  // reculent pour de bon.
  int steps = std::max(1, n);
  if (wrap_pending_) {
    wrap_pending_ = false;
    --steps;
  }
  move_to(cx_ - steps, cy_);
}
void Screen::set_column(int x) { move_to(x, cy_); }
void Screen::set_row(int y) { move_to(cx_, y); }

void Screen::set_tab() {
  if (cx_ >= 0 && cx_ < cols_) tabs_[static_cast<size_t>(cx_)] = true;
}

void Screen::clear_tab() {
  if (cx_ >= 0 && cx_ < cols_) tabs_[static_cast<size_t>(cx_)] = false;
}

void Screen::clear_all_tabs() {
  std::fill(tabs_.begin(), tabs_.end(), false);
}

std::string Screen::line_text(int y) const {
  std::string out;
  if (y < 0 || y >= rows_) return out;
  // Rognée : le scrollback garde 10 000 lignes, et 300 colonnes de blancs
  // par ligne coûteraient pour rien.
  int last = -1;
  for (int x = 0; x < cols_; ++x) {
    const ScreenCell& c = at(x, y);
    if (c.width != 0 && c.ch != U' ') last = x;
  }
  for (int x = 0; x <= last; ++x) {
    const ScreenCell& c = at(x, y);
    if (c.width == 0) continue;  // seconde moitié d'une pleine chasse
    append_utf8(out, c.ch);
  }
  return out;
}

}  // namespace sshos
