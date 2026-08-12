#include <string>

#include "harness.hpp"
#include "vt/screen.hpp"

using sshos::ScreenCell;
using sshos::Screen;

namespace {

// Écrit une chaîne ASCII, caractère par caractère. Tout ce qui suit passe
// par print(), jamais par un accès direct à la grille : un test qui
// écrirait dans les cellules ne mesurerait plus rien.
void puts_ascii(Screen& s, const std::string& text) {
  for (char c : text) s.print(static_cast<char32_t>(c));
}

// Une grille en une chaîne, lignes séparées par des '/'. Une transcription
// se lit d'un coup d'œil et s'imprime en entier quand elle diffère.
std::string dump(const Screen& s) {
  std::string out;
  for (int y = 0; y < s.rows(); ++y) {
    if (y != 0) out.push_back('/');
    out += s.line_text(y);
  }
  return out;
}

constexpr char32_t kWide = U'一';  // 一, pleine chasse

}  // namespace

// ---------------------------------------------------------------------------
// Le retour à la ligne différé. C'est LE piège de la grille : écrire dans
// la dernière colonne ne doit pas descendre, sinon toute ligne de largeur
// pleine saute une ligne sur deux.
// ---------------------------------------------------------------------------

TEST(screen_does_not_wrap_when_the_last_column_is_written) {
  Screen s(10, 4);
  puts_ascii(s, "0123456789");  // exactement la largeur
  CHECK_EQ(s.cursor().y, 0);
  CHECK_EQ(s.cursor().x, 9);  // reste SUR la dernière colonne
  CHECK(s.wrap_pending());
  CHECK_EQ(s.line_text(0), std::string("0123456789"));
  CHECK_EQ(s.line_text(1), std::string(""));
}

TEST(screen_wraps_on_the_character_after_the_last_column) {
  Screen s(10, 4);
  puts_ascii(s, "0123456789A");
  CHECK_EQ(s.cursor().y, 1);
  CHECK_EQ(s.cursor().x, 1);
  CHECK(!s.wrap_pending());
  CHECK_EQ(s.line_text(0), std::string("0123456789"));
  CHECK_EQ(s.line_text(1), std::string("A"));
}

TEST(screen_cancels_a_pending_wrap_on_backspace_without_wrapping) {
  Screen s(10, 4);
  puts_ascii(s, "0123456789");
  REQUIRE(s.wrap_pending());
  s.backspace();
  // Le drapeau tombe et le curseur NE BOUGE PAS : il est déjà sur la
  // dernière colonne, c'est là que la suite doit s'écrire.
  CHECK(!s.wrap_pending());
  CHECK_EQ(s.cursor().x, 9);
  CHECK_EQ(s.cursor().y, 0);
  s.print(U'Z');
  CHECK_EQ(s.line_text(0), std::string("012345678Z"));
  CHECK_EQ(s.line_text(1), std::string(""));
}

TEST(screen_cub_eats_one_step_to_cancel_a_pending_wrap) {
  // CUB se comporte comme BS : le curseur est logiquement une cellule
  // au-delà de la dernière colonne, donc le premier pas ne fait que l'y
  // ramener. `CSI 1 D` ne bouge pas, `CSI 2 D` recule d'une seule.
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.move_left(1);
    CHECK(!s.wrap_pending());
    CHECK_EQ(s.cursor().x, 9);
  }
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.move_left(2);
    CHECK(!s.wrap_pending());
    CHECK_EQ(s.cursor().x, 8);
  }
  {
    // Sans retour en attente, CUB recule normalement.
    Screen s(10, 4);
    s.move_to(5, 0);
    s.move_left(2);
    CHECK_EQ(s.cursor().x, 3);
  }
}

TEST(screen_cancels_a_pending_wrap_on_every_move_that_should) {
  // Chaque mouvement lève le drapeau : le laisser traîner ferait descendre
  // d'une ligne un caractère écrit ailleurs, longtemps après.
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.carriage_return();
    CHECK(!s.wrap_pending());
  }
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.move_to(0, 0);
    CHECK(!s.wrap_pending());
  }
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.tab();
    CHECK(!s.wrap_pending());
  }
  {
    Screen s(10, 4);
    puts_ascii(s, "0123456789");
    s.line_feed();
    CHECK(!s.wrap_pending());
  }
}

// ---------------------------------------------------------------------------
// La pleine chasse.
// ---------------------------------------------------------------------------

