#include <string>

#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Cell;
using sshos::Rect;
using sshos::Style;
using sshos::Surface;
using sshos::View;

TEST(surface_starts_blank) {
  Surface s(4, 2);
  CHECK_EQ(s.w(), 4);
  CHECK_EQ(s.h(), 2);
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(3, 1).width), 1);
}

TEST(surface_view_translates_coordinates) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  CHECK_EQ(v.w(), 3);
  CHECK_EQ(v.h(), 2);
  v.put(0, 0, U'X', Style{});
  CHECK_EQ(s.at(2, 1).ch, U'X');
  CHECK_EQ(s.at(0, 0).ch, U' ');
}

// La propriété qui rend une application incapable de nuire à ses voisines.
TEST(surface_view_silently_drops_out_of_clip_writes) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  v.put(-1, 0, U'A', Style{});
  v.put(3, 0, U'B', Style{});
  v.put(0, 2, U'C', Style{});
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 10; ++x) CHECK_EQ(s.at(x, y).ch, U' ');
  }
}

TEST(surface_nested_sub_clips_to_parent) {
  Surface s(10, 4);
  View outer = s.root().sub(Rect{2, 0, 4, 1});
  View inner = outer.sub(Rect{2, 0, 10, 1});  // déborde volontairement
  CHECK_EQ(inner.w(), 2);
  inner.put(0, 0, U'Z', Style{});
  CHECK_EQ(s.at(4, 0).ch, U'Z');
}

TEST(surface_text_writes_utf8) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "abc", Style{});
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'a');
  CHECK_EQ(s.at(2, 0).ch, U'c');
}

TEST(surface_text_marks_wide_and_continuation) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "\xe6\x97\xa5x", Style{});  // 日x
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'日');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);
  CHECK_EQ(s.at(2, 0).ch, U'x');
}

// Règle 2 du §4.1 : jamais de glyphe large en dernière colonne. Sans elle,
// le terminal replie la ligne et le modele de frame precedente est perdu.
TEST(surface_never_places_wide_glyph_in_last_column) {
  Surface s(3, 2);
  View v = s.root();
  const int cols = v.text(2, 0, "\xe6\x97\xa5", Style{});  // 日 en derniere colonne
  CHECK_EQ(cols, 0);
  CHECK_EQ(s.at(2, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 1);
}

TEST(surface_fill_respects_clip) {
  Surface s(6, 3);
  View v = s.root().sub(Rect{1, 1, 2, 1});
  Style red;
  red.bg = sshos::Color::indexed(1);
  v.fill(Rect{0, 0, 100, 100}, red);
  CHECK_EQ(s.at(1, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(2, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(3, 1).bg, sshos::Color::def());
  CHECK_EQ(s.at(1, 0).bg, sshos::Color::def());
}

TEST(utf8_decode_handles_truncated_input) {
  char32_t cp = 0;
  const std::string truncated = "\xe6\x97";  // moitie de 日
  const size_t used = sshos::utf8_decode(truncated, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(2));
  CHECK_EQ(cp, U'�');
}

// Écrire par-dessus la tête d'une paire large orpheline la continuation
TEST(surface_overwrite_wide_glyph_head) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Écrire à la position 0 (la tête) orpheline la continuation
  v.put(0, 0, U'A', Style{});
  CHECK_EQ(s.at(0, 0).ch, U'A');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}

// Écrire par-dessus la continuation d'une paire large orpheline la tête
TEST(surface_overwrite_wide_glyph_continuation) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Écrire à la position 1 (la continuation) orpheline la tête
  v.put(1, 0, U'B', Style{});
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U'B');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}

// Remplir une zone qui traverse une paire large orpheline les deux moitiés
TEST(surface_fill_wide_glyph_pair) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Remplir depuis 0 à 2 (couvre la tête et la continuation)
  Style empty;
  v.fill(Rect{0, 0, 2, 1}, empty);
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}

