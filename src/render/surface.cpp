#include "render/surface.hpp"

#include <algorithm>

#include "common/utf8.hpp"
#include "render/profile.hpp"
#include "render/width.hpp"

namespace sshos {

Surface::Surface(int w, int h) { resize(w, h); }

void Surface::resize(int w, int h) {
  w_ = std::max(0, w);
  h_ = std::max(0, h);
  cells_.assign(static_cast<size_t>(w_) * h_, Cell{});
}

void Surface::clear(Style s) {
  Cell c;
  c.fg = s.fg;
  c.bg = s.bg;
  c.attrs = s.attrs;
  std::fill(cells_.begin(), cells_.end(), c);
}

View Surface::root() { return View(*this, Rect{0, 0, w_, h_}); }

bool View::map(int x, int y, int& ox, int& oy) const {
  if (x < 0 || y < 0 || x >= clip_.w || y >= clip_.h) return false;
  ox = clip_.x + x;
  oy = clip_.y + y;
  return ox >= 0 && oy >= 0 && ox < s_->w() && oy < s_->h();
}

void View::cleanup_orphan(int ox, int oy) {
  // Quand une écriture détruit une moitié de paire large, l'autre moitié
  // devient orpheline. Cette méthode maintient l'invariante de la grille :
  // pas de continuation sans tête, pas de tête sans continuation.
  //
  // Sémantique retenue : nettoyer toujours, même hors du clip.
  // Raison : le diffeur du rendu (tâche 6) remonte d'une continuation vers
  // sa tête pour réémettre la paire. Une paire à moitié écrasée produit une
  // sortie ANSI incorrecte. Maintenir la cohérence globale de la grille est
  // prioritaire sur le clip d'une View, car la grille est partagée.

  // Si la cellule à gauche est la tête d'un glyphe large de 2 colonnes,
  // notre écriture détruit sa continuation. Blanch la tête pour rétablir
  // l'invariante.
  if (ox > 0) {
    const Cell& left = s_->at(ox - 1, oy);
    if (left.width == 2) {
      Cell& orphan = s_->at(ox - 1, oy);
      orphan = Cell{};
    }
  }

  // Si la cellule à droite est une continuation, notre écriture détruit sa
  // tête. Blanchis la continuation orpheline.
  if (ox + 1 < s_->w()) {
    const Cell& right = s_->at(ox + 1, oy);
    if (right.width == 0) {  // width==0 signifie continuation
      Cell& orphan = s_->at(ox + 1, oy);
      orphan = Cell{};
    }
  }
}


void View::put(int x, int y, char32_t ch, Style st) {
  int ox = 0;
  int oy = 0;
  if (!map(x, y, ox, oy)) return;

  const int cw = char_width(ch);
  if (cw == 0) return;
  // Règle 2 du §4.1 : jamais de glyphe large en dernière colonne.
  if (cw == 2 && (x + 1 >= clip_.w || ox + 1 >= s_->w())) return;

  cleanup_orphan(ox, oy);
  // Quand cw==2, on écrit sur deux colonnes : ox et ox+1. Il faut nettoyer
  // les orphelins autour de ox+1 aussi, sinon une paire à droite reste
  // orpheline. Exemple : put(2,0,'日') crée col2=head, col3=continuation.
  // Puis put(1,0,'中') : cleanup(1,0) ne voit pas col3. On écrit col1=head,
  // col2=continuation, orphelinage col3 sans garder la tête valide.
  if (cw == 2) {
    cleanup_orphan(ox + 1, oy);
  }

  Cell& c = s_->at(ox, oy);
  c.ch = ch;
  c.fg = st.fg;
  c.bg = st.bg;
  c.attrs = st.attrs;
  c.width = static_cast<uint8_t>(cw);

  if (cw == 2) {
    Cell& cont = s_->at(ox + 1, oy);
    cont = kContinuation;
    cont.bg = st.bg;
  }
}

int View::text(int x, int y, std::string_view utf8, Style st) {
  int col = x;
  size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = 0;
    i += utf8_decode(utf8, i, cp);
    const int cw = char_width(cp);
    if (cw == 0) continue;
    if (col + cw > clip_.w) break;
    put(col, y, cp, st);
    col += cw;
  }
  return col - x;
}

void View::fill(Rect r, Style st) {
  const int x0 = std::max(0, r.x);
  const int y0 = std::max(0, r.y);
  const int x1 = std::min(clip_.w, r.x + r.w);
  const int y1 = std::min(clip_.h, r.y + r.h);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      int ox = 0;
      int oy = 0;
      if (!map(x, y, ox, oy)) continue;
      cleanup_orphan(ox, oy);
      Cell& c = s_->at(ox, oy);
      c = Cell{};
      c.fg = st.fg;
      c.bg = st.bg;
      c.attrs = st.attrs;
    }
  }
}

void View::box(Rect r, Border b, Style st) {
  if (r.w <= 0 || r.h <= 0) return;

  const bool uni = (b == Border::Unicode);
  const char32_t hor = uni ? U'─' : U'-';
  const char32_t ver = uni ? U'│' : U'|';

  // Les cas dégénérés ne sont pas des erreurs : un cadre d'une ligne ou
  // d'une colonne arrive naturellement quand une fenêtre est réduite au
  // plancher, et planter là serait un défaut de robustesse, pas une
  // protection.
  if (r.h == 1) {
    for (int x = 0; x < r.w; ++x) put(r.x + x, r.y, hor, st);
    return;
  }
  if (r.w == 1) {
    for (int y = 0; y < r.h; ++y) put(r.x, r.y + y, ver, st);
    return;
  }

  for (int x = 1; x < r.w - 1; ++x) {
    put(r.x + x, r.y, hor, st);
    put(r.x + x, r.y + r.h - 1, hor, st);
  }
  for (int y = 1; y < r.h - 1; ++y) {
    put(r.x, r.y + y, ver, st);
    put(r.x + r.w - 1, r.y + y, ver, st);
  }
  put(r.x, r.y, uni ? U'┌' : U'+', st);
  put(r.x + r.w - 1, r.y, uni ? U'┐' : U'+', st);
  put(r.x, r.y + r.h - 1, uni ? U'└' : U'+', st);
  put(r.x + r.w - 1, r.y + r.h - 1, uni ? U'┘' : U'+', st);
}

View View::sub(Rect r) const {
  const int x0 = clip_.x + std::max(0, r.x);
  const int y0 = clip_.y + std::max(0, r.y);
  const int x1 = std::min(clip_.x + clip_.w, clip_.x + r.x + r.w);
  const int y1 = std::min(clip_.y + clip_.h, clip_.y + r.y + r.h);
  return View(*s_, Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)});
}

// utf8_decode() a déménagé, byte pour byte, dans common/utf8.cpp (I1 : la
// même validation durcie servait render/ ici et une copie non durcie dans
// input/, corrigée deux fois séparément aurait fini par diverger). Ce
// fichier l'appelle désormais via l'include de common/utf8.hpp ci-dessus ;
// aucun comportement de View::text() n'a changé.

std::string Surface::text_row(int y) const {
  std::string out;
  if (y < 0 || y >= h_) return out;
  for (int x = 0; x < w_; ++x) {
    const Cell& c = at(x, y);
    if (c.width == 0) continue;  // couverte par sa cellule de tête
    out += encode_utf8(c.ch);
  }
  return out;
}

}  // namespace sshos
