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

// ---------------------------------------------------------------------------
// Les effacements : ED, EL, ECH.
// ---------------------------------------------------------------------------

namespace {

// Une grille marquée : chaque ligne porte son propre chiffre répété, de
// sorte qu'une transcription dise d'un coup d'œil ce qui a disparu et où.
Screen marked(int cols, int rows) {
  Screen s(cols, rows);
  for (int y = 0; y < rows; ++y) {
    s.move_to(0, y);
    for (int x = 0; x < cols; ++x) s.print(static_cast<char32_t>(U'0' + y));
  }
  return s;
}

}  // namespace

TEST(screen_erases_from_the_cursor_to_the_end_of_the_display) {
  Screen s = marked(4, 3);
  s.move_to(2, 1);
  s.erase_display(0);

  CHECK_EQ(dump(s), "0000/11/");
  // Le curseur ne bouge pas : ED n'est pas un deplacement.
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(s.cursor().y, 1);
}

TEST(screen_erases_from_the_start_of_the_display_to_the_cursor) {
  Screen s = marked(4, 3);
  s.move_to(2, 1);
  s.erase_display(1);

  // La cellule SOUS le curseur part elle aussi.
  CHECK_EQ(dump(s), "/   1/2222");
}

TEST(screen_erases_the_whole_display) {
  Screen s = marked(4, 3);
  s.move_to(2, 1);
  s.erase_display(2);

  CHECK_EQ(dump(s), "//");
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(s.cursor().y, 1);
}

TEST(screen_ignores_an_unknown_erase_display_mode) {
  Screen s = marked(4, 2);
  s.move_to(0, 0);
  s.erase_display(9);

  CHECK_EQ(dump(s), "0000/1111");
}

TEST(screen_erases_from_the_cursor_to_the_end_of_the_line) {
  Screen s = marked(4, 2);
  s.move_to(1, 0);
  s.erase_line(0);

  CHECK_EQ(dump(s), "0/1111");
}

TEST(screen_erases_from_the_start_of_the_line_to_the_cursor) {
  Screen s = marked(4, 2);
  s.move_to(1, 0);
  s.erase_line(1);

  CHECK_EQ(dump(s), "  00/1111");
}

TEST(screen_erases_the_whole_line) {
  Screen s = marked(4, 2);
  s.move_to(1, 0);
  s.erase_line(2);

  CHECK_EQ(dump(s), "/1111");
}

TEST(screen_erases_a_fixed_number_of_cells_without_moving_anything) {
  Screen s = marked(6, 1);
  s.move_to(1, 0);
  s.erase_chars(2);

  // ECH creuse un trou : la fin de ligne NE remonte PAS.
  CHECK_EQ(dump(s), "0  000");
  CHECK_EQ(s.cursor().x, 1);
}

TEST(screen_clamps_an_erase_of_more_cells_than_the_line_holds) {
  Screen s = marked(6, 1);
  s.move_to(4, 0);
  s.erase_chars(999);

  CHECK_EQ(dump(s), "0000");
}

// Un effacement qui tombe au milieu d'une pleine chasse doit l'emporter
// entière. Sinon il reste une moitié, et le rendu peint un demi idéogramme.
TEST(screen_erases_a_wide_character_whole_when_it_cuts_it_in_half) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "b");
  REQUIRE_EQ(s.at(1, 0).width, 2);

  s.move_to(2, 0);  // pile sur la SECONDE moitie
  s.erase_line(0);

  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(s.at(1, 0).width, 1);
  CHECK_EQ(dump(s), "a");
}

TEST(screen_erases_a_wide_character_whole_at_the_far_end_of_a_span) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "b");

  s.move_to(0, 0);
  s.erase_chars(2);  // couvre 0 et 1 : la moitie droite est en 2

  CHECK_EQ(s.at(2, 0).ch, U' ');
  CHECK_EQ(s.at(2, 0).width, 1);
  CHECK_EQ(dump(s), "   b");
}