// Reproduire la corruption d'orpheline par décalage de colonne : la seconde
// paire devient orpheline si cleanup_orphan() n'est appelé que pour ox
TEST(surface_wide_glyph_shift_orphan) {
  Surface s(6, 1);
  View v = s.root();
  // Colonne 2 et 3 forment une paire large
  v.put(2, 0, U'日', Style{});
  CHECK_EQ(s.at(2, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(3, 0).width), 0);

  // Écrire une nouvelle paire large à la colonne 1 : elle écrase la tête de
  // l'ancienne paire à la colonne 2. Si on ne nettoie que col1, col3 reste
  // orpheline (continuation sans tête valide).
  v.put(1, 0, U'中', Style{});

  // Vérifier l'état final : col1-2 = paire, col3-4 = blancs (pas d'orpheline)
  CHECK_EQ(s.at(1, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 0);
  CHECK_EQ(s.at(3, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(3, 0).width), 1);
  CHECK_EQ(s.at(4, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(4, 0).width), 1);
}

// Paire large qui chevauche le clip de deux sub-views : vérifier que le
// nettoyage d'orpheline fonctionne même hors du clip. Créer une paire qui
// s'étend depuis col 1 à col 2, où col 1 est dans une vue et col 2 dans une autre.
TEST(surface_wide_orphan_outside_clip) {
  Surface s(6, 1);
  View v_right = s.root().sub(Rect{2, 0, 4, 1});  // cols 2-5 dans surface

  // Place la tête d'une paire large en col 1 (surface coords),
  // continuation en col 2. Ceci ne peut se faire qu'avec root ou via une
  // écriture qui place le premier caractère à la bonne position. Faire via
  // l'écriture directe sur la surface grâce au root view
  View root = s.root();
  root.put(1, 0, U'日', Style{});
  CHECK_EQ(s.at(1, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 0);

  // Maintenant v_right écrit dans son clip (surface col 2, qui est col 0
  // dans son clip). Cela écrase la continuation de la paire. Le cleanup
  // d'orpheline doit nettoyer la tête en col 1 même si elle est hors du
  // clip de v_right (c'est-à-dire hors de v_right's sub-view).
  v_right.put(0, 0, U'A', Style{});

  // Col 1 doit être nettoyé (tête orpheline) et col 2 contient 'A'
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
  CHECK_EQ(s.at(2, 0).ch, U'A');
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 1);
}

// Vérifier que fill() nettoie les orphelines même partiellement : remplir
// seulement une moitié d'une paire laisse l'autre en blanc
TEST(surface_fill_half_wide_glyph) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Remplir seulement la colonne 0 (la tête) : col 1 doit devenir blanc
  Style red;
  red.bg = sshos::Color::indexed(1);
  v.fill(Rect{0, 0, 1, 1}, red);

  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(0, 0).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
  CHECK_EQ(s.at(1, 0).bg, sshos::Color::def());  // bg non changé
}

// Le deuxième appel cleanup_orphan(ox+1,oy) ne peut être testé que quand
// l'orpheline qu'il nettoie est *hors du clip* du view qui fait l'écriture.
// Reproduire : Surface(8,1), placer paire large aux cols 4-5 via root.
// V_left = root.sub({0,0,5,1}) couvre cols 0-4, col 5 est dehors.
// V_left.put(3,0,'中') écrit ox=3,4 -> écrase tête col 4 -> col 5 orpheline.
// Seul le deuxième cleanup peut la trouver (première call cleans ox=3 neighbours).
TEST(surface_second_cleanup_orphan_outside_clip) {
  Surface s(8, 1);
  View root = s.root();
  // Placer paire large aux cols 4-5 via root
  root.put(4, 0, U'日', Style{});
  CHECK_EQ(s.at(4, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(5, 0).width), 0);

  // Créer sub-view qui couvre cols 0-4, col 5 est hors du clip
  View v_left = root.sub(Rect{0, 0, 5, 1});

  // V_left.put(3,0,'中') -> ox=3, écrit ox=3 et ox+1=4
  // Clobbered: col 4 était tête de la paire, devient continuation de '中'
  // Résultat: col 5 (ancienne continuation) est orpheline, hors du clip
  v_left.put(3, 0, U'中', Style{});

  // Vérifier la grille entière : toute continuation doit avoir tête à gauche,
  // toute tête doit avoir continuation à droite (invariant global)
  for (int x = 0; x < 8; ++x) {
    const int w = static_cast<int>(s.at(x, 0).width);
    if (w == 0) {
      // Continuation : vérifier tête immédiatement à gauche
      if (x > 0) {
        CHECK_EQ(static_cast<int>(s.at(x - 1, 0).width), 2);
      }
    } else if (w == 2) {
      // Tête : vérifier continuation immédiatement à droite
      if (x + 1 < 8) {
        CHECK_EQ(static_cast<int>(s.at(x + 1, 0).width), 0);
      }
    } else {
      // Largeur 1 : pas de contrainte
    }
  }
}
