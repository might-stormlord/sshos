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
  bottom_ = rows_ - 1;
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

ScreenCell Screen::erased() const {
  ScreenCell c;
  c.style.bg = pen_.bg;
  return c;
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
    cell(x + 1, y) = erased();
  } else if (c.width == 0 && x > 0) {
    cell(x - 1, y) = erased();
  }
}

void Screen::scroll_up() { scroll_slice_up(top_, bottom_, 1); }

void Screen::scroll_down() { scroll_slice_down(top_, bottom_, 1); }

// Rotation plutôt que recopie ligne à ligne : une seule passe, et les
// lignes recyclées sont exactement celles qui sortaient de la tranche.
//
// Les deux gardes sur `n` sont écrites larges -- `n <= 0` et `n >= height`
// -- alors que seul leur bord extérieur est porteur : ce sont `n < 0` et
// `n > height` qui feraient sortir `cut` de [first, last] et partir
// std::rotate hors des bornes. Pour n == 0 et n == height exactement, le
// chemin général retomberait sur ses pieds, d'où deux mutations
// équivalentes, volontairement non couvertes. Elles restent écrites ainsi
// pour que le lecteur n'ait pas à refaire cette vérification.
void Screen::scroll_slice_up(int top, int bottom, int n) {
  if (n <= 0 || top < 0 || bottom >= rows_ || top > bottom) return;
  const int height = bottom - top + 1;
  const auto first = grid_.begin() + static_cast<std::ptrdiff_t>(top) * cols_;
  const auto last =
      grid_.begin() + static_cast<std::ptrdiff_t>(bottom + 1) * cols_;
  if (n >= height) {
    // Tout sort : inutile de faire tourner ce qui va disparaître.
    std::fill(first, last, erased());
    return;
  }
  const auto cut = first + static_cast<std::ptrdiff_t>(n) * cols_;
  std::rotate(first, cut, last);
  std::fill(last - static_cast<std::ptrdiff_t>(n) * cols_, last, erased());
}

