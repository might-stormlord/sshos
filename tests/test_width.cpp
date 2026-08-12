#include <string>

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

// ---------------------------------------------------------------------------
// text_cells / elide_to_cells : la mesure et la coupure partagées par le
// panneau et l'aide. Elles portent l'invariant qui compte pour un cadre --
// ce qui sort ne dépasse JAMAIS le budget donné, marque de coupure comprise.
// ---------------------------------------------------------------------------

TEST(text_cells_counts_cells_not_bytes_nor_codepoints) {
  CHECK_EQ(sshos::text_cells("abc"), 3);
  CHECK_EQ(sshos::text_cells(""), 0);
  // « è » fait deux octets et une cellule.
  CHECK_EQ(sshos::text_cells("fenêtre"), 7);
  // Un idéogramme fait une cellule de plus qu'un point de code.
  CHECK_EQ(sshos::text_cells("日本"), 4);
  // Un combinant n'en prend aucune.
  CHECK_EQ(sshos::text_cells("e\xcc\x81"), 1);
}

TEST(elide_to_cells_never_exceeds_its_budget) {
  const char* samples[] = {"abcdefghij", "fenêtre modifiée", "日本語のファイル",
                           "a", "", "ааааааааああ"};
  for (const char* s : samples) {
    for (int budget = 0; budget <= 12; ++budget) {
      const std::string cut = sshos::elide_to_cells(s, budget, "…");
      if (sshos::text_cells(cut) > budget) {
        th::fail(__FILE__, __LINE__,
                 std::string("« ") + s + " » coupe a " +
                     std::to_string(budget) + " rend " +
                     std::to_string(sshos::text_cells(cut)) + " cellules");
      }
    }
  }
}

TEST(elide_to_cells_leaves_what_already_fits_untouched) {
  CHECK(sshos::elide_to_cells("abc", 3, "…") == "abc");
  CHECK(sshos::elide_to_cells("abc", 9, "…") == "abc");
  CHECK(sshos::elide_to_cells("fenêtre", 7, "…") == "fenêtre");
}

TEST(elide_to_cells_marks_the_cut_and_never_splits_a_sequence) {
  const std::string cut = sshos::elide_to_cells("fenêtre", 5, "…");
  CHECK_EQ(sshos::text_cells(cut), 5);
  CHECK(cut == "fenê…");

  // Pleine chasse : couper à 3 ne peut garder qu'UN idéogramme, pas un et
  // demi -- il n'existe pas de demi-cellule.
  const std::string wide = sshos::elide_to_cells("日本語", 3, "…");
  CHECK(wide == "日…");
}

// Le budget compte la marque. Sans ça, une élision « à 8 cellules » en
// rendrait neuf et écraserait la bordure d'à côté (défaut vu à la sonde de
// l'aide à 40x12).
TEST(elide_to_cells_counts_the_mark_inside_the_budget) {
  CHECK_EQ(sshos::text_cells(sshos::elide_to_cells("abcdefgh", 4, "…")), 4);
  CHECK_EQ(sshos::text_cells(sshos::elide_to_cells("abcdefgh", 4, "...")), 4);
  // Et quand même la marque ne tient pas, rien ne sort : mieux vaut du vide
  // qu'un signe qui déborde.
  CHECK(sshos::elide_to_cells("abcdefgh", 2, "...").empty());
  CHECK(sshos::elide_to_cells("abcdefgh", 0, "…").empty());
}