// ---------------------------------------------------------------------------
// Les éditions : ICH, DCH, IL, DL.
// ---------------------------------------------------------------------------

TEST(screen_inserts_cells_by_pushing_the_rest_of_the_line_right) {
  Screen s(6, 1);
  puts_ascii(s, "abcdef");
  s.move_to(2, 0);
  s.insert_chars(2);

  // "ef" sort de la ligne par la droite et se perd.
  CHECK_EQ(dump(s), "ab  cd");
  CHECK_EQ(s.cursor().x, 2);
}

TEST(screen_deletes_cells_by_pulling_the_rest_of_the_line_left) {
  Screen s(6, 1);
  puts_ascii(s, "abcdef");
  s.move_to(2, 0);
  s.delete_chars(2);

  CHECK_EQ(dump(s), "abef");
}

TEST(screen_treats_an_oversized_insert_as_a_line_erase) {
  Screen s(6, 1);
  puts_ascii(s, "abcdef");
  s.move_to(2, 0);
  s.insert_chars(99);

  CHECK_EQ(dump(s), "ab");
}

TEST(screen_treats_an_oversized_delete_as_a_line_erase) {
  Screen s(6, 1);
  puts_ascii(s, "abcdef");
  s.move_to(2, 0);
  s.delete_chars(99);

  CHECK_EQ(dump(s), "ab");
}

TEST(screen_ignores_an_edit_of_zero_cells) {
  Screen s(6, 1);
  puts_ascii(s, "abcdef");
  s.move_to(2, 0);
  s.insert_chars(0);
  s.delete_chars(0);
  s.erase_chars(0);

  CHECK_EQ(dump(s), "abcdef");
}

TEST(screen_inserts_lines_and_drops_the_bottom_of_the_region) {
  Screen s = marked(4, 4);
  s.move_to(0, 1);
  s.insert_lines(1);

  // La ligne 3 tombe de l'ecran, une ligne vierge s'ouvre en 1.
  CHECK_EQ(dump(s), "0000//1111/2222");
}

TEST(screen_deletes_lines_and_pulls_the_region_up) {
  Screen s = marked(4, 4);
  s.move_to(0, 1);
  s.delete_lines(1);

  CHECK_EQ(dump(s), "0000/2222/3333/");
}

// Le curseur reste où il est : IL et DL ne sont pas des déplacements.
TEST(screen_keeps_the_cursor_where_it_is_across_a_line_edit) {
  Screen s = marked(4, 4);
  s.move_to(2, 1);
  s.insert_lines(1);
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(s.cursor().y, 1);

  s.delete_lines(1);
  CHECK_EQ(s.cursor().x, 2);
  CHECK_EQ(s.cursor().y, 1);
}

// ---------------------------------------------------------------------------
// La région de défilement (DECSTBM).
// ---------------------------------------------------------------------------

TEST(screen_starts_with_a_region_that_covers_the_whole_page) {
  Screen s(4, 5);
  CHECK_EQ(s.scroll_top(), 0);
  CHECK_EQ(s.scroll_bottom(), 4);
}

TEST(screen_scrolls_only_inside_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);
  s.move_to(0, 2);  // au bas de la region
  s.index();

  // Les lignes 0 et 3 ne bougent pas d'un pouce.
  CHECK_EQ(dump(s), "0000/2222//3333");
}

TEST(screen_scrolls_the_region_backwards_at_its_top) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);
  s.move_to(0, 1);  // au sommet de la region
  s.reverse_index();

  CHECK_EQ(dump(s), "0000//1111/3333");
}

// Le curseur va à l'origine de l'écran : c'est ce que fait DECSTBM, et
// c'est ce qui garantit qu'une application ne reste pas posée sur une
// ligne qui vient de changer de sens.
// L'origine visée est celle de l'ÉCRAN, pas celle de la région : DECSTBM
// ramène en (0, 0) même quand la région commence plus bas. C'est le choix
// de xterm tant que DECOM n'est pas armé, et le test le distingue puisque
// la région posée ici démarre à la ligne 1 -- une origine de région
// donnerait y == 1.
TEST(screen_sends_the_cursor_home_when_the_region_changes) {
  Screen s = marked(4, 4);
  s.move_to(3, 3);
  s.set_scroll_region(1, 2);

  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 0);
}

