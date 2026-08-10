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