void Screen::scroll_slice_down(int top, int bottom, int n) {
  if (n <= 0 || top < 0 || bottom >= rows_ || top > bottom) return;
  const int height = bottom - top + 1;
  const auto first = grid_.begin() + static_cast<std::ptrdiff_t>(top) * cols_;
  const auto last =
      grid_.begin() + static_cast<std::ptrdiff_t>(bottom + 1) * cols_;
  if (n >= height) {
    std::fill(first, last, erased());
    return;
  }
  const auto cut = last - static_cast<std::ptrdiff_t>(n) * cols_;
  std::rotate(first, cut, last);
  std::fill(first, first + static_cast<std::ptrdiff_t>(n) * cols_,
            erased());
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
  c.style = pen_;
  if (w == 2 && cx_ + 1 < cols_) {
    // La moitié droite porte LE MÊME style que la gauche : le rendu la
    // peint comme n'importe quelle cellule, et un fond qui s'arrêterait au
    // milieu de l'idéogramme se verrait.
    ScreenCell& tail = cell(cx_ + 1, cy_);
    tail.ch = U' ';
    tail.width = 0;
    tail.style = pen_;
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
  if (cy_ == bottom_) {
    // Au bas de la région : c'est la région qui tourne, pas l'écran.
    scroll_up();
  } else if (cy_ + 1 < rows_) {
    // Sous la région, on descend jusqu'au bas de l'écran sans rien faire
    // défiler -- une application qui laisse son curseur sous la région n'a
    // pas demandé à ce que la page bouge.
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
  if (cy_ == top_) {
    scroll_down();
  } else if (cy_ > 0) {
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

// ---------------------------------------------------------------------------
// Les effacements et les éditions.
// ---------------------------------------------------------------------------

void Screen::erase_span(int x0, int x1, int y) {
  if (y < 0 || y >= rows_) return;
  // La borne gauche est DÉFENSIVE et non observable : aucun appelant ne
  // passe un x0 négatif (tous partent de 0, de cx_, ou de cols_ - n avec n
  // borné). Elle reste parce qu'elle sépare une erreur d'appelant d'un
  // accès hors grille, pour un coût nul. La borne droite, elle, est bel et
  // bien porteuse : c'est elle qui absorbe les comptes trop grands d'ECH.
  x0 = std::max(0, x0);
  x1 = std::min(cols_ - 1, x1);
  if (x0 > x1) return;
  // Les deux bornes peuvent tomber au milieu d'un caractère pleine chasse.
  // On emporte d'abord sa moitié restée dehors, sinon le rendu peint un
  // demi idéogramme collé au bord de la plage effacée.
  clear_wide_at(x0, y);
  clear_wide_at(x1, y);
  for (int x = x0; x <= x1; ++x) cell(x, y) = erased();
}

void Screen::erase_display(int mode) {
  if (mode == 0) {
    erase_span(cx_, cols_ - 1, cy_);
    for (int y = cy_ + 1; y < rows_; ++y) erase_span(0, cols_ - 1, y);
  } else if (mode == 1) {
    for (int y = 0; y < cy_; ++y) erase_span(0, cols_ - 1, y);
    erase_span(0, cx_, cy_);
  } else if (mode == 2) {
    for (int y = 0; y < rows_; ++y) erase_span(0, cols_ - 1, y);
  }
  // Le mode 3 vide le scrollback : il ne concerne pas la grille, et sera
  // traité au-dessus d'elle à la tâche 7.
}

void Screen::erase_line(int mode) {
  if (mode == 0) {
    erase_span(cx_, cols_ - 1, cy_);
  } else if (mode == 1) {
    erase_span(0, cx_, cy_);
  } else if (mode == 2) {
    erase_span(0, cols_ - 1, cy_);
  }
}

void Screen::erase_chars(int n) {
  // Pas de garde sur n : un compte nul ou négatif donne une plage vide ou
  // renversée, qu'erase_span refuse déjà. La re-tester ici ne discrimine
  // rien -- la mutation qui la relâchait survivait à toute la suite.
  erase_span(cx_, cx_ + n - 1, cy_);
}

void Screen::break_wide_at(int x, int y) {
  const ScreenCell& c = at(x, y);
  if (c.width == 2 && x + 1 < cols_) {
    cell(x, y) = erased();
    cell(x + 1, y) = erased();
  } else if (c.width == 0 && x > 0) {
    cell(x, y) = erased();
    cell(x - 1, y) = erased();
  }
}

void Screen::insert_chars(int n) {
  if (n <= 0 || cx_ >= cols_) return;
  const int room = cols_ - cx_;
  // Ce bornage-ci ne change PAS le résultat : erase_span borne déjà sa
  // plage et la boucle de décalage ne tourne pas pour un n trop grand. Il
  // n'est là que contre le débordement de `cx_ + n`, qui serait un
  // comportement indéfini et non un grand nombre. Le bornage de
  // delete_chars, lui, est porteur -- `cols_ - n` y devient négatif et
  // emporterait la ligne entière au lieu de sa seule queue.
  n = std::min(n, room);
  // Le curseur peut être posé sur la seconde moitié d'une pleine chasse :
  // décaler cette moitié sans sa jumelle ferait voyager un demi caractère.
  break_wide_at(cx_, cy_);
  // Ce qui sort de la ligne par la droite est perdu : il n'y a pas de
  // débordement d'une ligne sur la suivante, une édition reste chez elle.
  // Le décalage se fait de la droite vers la gauche pour ne pas écraser sa
  // propre source.
  for (int x = cols_ - 1; x >= cx_ + n; --x) cell(x, cy_) = at(x - n, cy_);
  erase_span(cx_, cx_ + n - 1, cy_);
  // Le décalage a pu pousser la première moitié d'une pleine chasse contre
  // le bord droit, sa seconde étant tombée de la ligne.
  if (at(cols_ - 1, cy_).width == 2) cell(cols_ - 1, cy_) = erased();
}

void Screen::delete_chars(int n) {
  if (n <= 0 || cx_ >= cols_) return;
  const int room = cols_ - cx_;
  n = std::min(n, room);
  break_wide_at(cx_, cy_);
  // La coupe elle-même peut tomber au milieu d'une pleine chasse : sa
  // première moitié part avec ce qu'on supprime, sa seconde survivrait.
  break_wide_at(cx_ + n, cy_);
  for (int x = cx_; x + n < cols_; ++x) cell(x, cy_) = at(x + n, cy_);
  erase_span(cols_ - n, cols_ - 1, cy_);
}

void Screen::insert_lines(int n) {
  // Hors région, IL ne fait rien : c'est la règle qui empêche une
  // application de pousser des lignes dans une zone qu'elle a elle-même
  // déclarée fixe.
  if (n <= 0 || cy_ < top_ || cy_ > bottom_) return;
  scroll_slice_down(cy_, bottom_, n);
}

void Screen::delete_lines(int n) {
  if (n <= 0 || cy_ < top_ || cy_ > bottom_) return;
  scroll_slice_up(cy_, bottom_, n);
}

// ---------------------------------------------------------------------------
// La région de défilement et le curseur sauvé.
// ---------------------------------------------------------------------------

void Screen::set_scroll_region(int top, int bottom) {
  top = std::max(0, top);
  bottom = std::min(rows_ - 1, bottom);
  // Une région de moins de deux lignes ne peut pas défiler : on la refuse
  // au lieu de la subir, et la région précédente reste en place.
  if (bottom - top < 1) return;
  top_ = top;
  bottom_ = bottom;
  // DECSTBM ramène le curseur à l'origine de l'écran.
  cx_ = 0;
  cy_ = 0;
  wrap_pending_ = false;
}

void Screen::reset_scroll_region() {
  top_ = 0;
  bottom_ = rows_ - 1;
  cx_ = 0;
  cy_ = 0;
  wrap_pending_ = false;
}

void Screen::save_cursor() {
  saved_ = SavedCursor{cx_, cy_, wrap_pending_, pen_};
}

void Screen::restore_cursor() {
  // Bornée : entre la sauvegarde et la reprise, l'écran a pu rétrécir.
  cx_ = std::clamp(saved_.x, 0, cols_ - 1);
  cy_ = std::clamp(saved_.y, 0, rows_ - 1);
  wrap_pending_ = saved_.wrap_pending && cx_ == cols_ - 1;
  // Le style repart avec le curseur : une application qui sauve au milieu
  // d'un passage en gras attend de le retrouver en gras. Sans DECSC
  // préalable, `saved_` est vierge -- et un DECRC seul rend donc un
  // terminal fraîchement allumé, curseur à l'origine et stylo neuf.
  pen_ = saved_.style;
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