// Corollaire du précédent, écrit pour qu'on ne relise pas ce (0, 0) comme
// un curseur coincé hors de sa région : la région borne le DÉFILEMENT, pas
// les déplacements. On écrit au-dessus d'elle, on y descend, et rien ne
// défile tant qu'on n'a pas atteint son bas.
TEST(screen_lets_the_cursor_work_above_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);

  s.print(U'X');
  CHECK_EQ(s.cursor().x, 1);
  CHECK_EQ(dump(s), "X000/1111/2222/3333");

  s.carriage_return();
  s.line_feed();
  CHECK_EQ(s.cursor().y, 1);
  CHECK_EQ(dump(s), "X000/1111/2222/3333");
}

TEST(screen_refuses_a_region_that_is_upside_down) {
  Screen s(4, 4);
  s.set_scroll_region(1, 2);
  s.set_scroll_region(3, 1);  // refusee

  CHECK_EQ(s.scroll_top(), 1);
  CHECK_EQ(s.scroll_bottom(), 2);
}

TEST(screen_refuses_a_region_of_a_single_line) {
  Screen s(4, 4);
  s.set_scroll_region(2, 2);

  // Une region d'une ligne ne peut pas defiler : xterm la refuse, et sans
  // ce refus un LF au mauvais endroit effacerait la ligne a chaque tour.
  CHECK_EQ(s.scroll_top(), 0);
  CHECK_EQ(s.scroll_bottom(), 3);
}

TEST(screen_clamps_a_region_that_runs_past_the_bottom) {
  Screen s(4, 4);
  s.set_scroll_region(1, 99);

  CHECK_EQ(s.scroll_top(), 1);
  CHECK_EQ(s.scroll_bottom(), 3);
}

TEST(screen_reopens_the_region_to_the_whole_page) {
  Screen s(4, 4);
  s.set_scroll_region(1, 2);
  s.reset_scroll_region();

  CHECK_EQ(s.scroll_top(), 0);
  CHECK_EQ(s.scroll_bottom(), 3);
}

// Sous la région, LF descend sans jamais faire défiler quoi que ce soit :
// le curseur s'arrête au bas de l'écran.
TEST(screen_does_not_scroll_when_the_cursor_sits_below_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(0, 1);
  s.move_to(0, 3);
  s.index();

  CHECK_EQ(dump(s), "0000/1111/2222/3333");
  CHECK_EQ(s.cursor().y, 3);
}

TEST(screen_ignores_a_line_insert_outside_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(0, 1);
  s.move_to(0, 3);  // sous la region
  s.insert_lines(1);
  s.delete_lines(1);

  CHECK_EQ(dump(s), "0000/1111/2222/3333");
}

// IL au bas de la région se réduit à effacer la ligne : ce qu'elle pousse
// sort immédiatement de la région.
TEST(screen_inserting_at_the_bottom_of_the_region_just_blanks_the_line) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);
  s.move_to(0, 2);
  s.insert_lines(1);

  CHECK_EQ(dump(s), "0000/1111//3333");
}

TEST(screen_line_edits_stop_at_the_bottom_of_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);
  s.move_to(0, 1);
  s.delete_lines(1);

  // Seules 1 et 2 tournent ; 3 reste dehors.
  CHECK_EQ(dump(s), "0000/2222//3333");
}

// ---------------------------------------------------------------------------
// DECSC / DECRC.
// ---------------------------------------------------------------------------

TEST(screen_restores_the_cursor_it_saved) {
  Screen s(10, 4);
  s.move_to(5, 2);
  s.save_cursor();
  s.move_to(0, 0);
  s.restore_cursor();

  CHECK_EQ(s.cursor().x, 5);
  CHECK_EQ(s.cursor().y, 2);
}

