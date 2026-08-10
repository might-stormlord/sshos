#include "render/surface.hpp"

#include <algorithm>

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
  c.cluster = 0;
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

View View::sub(Rect r) const {
  const int x0 = clip_.x + std::max(0, r.x);
  const int y0 = clip_.y + std::max(0, r.y);
  const int x1 = std::min(clip_.x + clip_.w, clip_.x + r.x + r.w);
  const int y1 = std::min(clip_.y + clip_.h, clip_.y + r.y + r.h);
  return View(*s_, Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)});
}

size_t utf8_decode(std::string_view s, size_t pos, char32_t& out) {
  out = U'�';
  if (pos >= s.size()) return 1;

  const auto b0 = static_cast<unsigned char>(s[pos]);
  int need = 0;
  char32_t cp = 0;

  if (b0 < 0x80) {
    out = b0;
    return 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    need = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    need = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    need = 3;
    cp = b0 & 0x07;
  } else {
    return 1;  // octet de continuation isolé ou séquence illégale
  }

  if (pos + need >= s.size() + 0 && pos + need > s.size() - 1) {
    // séquence tronquée : consommer ce qui est là, sans jamais rendre 0
    return s.size() - pos;
  }

  for (int k = 1; k <= need; ++k) {
    const auto bk = static_cast<unsigned char>(s[pos + k]);
    if ((bk & 0xC0) != 0x80) return static_cast<size_t>(k);
    cp = (cp << 6) | (bk & 0x3F);
  }

  // Valider le scalaire Unicode : rejeter les surrogates [D800, DFFF],
  // les valeurs > 10FFFF, et les séquences trop longues (ex: E0 80 80 → 0).
  // Utiliser la règle "maximal subpart" : consommer la séquence complète
  // même si le codepoint est invalide.
  const bool is_surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  const bool is_out_of_range = (cp > 0x10FFFF);
  const bool is_overlong = (need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
                           (need == 3 && cp < 0x10000);

  if (is_surrogate || is_out_of_range || is_overlong) {
    out = 0xFFFD;  // Caractère de remplacement
  } else {
    out = cp;
  }
  return static_cast<size_t>(need + 1);
}

}  // namespace sshos
