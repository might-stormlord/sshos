#include <string>

#include "harness.hpp"
#include "vt/charset.hpp"
#include "vt/screen.hpp"

using sshos::Charset;
using sshos::charset_from_final;
using sshos::translate;

// ------------------------------------------------------------- la bascule

TEST(charset_reads_the_final_byte_of_the_designation) {
  CHECK(charset_from_final('0') == Charset::Graphics);
  CHECK(charset_from_final('B') == Charset::Ascii);
}

// Un final qu'on ne connaît pas rend l'ASCII : c'est le jeu qui ne
// surprend personne, et un invité qui demande un jeu exotique lira au
// moins des lettres au lieu de traits arbitraires.
TEST(charset_falls_back_to_ascii_on_a_designation_it_does_not_know) {
  CHECK(charset_from_final('A') == Charset::Ascii);
  CHECK(charset_from_final('%') == Charset::Ascii);
}

// ---------------------------------------------------------- la traduction

TEST(charset_leaves_everything_alone_in_ascii) {
  CHECK_EQ(translate(U'q', Charset::Ascii), U'q');
  CHECK_EQ(translate(U'x', Charset::Ascii), U'x');
  CHECK_EQ(translate(U'A', Charset::Ascii), U'A');
}

// Les quatre traits dont dépend tout cadre. Sans eux, `mc` s'affiche en
// `qqqqqqq` au lieu d'une ligne.
TEST(charset_draws_the_lines_of_a_frame) {
  CHECK_EQ(translate(U'q', Charset::Graphics), U'─');
  CHECK_EQ(translate(U'x', Charset::Graphics), U'│');
  CHECK_EQ(translate(U'l', Charset::Graphics), U'┌');
  CHECK_EQ(translate(U'k', Charset::Graphics), U'┐');
  CHECK_EQ(translate(U'm', Charset::Graphics), U'└');
  CHECK_EQ(translate(U'j', Charset::Graphics), U'┘');
}

TEST(charset_draws_the_junctions_of_a_frame) {
  CHECK_EQ(translate(U't', Charset::Graphics), U'├');
  CHECK_EQ(translate(U'u', Charset::Graphics), U'┤');
  CHECK_EQ(translate(U'v', Charset::Graphics), U'┴');
  CHECK_EQ(translate(U'w', Charset::Graphics), U'┬');
  CHECK_EQ(translate(U'n', Charset::Graphics), U'┼');
}

TEST(charset_draws_the_symbols_that_are_not_lines) {
  CHECK_EQ(translate(U'a', Charset::Graphics), U'▒');
  CHECK_EQ(translate(U'`', Charset::Graphics), U'◆');
  CHECK_EQ(translate(U'f', Charset::Graphics), U'°');
  CHECK_EQ(translate(U'g', Charset::Graphics), U'±');
  CHECK_EQ(translate(U'~', Charset::Graphics), U'·');
  CHECK_EQ(translate(U'{', Charset::Graphics), U'π');
  CHECK_EQ(translate(U'y', Charset::Graphics), U'≤');
}

// La table ne couvre QUE 0x5F-0x7E. Les chiffres, les majuscules et la
// ponctuation basse restent eux-mêmes : un titre écrit en semi-graphique
// resterait lisible.
TEST(charset_leaves_what_is_outside_the_table_alone) {
  CHECK_EQ(translate(U'A', Charset::Graphics), U'A');
  CHECK_EQ(translate(U'1', Charset::Graphics), U'1');
  CHECK_EQ(translate(U'0', Charset::Graphics), U'0');
  CHECK_EQ(translate(U' ', Charset::Graphics), U' ');
  CHECK_EQ(translate(U'^', Charset::Graphics), U'^');  // juste avant 0x5F
}

// Le point de code non ASCII passe tel quel : un invité peut mêler de
// l'UTF-8 à ses cadres, et le traduire serait le corrompre.
TEST(charset_leaves_a_non_ascii_code_point_alone) {
  CHECK_EQ(translate(U'é', Charset::Graphics), U'é');
  CHECK_EQ(translate(U'一', Charset::Graphics), U'一');
}

// Un trou de la table (`0x5F`, l'espace insécable de la norme DEC, et les
// quelques positions sans équivalent) doit rendre quelque chose de
// visible, jamais un point de code nul.
TEST(charset_never_renders_a_null_code_point) {
  for (char32_t c = 0x5F; c <= 0x7E; ++c) {
    CHECK(translate(c, Charset::Graphics) != U'\0');
  }
}

// --------------------------------------------- le jeu courant de la grille

// La grille tient le jeu courant, et `print()` le consulte pour CHAQUE
// caractère : un `ESC ( 0` reçu au milieu d'une ligne prend effet sur la
// suite de cette ligne, pas sur ce qui la précède.
TEST(screen_prints_through_the_current_charset) {
  sshos::Screen s(8, 2);
  s.print(U'q');
  s.set_charset(Charset::Graphics);
  s.print(U'q');

  CHECK_EQ(s.at(0, 0).ch, U'q');
  CHECK_EQ(s.at(1, 0).ch, U'─');
}

TEST(screen_starts_in_ascii) {
  const sshos::Screen s(8, 2);
  CHECK(s.charset() == Charset::Ascii);
}

// DECSC sauve le jeu courant, DECRC le rend : un `mc` qui sauve son
// curseur en plein cadre attend de retrouver ses traits.
TEST(screen_saves_the_charset_with_the_cursor) {
  sshos::Screen s(8, 2);
  s.set_charset(Charset::Graphics);
  s.save_cursor();

  s.set_charset(Charset::Ascii);
  s.restore_cursor();
  CHECK(s.charset() == Charset::Graphics);
}

// Et l'écran alterné le garde de côté comme le reste.
TEST(screen_gives_the_charset_back_when_it_leaves_the_alternate_page) {
  sshos::Screen s(8, 2);
  s.set_charset(Charset::Graphics);

  s.enter_alt_screen();
  s.set_charset(Charset::Ascii);
  s.leave_alt_screen();

  CHECK(s.charset() == Charset::Graphics);
}