// Le retour différé fait partie de l'état du curseur : le perdre ferait
// sauter une ligne à la reprise.
TEST(screen_restores_the_pending_wrap_it_saved) {
  Screen s(4, 3);
  puts_ascii(s, "abcd");
  REQUIRE(s.wrap_pending());
  s.save_cursor();

  s.move_to(0, 0);
  REQUIRE(!s.wrap_pending());
  s.restore_cursor();

  CHECK(s.wrap_pending());
  CHECK_EQ(s.cursor().x, 3);

  // Et il se consomme normalement : le prochain caractere descend.
  s.print(U'z');
  CHECK_EQ(dump(s), "abcd/z/");
}

TEST(screen_restores_to_the_origin_when_nothing_was_saved) {
  Screen s(10, 4);
  s.move_to(5, 2);
  s.restore_cursor();

  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 0);
}

TEST(screen_clamps_a_restored_cursor_that_no_longer_fits) {
  Screen s(10, 4);
  s.move_to(9, 3);
  s.save_cursor();
  s.restore_cursor();

  CHECK_EQ(s.cursor().x, 9);
  CHECK_EQ(s.cursor().y, 3);
}

// Une édition qui déplace des cellules doit briser les paires pleine
// chasse qu'elle coupe, sinon une moitié voyage sans l'autre et le rendu
// peint un demi idéogramme quelque part sur la ligne.
TEST(screen_breaks_a_wide_pair_the_insert_cuts_in_half) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "bc");
  REQUIRE_EQ(s.at(1, 0).width, 2);

  s.move_to(2, 0);  // sur la SECONDE moitie
  s.insert_chars(1);

  // Les deux moities sont parties ensemble.
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(s.at(1, 0).width, 1);
  CHECK_EQ(dump(s), "a   bc");
}

TEST(screen_breaks_a_wide_pair_the_delete_cuts_in_half) {
  Screen s(6, 1);
  puts_ascii(s, "ab");
  s.print(kWide);  // colonnes 2 et 3
  puts_ascii(s, "c");

  s.move_to(1, 0);
  s.delete_chars(2);  // emporte 'b' et la premiere moitie

  // DCH supprime DEUX colonnes, pas trois : la moitie survivante devient un
  // blanc et se decale avec le reste. Fermer le trou entierement
  // reviendrait a supprimer une colonne de plus que demande.
  CHECK_EQ(dump(s), "a c");
  for (int x = 0; x < 6; ++x) CHECK_EQ(s.at(x, 0).width, 1);
}

// Poussée contre le bord droit, une pleine chasse perd sa seconde moitié
// hors de la ligne : la première ne doit pas rester seule.
TEST(screen_does_not_leave_a_half_wide_against_the_right_edge) {
  Screen s(6, 1);
  puts_ascii(s, "abc");
  s.print(kWide);  // colonnes 3 et 4
  REQUIRE_EQ(s.at(3, 0).width, 2);

  s.move_to(0, 0);
  s.insert_chars(1);  // la premiere moitie arrive en 4, la seconde en 5

  // Elle tient encore : rien ne doit avoir change de nature.
  CHECK_EQ(s.at(4, 0).width, 2);
  CHECK_EQ(s.at(5, 0).width, 0);

  s.move_to(0, 0);
  s.insert_chars(1);  // cette fois la seconde moitie tombe de la ligne
  CHECK_EQ(s.at(5, 0).ch, U' ');
  CHECK_EQ(s.at(5, 0).width, 1);
}


// Un ECH de zéro cellule ne doit RIEN toucher -- pas même la moitié du
// caractère large sur lequel le curseur se trouve. C'est le refus de la
// plage vide, dans erase_span, qui l'assure : il vient AVANT le nettoyage
// des bords, sinon un effacement de rien casserait une paire.
TEST(screen_erasing_zero_cells_spares_the_wide_pair_under_the_cursor) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "b");
  REQUIRE_EQ(s.at(1, 0).width, 2);

  s.move_to(2, 0);  // sur la seconde moitie
  s.erase_chars(0);

  CHECK_EQ(s.at(1, 0).width, 2);
  CHECK_EQ(s.at(1, 0).ch, kWide);
  CHECK_EQ(s.at(2, 0).width, 0);
}