TEST(screen_takes_two_cells_for_a_full_width_character) {
  Screen s(10, 4);
  s.print(kWide);
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(int(s.at(0, 0).width), 2);
  CHECK_EQ(int(s.at(1, 0).width), 0);  // la moitié droite
  CHECK_EQ(s.at(0, 0).ch, kWide);
}

TEST(screen_pushes_a_full_width_character_down_instead_of_splitting_it) {
  Screen s(10, 4);
  puts_ascii(s, "012345678");  // curseur en colonne 9, la dernière
  REQUIRE_EQ(s.cursor().x, 9);
  REQUIRE(!s.wrap_pending());
  s.print(kWide);
  // Il ne tient pas : il descend ENTIER, il ne se coupe pas en deux.
  CHECK_EQ(s.cursor().y, 1);
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(int(s.at(9, 0).width), 1);  // la case laissée vide reste simple
  CHECK_EQ(s.at(9, 0).ch, U' ');
  CHECK_EQ(int(s.at(0, 1).width), 2);
}

TEST(screen_clears_the_orphan_half_when_a_wide_cell_is_overwritten) {
  Screen s(10, 4);
  s.print(kWide);
  s.move_to(0, 0);
  s.print(U'A');
  // La moitié droite ne doit pas survivre à la disparition de sa gauche :
  // le rendu afficherait un demi-idéogramme.
  CHECK_EQ(s.at(0, 0).ch, U'A');
  CHECK_EQ(int(s.at(1, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(s.line_text(0), std::string("A"));
}

TEST(screen_clears_the_left_half_when_the_right_one_is_overwritten) {
  Screen s(10, 4);
  s.print(kWide);
  s.move_to(1, 0);
  s.print(U'B');
  CHECK_EQ(s.at(1, 0).ch, U'B');
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(int(s.at(0, 0).width), 1);
  CHECK_EQ(s.line_text(0), std::string(" B"));
}

// ---------------------------------------------------------------------------
// Les mouvements et leurs bornes.
// ---------------------------------------------------------------------------

TEST(screen_bounds_every_cursor_move) {
  Screen s(10, 4);
  s.move_to(99, 99);
  CHECK_EQ(s.cursor().x, 9);
  CHECK_EQ(s.cursor().y, 3);
  s.move_to(-5, -5);
  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 0);

  s.move_up(100);
  CHECK_EQ(s.cursor().y, 0);
  s.move_left(100);
  CHECK_EQ(s.cursor().x, 0);
  s.move_down(100);
  CHECK_EQ(s.cursor().y, 3);
  s.move_right(100);
  CHECK_EQ(s.cursor().x, 9);
}

TEST(screen_treats_a_zero_move_as_one) {
  // CSI 0 A veut dire « une ligne », pas « zéro ». C'est la convention de
  // tous les paramètres de déplacement, et l'oublier fige le curseur.
  Screen s(10, 4);
  s.move_to(5, 2);
  s.move_up(0);
  CHECK_EQ(s.cursor().y, 1);
  s.move_left(0);
  CHECK_EQ(s.cursor().x, 4);
  s.move_down(0);
  CHECK_EQ(s.cursor().y, 2);
  s.move_right(0);
  CHECK_EQ(s.cursor().x, 5);
}

TEST(screen_moves_on_a_single_axis_with_cha_and_vpa) {
  Screen s(10, 4);
  s.move_to(3, 2);
  s.set_column(7);
  CHECK_EQ(s.cursor().x, 7);
  CHECK_EQ(s.cursor().y, 2);  // la ligne ne bouge pas
  s.set_row(0);
  CHECK_EQ(s.cursor().x, 7);  // la colonne ne bouge pas
  CHECK_EQ(s.cursor().y, 0);
}

TEST(screen_backspace_stops_at_the_left_edge) {
  Screen s(10, 4);
  s.move_to(1, 1);
  s.backspace();
  CHECK_EQ(s.cursor().x, 0);
  s.backspace();
  CHECK_EQ(s.cursor().x, 0);  // et ne remonte pas à la ligne d'avant
  CHECK_EQ(s.cursor().y, 1);
}

// ---------------------------------------------------------------------------
// Le défilement.
// ---------------------------------------------------------------------------

TEST(screen_scrolls_up_when_line_feed_falls_off_the_bottom) {
  Screen s(4, 3);
  puts_ascii(s, "aa");
  s.next_line();
  puts_ascii(s, "bb");
  s.next_line();
  puts_ascii(s, "cc");
  REQUIRE_EQ(dump(s), std::string("aa/bb/cc"));

  s.next_line();
  puts_ascii(s, "dd");
  CHECK_EQ(dump(s), std::string("bb/cc/dd"));
  CHECK_EQ(s.cursor().y, 2);  // le curseur reste sur la dernière ligne
}

TEST(screen_scrolls_down_on_reverse_index_at_the_top) {
  Screen s(4, 3);
  puts_ascii(s, "aa");
  s.next_line();
  puts_ascii(s, "bb");
  s.move_to(0, 0);
  s.reverse_index();
  CHECK_EQ(s.cursor().y, 0);
  CHECK_EQ(dump(s), std::string("/aa/bb"));
}

TEST(screen_reverse_index_just_goes_up_when_it_can) {
  Screen s(4, 3);
  s.move_to(2, 2);
  s.reverse_index();
  CHECK_EQ(s.cursor().y, 1);
  CHECK_EQ(s.cursor().x, 2);  // RI ne touche pas à la colonne
}

TEST(screen_next_line_returns_to_the_left_margin) {
  Screen s(10, 4);
  s.move_to(6, 0);
  s.next_line();
  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 1);
}

TEST(screen_line_feed_keeps_the_column) {
  // LF descend SANS revenir à gauche : c'est NEL qui fait les deux, et les
  // confondre casse tout affichage en colonnes.
  Screen s(10, 4);
  s.move_to(6, 0);
  s.line_feed();
  CHECK_EQ(s.cursor().x, 6);
  CHECK_EQ(s.cursor().y, 1);
}

// ---------------------------------------------------------------------------
// Les taquets de tabulation.
// ---------------------------------------------------------------------------

TEST(screen_tabs_land_on_multiples_of_eight_by_default) {
  Screen s(30, 3);
  s.tab();
  CHECK_EQ(s.cursor().x, 8);
  s.tab();
  CHECK_EQ(s.cursor().x, 16);
  s.move_to(9, 0);
  s.tab();
  CHECK_EQ(s.cursor().x, 16);  // le taquet SUIVANT, pas le sien
}

TEST(screen_tab_stops_at_the_last_column_without_wrapping) {
  Screen s(10, 3);
  s.tab();
  REQUIRE_EQ(s.cursor().x, 8);
  s.tab();
  CHECK_EQ(s.cursor().x, 9);  // plus de taquet devant
  CHECK_EQ(s.cursor().y, 0);  // et surtout : pas de retour à la ligne
  s.tab();
  CHECK_EQ(s.cursor().x, 9);
}

TEST(screen_honours_a_tab_stop_set_by_hand) {
  Screen s(30, 3);
  s.move_to(3, 0);
  s.set_tab();
  s.move_to(0, 0);
  s.tab();
  CHECK_EQ(s.cursor().x, 3);
}

TEST(screen_forgets_a_tab_stop_that_was_cleared) {
  Screen s(30, 3);
  s.move_to(8, 0);
  s.clear_tab();
  s.move_to(0, 0);
  s.tab();
  CHECK_EQ(s.cursor().x, 16);  // 8 a disparu, on va au suivant
}

TEST(screen_clears_every_tab_stop_at_once) {
  Screen s(30, 3);
  s.clear_all_tabs();
  s.tab();
  CHECK_EQ(s.cursor().x, 29);  // plus un seul taquet : au bout
}

// ---------------------------------------------------------------------------
// La lecture de la grille.
// ---------------------------------------------------------------------------

TEST(screen_trims_the_trailing_blanks_of_a_line) {
  Screen s(20, 3);
  puts_ascii(s, "salut");
  // Rognée : garder 20 colonnes de blancs par ligne pour 10 000 lignes de
  // scrollback coûterait pour rien.
  CHECK_EQ(s.line_text(0), std::string("salut"));
}

TEST(screen_keeps_the_blanks_that_are_inside_a_line) {
  Screen s(20, 3);
  puts_ascii(s, "a   b");
  CHECK_EQ(s.line_text(0), std::string("a   b"));
}

TEST(screen_reads_a_line_out_of_range_as_empty) {
  Screen s(10, 3);
  CHECK_EQ(s.line_text(-1), std::string(""));
  CHECK_EQ(s.line_text(3), std::string(""));
  CHECK_EQ(s.line_text(99), std::string(""));
}

TEST(screen_reads_a_cell_out_of_range_as_blank) {
  Screen s(10, 3);
  CHECK_EQ(s.at(-1, 0).ch, U' ');
  CHECK_EQ(s.at(0, -1).ch, U' ');
  CHECK_EQ(s.at(10, 0).ch, U' ');
  CHECK_EQ(s.at(0, 3).ch, U' ');
}

TEST(screen_reads_a_full_width_character_back_whole) {
  Screen s(10, 3);
  s.print(kWide);
  puts_ascii(s, "x");
  // La moitié droite ne doit PAS ressortir en double dans le texte.
  CHECK_EQ(s.line_text(0), std::string("\xe4\xb8\x80" "x"));
}

TEST(screen_refuses_a_degenerate_size_instead_of_crashing) {
  Screen s(0, 0);
  CHECK(s.cols() >= 1);
  CHECK(s.rows() >= 1);
  s.print(U'a');  // ne doit pas déborder
  CHECK_EQ(s.cursor().x, 0);
}

// ---------------------------------------------------------------------------
// Les orphelins de pleine chasse, et le recyclage des lignes.
// ---------------------------------------------------------------------------

// Écraser une pleine chasse par SA GAUCHE laisse sa seconde moitié seule
// derrière : une cellule de largeur 0 qui ne suit plus rien. Le nettoyage
// doit regarder devant lui, pas seulement sous lui.
TEST(screen_clears_the_tail_of_a_wide_character_it_writes_over) {
  Screen s(10, 2);
  s.move_to(1, 0);
  s.print(kWide);  // occupe 1 et 2
  s.move_to(0, 0);
  s.print(kWide);  // occupe 0 et 1, et doit nettoyer 2

  CHECK_EQ(s.at(0, 0).ch, kWide);
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(s.at(1, 0).width, 0);
  // 2 etait la seconde moitie de l'ancien : plus rien ne la reclame.
  CHECK_EQ(s.at(2, 0).ch, U' ');
  CHECK_EQ(s.at(2, 0).width, 1);
}

// Un combinant ne prend pas de place et ne s'imprime pas seul. L'imprimer
// quand même poserait une cellule de largeur 0, c'est-à-dire une seconde
// moitié de pleine chasse qui ne suit aucune première : line_text() la
// sauterait, et la grille mentirait sur ce qu'elle contient.
TEST(screen_ignores_a_zero_width_character) {
  Screen s(10, 2);
  puts_ascii(s, "ab");
  s.print(U'́');  // accent aigu combinant

  CHECK_EQ(dump(s), "ab/");
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(s.at(2, 0).width, 1);
}

// La ligne qui rentre par le bas d'un défilement est celle qui vient de
// sortir par le haut : sans effacement, le texte d'il y a un écran
// réapparaît en bas.
TEST(screen_blanks_the_line_recycled_by_a_scroll_up) {
  Screen s(6, 3);
  puts_ascii(s, "aaa");
  s.next_line();
  puts_ascii(s, "bbb");
  s.next_line();
  puts_ascii(s, "ccc");
  REQUIRE_EQ(dump(s), "aaa/bbb/ccc");

  s.index();  // depuis la derniere ligne : defile
  CHECK_EQ(dump(s), "bbb/ccc/");
}

TEST(screen_blanks_the_line_recycled_by_a_scroll_down) {
  Screen s(6, 3);
  puts_ascii(s, "aaa");
  s.next_line();
  puts_ascii(s, "bbb");
  s.next_line();
  puts_ascii(s, "ccc");
  s.move_to(0, 0);

  s.reverse_index();  // depuis la premiere ligne : defile a l'envers
  CHECK_EQ(dump(s), "/aaa/bbb");
}

// LF descend d'une ligne SANS revenir à gauche -- c'est NEL, et lui seul,
// qui fait les deux. Un LF qui ramènerait le curseur en colonne 0
// casserait tout affichage qui compose une ligne en plusieurs morceaux.
TEST(screen_keeps_the_column_across_a_line_feed) {
  Screen s(10, 3);
  puts_ascii(s, "abc");
  s.line_feed();

  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 1);

  s.next_line();  // celui-la, oui, revient a gauche
  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 2);
}

// IND (ESC D) est le LF de la forme échappée : il descend et garde sa
// colonne. C'est NEL (ESC E) qui descend ET revient à gauche. Confondre
// les deux décale toute sortie qui indexe en milieu de ligne.
TEST(screen_keeps_the_column_across_an_index) {
  Screen s(10, 3);
  puts_ascii(s, "abc");
  s.index();

  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 1);
}

// Et son symétrique : RI (ESC M) remonte sans revenir à gauche.
TEST(screen_keeps_the_column_across_a_reverse_index) {
  Screen s(10, 3);
  s.move_to(0, 1);
  puts_ascii(s, "abc");
  s.reverse_index();

  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 0);
}
