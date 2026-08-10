#include "harness.hpp"
#include "render/cell.hpp"
#include "render/width.hpp"

using sshos::char_width;

TEST(width_ascii_is_one) {
  CHECK_EQ(char_width(U'a'), 1);
  CHECK_EQ(char_width(U' '), 1);
  CHECK_EQ(char_width(U'~'), 1);
}

TEST(width_cjk_is_two) {
  CHECK_EQ(char_width(U'日'), 2);  // 日
  CHECK_EQ(char_width(U'가'), 2);  // 가 hangul
  CHECK_EQ(char_width(U'Ａ'), 2);  // Ａ pleine chasse
}

TEST(width_emoji_is_two) {
  CHECK_EQ(char_width(U'\U0001f600'), 2);  // 😀
}

TEST(width_combining_is_zero) {
  CHECK_EQ(char_width(U'́'), 0);      // accent aigu combinant
  CHECK_EQ(char_width(U'‍'), 0);      // ZWJ
  CHECK_EQ(char_width(U'️'), 0);      // sélecteur de variation 16
  CHECK_EQ(char_width(U'\U000e0101'), 0);  // sélecteur de variation 18
}

TEST(width_control_is_zero) {
  CHECK_EQ(char_width(U'\n'), 0);
  CHECK_EQ(char_width(U'\033'), 0);
}

// East Asian Ambiguous : étroit par défaut, large sur demande.
TEST(width_ambiguous_follows_policy) {
  sshos::set_ambiguous_wide(false);
  CHECK_EQ(char_width(U'±'), 1);  // ±
  CHECK_EQ(char_width(U'─'), 1);  // ─ dessin de boîte

  sshos::set_ambiguous_wide(true);
  CHECK_EQ(char_width(U'±'), 2);
  CHECK_EQ(char_width(U'─'), 2);

  sshos::set_ambiguous_wide(false);  // remettre l'état par défaut
}

// Tests des bornes : chaque table doit traiter lo et hi inclusivement.
// Si hi était exclusif ou si upper_bound était mal orienté, ces tests
// ne captureraient pas l'erreur.

TEST(width_zero_boundaries) {
  // Plage choisie : {0x200B, 0x200F} (Zero Width Space, etc.)
  // Voisins vérifiés : 0x200A est largeur 1 (avant plage), 0x2010 est
  // largeur 1 (après, dans kAmbiguous mais ambiguous_wide=false donc 1)
  sshos::set_ambiguous_wide(false);

  CHECK_EQ(char_width(0x200A), 1);   // avant la plage
  CHECK_EQ(char_width(0x200B), 0);   // lo (start de plage)
  CHECK_EQ(char_width(0x200F), 0);   // hi (end de plage, doit être inclusif)
  CHECK_EQ(char_width(0x2010), 1);   // après la plage
}

TEST(width_wide_boundaries) {
  // Plage choisie : {0x1F300, 0x1F64F} (Miscellaneous Symbols and Pictographs, emoji)
  // Voisins vérifiés : 0x1F2FF est largeur 1 (avant emoji), 0x1F650 est
  // largeur 1 (après emoji)
  CHECK_EQ(char_width(0x1F2FF), 1);  // avant la plage
  CHECK_EQ(char_width(0x1F300), 2);  // lo (start de plage)
  CHECK_EQ(char_width(0x1F64F), 2);  // hi (end de plage, doit être inclusif)
  CHECK_EQ(char_width(0x1F650), 1);  // après la plage
}

TEST(width_ambiguous_boundaries) {
  // Plage choisie : {0x2500, 0x257F} (Box Drawing)
  // Voisins vérifiés : 0x24FF est largeur 1 (avant dessin de boîte),
  // 0x2580 est largeur 1 (après boîte, pas dans autre plage)

  // Avec ambiguous_wide = false
  sshos::set_ambiguous_wide(false);
  CHECK_EQ(char_width(0x24FF), 1);   // avant la plage
  CHECK_EQ(char_width(0x2500), 1);   // lo (start de plage, étroit par défaut)
  CHECK_EQ(char_width(0x257F), 1);   // hi (end de plage, étroit par défaut)
  CHECK_EQ(char_width(0x2580), 1);   // après la plage

  // Avec ambiguous_wide = true
  sshos::set_ambiguous_wide(true);
  CHECK_EQ(char_width(0x24FF), 1);   // avant la plage (pas changé)
  CHECK_EQ(char_width(0x2500), 2);   // lo (start, large selon policy)
  CHECK_EQ(char_width(0x257F), 2);   // hi (end, large selon policy)
  CHECK_EQ(char_width(0x2580), 1);   // après la plage (pas changé)

  // Restaurer l'état par défaut
  sshos::set_ambiguous_wide(false);
}

// Contrôles : DEL (0x7F) et bande C1 (0x80–0x9F) doivent être largeur 0.
TEST(width_control_boundaries) {
  CHECK_EQ(char_width(0x7F), 0);   // DEL
  CHECK_EQ(char_width(0x80), 0);   // Première contrôle C1
  CHECK_EQ(char_width(0x9F), 0);   // Dernière contrôle C1
}

// Getter : ambiguous_wide() doit refléter ce que set_ambiguous_wide() a posé.
TEST(width_ambiguous_getter) {
  // État initial (par défaut, étroit)
  CHECK_EQ(sshos::ambiguous_wide(), false);

  // Après set à true
  sshos::set_ambiguous_wide(true);
  CHECK_EQ(sshos::ambiguous_wide(), true);

  // Après set à false
  sshos::set_ambiguous_wide(false);
  CHECK_EQ(sshos::ambiguous_wide(), false);

  // Vérifier état final restauré (important pour ne pas contaminer autres tests)
}