// DCH posé sur la seconde moitié d'un caractère large : sa première moitié
// ne doit pas rester seule derrière le décalage.
TEST(screen_breaks_the_wide_pair_under_the_delete_cursor) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "bc");

  s.move_to(2, 0);
  s.delete_chars(1);

  CHECK_EQ(s.at(1, 0).width, 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(dump(s), "a bc");
}

// Le curseur sur la PREMIÈRE moitié d'une paire, cette fois : si l'une des
// deux moitiés survit à la rupture, le décalage la transporte, et il reste
// un caractère large déclaré sur deux colonnes dont la seconde est un
// blanc ordinaire -- une grille qui ment sur sa propre géométrie.
TEST(screen_carries_no_half_wide_through_an_insert) {
  Screen s(6, 1);
  puts_ascii(s, "a");
  s.print(kWide);  // colonnes 1 et 2
  puts_ascii(s, "b");
  REQUIRE_EQ(s.at(1, 0).width, 2);

  s.move_to(1, 0);  // sur la PREMIERE moitie
  s.insert_chars(2);

  CHECK_EQ(dump(s), "a    b");
  for (int x = 0; x < 6; ++x) CHECK_EQ(s.at(x, 0).width, 1);
}

// Au-DESSUS de la région, l'édition de lignes ne fait rien non plus. Ce
// cas-là est le vrai enjeu de la garde : sous la région, la tranche part à
// l'envers et le défilement la refuse tout seul ; au-dessus, elle serait
// parfaitement valide et emporterait des lignes que l'application a
// justement déclarées fixes.
TEST(screen_ignores_a_line_edit_above_the_region) {
  Screen s = marked(4, 4);
  s.set_scroll_region(2, 3);
  s.move_to(0, 0);

  s.insert_lines(1);
  CHECK_EQ(dump(s), "0000/1111/2222/3333");

  s.delete_lines(1);
  CHECK_EQ(dump(s), "0000/1111/2222/3333");
}

// ---------------------------------------------------------------------------
// Le stylo. La grille porte un style par cellule, et `print()` y dépose
// celui du stylo courant. Sans lui, `apply_sgr` n'aurait aucun appelant :
// l'invité écrirait ses couleurs dans le vide.
// ---------------------------------------------------------------------------

namespace {

// Un stylo reconnaissable : les trois champs sont distincts, pour qu'un
// style à moitié recopié se voie.
sshos::Style painted_pen() {
  sshos::Style p;
  p.fg = sshos::Color::indexed(2);
  p.bg = sshos::Color::indexed(4);
  p.attrs = sshos::attr::Bold | sshos::attr::Underline;
  return p;
}

}  // namespace

TEST(screen_starts_with_a_blank_pen_on_a_blank_grid) {
  Screen s(4, 2);
  CHECK(s.pen() == sshos::Style{});
  CHECK(s.at(0, 0).style == sshos::Style{});
}

TEST(screen_paints_what_it_prints_with_the_current_pen) {
  Screen s(4, 2);
  s.set_pen(painted_pen());

  puts_ascii(s, "ab");
  CHECK(s.at(0, 0).style == painted_pen());
  CHECK(s.at(1, 0).style == painted_pen());
  // Ce qui n'a pas été écrit n'a pas été peint.
  CHECK(s.at(2, 0).style == sshos::Style{});
}

// Le stylo n'est pas un calque : changer de couleur ne repeint pas ce qui
// est déjà à l'écran. C'est la différence entre un terminal et un tableau.
TEST(screen_leaves_what_is_already_printed_alone_when_the_pen_changes) {
  Screen s(4, 2);
  s.set_pen(painted_pen());
  puts_ascii(s, "a");

  sshos::Style other;
  other.fg = sshos::Color::indexed(1);
  s.set_pen(other);
  puts_ascii(s, "b");

  CHECK(s.at(0, 0).style == painted_pen());
  CHECK(s.at(1, 0).style == other);
}

// Les deux moitiés d'une pleine chasse portent le MÊME style : la moitié
// droite est peinte par le rendu comme n'importe quelle cellule, et un
// fond qui s'arrêterait au milieu de l'idéogramme se verrait.
TEST(screen_gives_both_halves_of_a_wide_character_the_same_style) {
  Screen s(4, 2);
  s.set_pen(painted_pen());

  s.print(kWide);
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(s.at(1, 0).width, 0);
  CHECK(s.at(0, 0).style == painted_pen());
  CHECK(s.at(1, 0).style == painted_pen());
}

// ---------------------------------------------------------------------------
// L'effacement peint le FOND COURANT (« background colour erase »). Le
// terminfo d'xterm-256color, que nous promettons à l'invité, porte la
// capacité `bce` : une application qui pose son fond puis efface attend
// que la zone effacée prenne ce fond.
// ---------------------------------------------------------------------------

TEST(screen_erases_the_display_with_the_current_background) {
  Screen s = marked(4, 3);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.erase_display(2);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      CHECK(s.at(x, y).style.bg == sshos::Color::indexed(4));
    }
  }
}

// Le fond SEUL. Une cellule vide n'a pas de glyphe : lui recopier le
// premier plan ne se verrait pas, mais lui recopier les attributs
// soulignerait le vide.
TEST(screen_erases_with_the_background_alone_and_not_the_rest_of_the_pen) {
  Screen s = marked(4, 3);
  s.set_pen(painted_pen());

  s.move_to(0, 0);
  s.erase_chars(2);

  sshos::Style expected;
  expected.bg = sshos::Color::indexed(4);
  CHECK(s.at(0, 0).style == expected);
  CHECK(s.at(1, 0).style == expected);
}

// Une ligne qui entre par défilement est une ligne effacée : elle prend le
// fond courant elle aussi. C'est ce qui donne une marge de couleur uniforme
// quand un pager fait défiler sa page.
TEST(screen_scrolls_in_a_line_painted_with_the_current_background) {
  Screen s = marked(4, 3);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(0, 2);
  s.line_feed();  // au bas de la région : l'écran tourne

  for (int x = 0; x < 4; ++x) {
    CHECK(s.at(x, 2).style.bg == sshos::Color::indexed(4));
  }
  // Ce qui a seulement remonté garde son style d'origine.
  CHECK(s.at(0, 0).style == sshos::Style{});
}

TEST(screen_opens_an_insert_gap_in_the_current_background) {
  Screen s = marked(4, 2);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(1, 0);
  s.insert_chars(2);

  CHECK(s.at(1, 0).style.bg == sshos::Color::indexed(4));
  CHECK(s.at(2, 0).style.bg == sshos::Color::indexed(4));
  // La cellule poussée à droite n'a pas été effacée : elle garde le sien.
  CHECK(s.at(3, 0).style == sshos::Style{});
}

TEST(screen_leaves_the_current_background_behind_a_delete) {
  Screen s = marked(4, 2);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(0, 0);
  s.delete_chars(2);

  CHECK(s.at(2, 0).style.bg == sshos::Color::indexed(4));
  CHECK(s.at(3, 0).style.bg == sshos::Color::indexed(4));
  CHECK(s.at(0, 0).style == sshos::Style{});
}

TEST(screen_inserts_a_line_painted_with_the_current_background) {
  Screen s = marked(4, 3);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(0, 0);
  s.insert_lines(1);

  for (int x = 0; x < 4; ++x) {
    CHECK(s.at(x, 0).style.bg == sshos::Color::indexed(4));
  }
}

// ---------------------------------------------------------------------------
// DECSC / DECRC. La dette de la tâche 4 : « DECSC sauve curseur ET
// attributs », reporté ici faute d'attributs à sauver.
// ---------------------------------------------------------------------------

TEST(screen_saves_the_pen_with_the_cursor_and_gives_it_back) {
  Screen s(8, 3);
  s.set_pen(painted_pen());
  s.save_cursor();

  sshos::Style other;
  other.fg = sshos::Color::indexed(1);
  s.set_pen(other);

  s.restore_cursor();
  CHECK(s.pen() == painted_pen());
}

// Un DECRC sans DECSC rend un terminal fraîchement allumé : le curseur à
// l'origine ET le stylo vierge.
TEST(screen_restores_a_blank_pen_when_nothing_was_saved) {
  Screen s(8, 3);
  s.set_pen(painted_pen());

  s.restore_cursor();
  CHECK(s.pen() == sshos::Style{});
}

// Les quatre chemins d'effacement que les mutations ont montrés à
// découvert. Chacun peint des cellules que ni ED, ni EL, ni le défilement
// d'une seule ligne n'atteignent -- et chacun laissait donc passer un
// effacement qui oubliait le fond courant.

// Quand une tranche sort ENTIÈRE, le défilement ne fait rien tourner : il
// remplit d'un coup. C'est un chemin à part, et il doit peindre comme
// l'autre.
TEST(screen_scrolls_a_whole_region_out_in_the_current_background) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);  // deux lignes de haut
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(0, 1);
  s.delete_lines(2);  // autant que la région : tout sort

  for (int y = 1; y <= 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      CHECK(s.at(x, y).style.bg == sshos::Color::indexed(4));
    }
  }
  CHECK_EQ(dump(s), "0000///3333");
}

TEST(screen_scrolls_a_whole_region_backwards_in_the_current_background) {
  Screen s = marked(4, 4);
  s.set_scroll_region(1, 2);
  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);

  s.move_to(0, 1);
  s.insert_lines(2);

  for (int y = 1; y <= 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      CHECK(s.at(x, y).style.bg == sshos::Color::indexed(4));
    }
  }
}

// La moitié orpheline d'une pleine chasse qu'on recouvre est effacée, donc
// peinte du fond courant : sans cela, écrire sur un idéogramme laisse à
// côté de lui un trou de la couleur d'avant.
TEST(screen_repaints_the_orphan_half_of_a_wide_character_it_covers) {
  Screen s(6, 1);
  s.print(kWide);  // colonnes 0 et 1
  REQUIRE_EQ(s.at(0, 0).width, 2);

  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);
  s.move_to(0, 0);
  s.print(U'a');  // recouvre la moitié gauche ; la droite reste orpheline

  CHECK_EQ(s.at(1, 0).width, 1);
  CHECK(s.at(1, 0).style.bg == sshos::Color::indexed(4));
}

// Une édition qui coupe une paire en deux l'emporte ENTIÈRE, et les deux
// cellules libérées sont des cellules effacées comme les autres.
TEST(screen_repaints_the_wide_pair_an_edit_breaks) {
  Screen s(6, 1);
  s.print(kWide);  // colonnes 0 et 1

  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);
  s.move_to(1, 0);  // sur la SECONDE moitié
  s.insert_chars(1);

  CHECK_EQ(s.at(0, 0).width, 1);
  CHECK(s.at(0, 0).style.bg == sshos::Color::indexed(4));
}

// Poussée contre le bord droit, une pleine chasse perd sa seconde moitié
// hors de la ligne ; la première est effacée sur place -- avec le fond
// courant, comme tout effacement.
TEST(screen_repaints_a_half_wide_left_against_the_right_edge) {
  Screen s(6, 1);
  puts_ascii(s, "abc");
  s.print(kWide);  // colonnes 3 et 4
  s.move_to(0, 0);
  s.insert_chars(1);  // la paire glisse en 4 et 5
  REQUIRE_EQ(s.at(4, 0).width, 2);

  sshos::Style p;
  p.bg = sshos::Color::indexed(4);
  s.set_pen(p);
  s.move_to(0, 0);
  s.insert_chars(1);  // la seconde moitié tombe de la ligne

  CHECK_EQ(s.at(5, 0).width, 1);
  CHECK(s.at(5, 0).style.bg == sshos::Color::indexed(4));
}
