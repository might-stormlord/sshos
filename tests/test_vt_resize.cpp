#include <string>

#include "harness.hpp"
#include "vt/screen.hpp"
#include "vt/scrollback.hpp"

using sshos::Screen;
using sshos::Scrollback;

namespace {

void puts_ascii(Screen& s, const std::string& text) {
  for (char c : text) s.print(static_cast<char32_t>(c));
}

// Une grille marquée : chaque ligne porte son chiffre répété, de sorte
// qu'une transcription dise d'un coup d'œil ce qui a été coupé et où.
Screen marked(int cols, int rows) {
  Screen s(cols, rows);
  for (int y = 0; y < rows; ++y) {
    s.move_to(0, y);
    for (int x = 0; x < cols; ++x) s.print(static_cast<char32_t>(U'0' + y));
  }
  return s;
}

std::string dump(const Screen& s) {
  std::string out;
  for (int y = 0; y < s.rows(); ++y) {
    if (y != 0) out.push_back('/');
    out += s.line_text(y);
  }
  return out;
}

constexpr char32_t kWide = U'一';

}  // namespace

// ------------------------------------------------------------ la géométrie

TEST(resize_takes_the_new_size) {
  Screen s(8, 3);
  s.resize(12, 5);

  CHECK_EQ(s.cols(), 12);
  CHECK_EQ(s.rows(), 5);
}

// Une taille dégénérée est ramenée à un, comme au constructeur : une
// grille de zéro colonne n'a pas de sens et ferait diviser par zéro.
TEST(resize_refuses_a_degenerate_size) {
  Screen s(8, 3);
  s.resize(0, -4);

  CHECK_EQ(s.cols(), 1);
  CHECK_EQ(s.rows(), 1);
}

TEST(resize_keeps_what_still_fits_when_it_grows) {
  Screen s = marked(4, 2);
  s.resize(6, 3);

  CHECK_EQ(s.line_text(0), std::string("0000"));
  CHECK_EQ(s.line_text(1), std::string("1111"));
  CHECK_EQ(s.line_text(2), std::string(""));
}

// Les cellules neuves sont des cellules effacées : elles prennent le fond
// courant, comme tout ce qui s'efface (`bce`).
TEST(resize_paints_the_new_cells_with_the_current_background) {
  Screen s = marked(4, 2);
  sshos::Style pen;
  pen.bg = sshos::Color::indexed(4);
  s.set_pen(pen);

  s.resize(6, 3);
  CHECK(s.at(5, 0).style.bg == sshos::Color::indexed(4));
  CHECK(s.at(0, 2).style.bg == sshos::Color::indexed(4));
}

TEST(resize_truncates_what_no_longer_fits) {
  Screen s = marked(6, 4);
  s.resize(3, 2);

  CHECK_EQ(dump(s), "000/111");
}

// LA POLITIQUE, figée par ce test : rétrécir PERD ce qui dépasse, et
// réélargir ne le rend pas. Ce n'est pas un défaut -- c'est le refus du
// reflow, qui invaliderait la position du curseur et tous les décalages de
// l'historique.
TEST(resize_does_not_give_back_what_it_truncated) {
  Screen s = marked(6, 2);
  s.resize(3, 2);
  s.resize(6, 2);

  CHECK_EQ(s.line_text(0), std::string("000"));
}

// Une pleine chasse coupée en deux par la troncature laisserait une moitié
// seule contre le bord droit.
TEST(resize_does_not_leave_half_a_wide_character_at_the_edge) {
  Screen s(6, 1);
  puts_ascii(s, "abc");
  s.print(kWide);  // colonnes 3 et 4
  REQUIRE_EQ(s.at(3, 0).width, 2);

  s.resize(4, 1);  // la coupe tombe entre les deux moitiés
  CHECK_EQ(s.at(3, 0).width, 1);
  CHECK_EQ(s.line_text(0), std::string("abc"));
}

// ---------------------------------------------------------- ce qui se remet

TEST(resize_keeps_the_cursor_inside_the_new_bounds) {
  Screen s = marked(8, 4);
  s.move_to(7, 3);

  s.resize(4, 2);
  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 1);
}

// Le retour en attente parlait d'une géométrie qui n'existe plus.
TEST(resize_forgets_a_pending_wrap) {
  Screen s(4, 2);
  puts_ascii(s, "abcd");
  REQUIRE(s.wrap_pending());

  s.resize(6, 2);
  CHECK(!s.wrap_pending());
}

// La région est remise à pleine hauteur à TOUT changement de taille : une
// région qui survivrait à un rétrécissement pourrait tomber hors page.
TEST(resize_puts_the_scroll_region_back_to_full_height) {
  Screen s(8, 6);
  s.set_scroll_region(2, 4);

  s.resize(8, 8);
  CHECK_EQ(s.scroll_top(), 0);
  CHECK_EQ(s.scroll_bottom(), 7);
}

// « À tout CHANGEMENT » : redemander la taille qu'on a déjà n'en est pas
// un, et ne doit pas balayer la région que l'invité vient de poser.
TEST(resize_to_the_same_size_leaves_the_region_alone) {
  Screen s(8, 6);
  s.set_scroll_region(2, 4);

  s.resize(8, 6);
  CHECK_EQ(s.scroll_top(), 2);
  CHECK_EQ(s.scroll_bottom(), 4);
}

// Les taquets sont indexés par colonne : changer la largeur les refait.
TEST(resize_rebuilds_the_tab_stops_when_the_width_changes) {
  Screen s(24, 2);
  s.move_to(3, 0);
  s.set_tab();

  s.resize(32, 2);
  s.move_to(0, 0);
  s.tab();
  CHECK_EQ(s.cursor().x, 8);  // le taquet posé en 3 a disparu
}

// Changer la seule hauteur ne les touche pas : rien n'a bougé de leur axe.
TEST(resize_keeps_the_tab_stops_when_only_the_height_changes) {
  Screen s(24, 2);
  s.move_to(3, 0);
  s.set_tab();

  s.resize(24, 4);
  s.move_to(0, 0);
  s.tab();
  CHECK_EQ(s.cursor().x, 3);
}

// --------------------------------------------------------- les deux pages

// L'écran alterné est JETÉ : l'invité le régénère en recevant SIGWINCH, et
// tenter de le recoudre ne ferait que lui livrer un écran à moitié faux
// avant qu'il ne le redessine.
TEST(resize_throws_the_alternate_page_away) {
  Screen s(8, 3);
  s.enter_alt_screen();
  puts_ascii(s, "vim");

  s.resize(6, 3);
  CHECK_EQ(s.line_text(0), std::string(""));
}

// La page principale, elle, ATTEND dehors : elle est recoupée comme si
// elle était à l'écran, et revient telle quelle.
TEST(resize_reshapes_the_main_page_waiting_under_the_alternate) {
  Screen s = marked(6, 2);
  s.enter_alt_screen();

  s.resize(3, 2);
  s.leave_alt_screen();
  CHECK_EQ(dump(s), "000/111");
}

// Le curseur que 1049 garde de côté doit revenir dans les bornes lui
// aussi : il a été sauvé dans une géométrie qui n'existe plus.
TEST(resize_brings_the_parked_cursor_back_inside) {
  Screen s(8, 4);
  s.move_to(7, 3);
  s.enter_alt_screen();

  s.resize(4, 2);
  s.leave_alt_screen();
  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 1);
}

// ----------------------------------------------------------- l'historique

// Pas de reflow : ce qui est déjà rangé garde la largeur qu'il avait. Le
// recoudre invaliderait tous les décalages de consultation.
TEST(resize_leaves_the_history_alone) {
  Screen s(8, 2);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  puts_ascii(s, "12345678");
  s.move_to(0, 1);
  s.line_feed();
  REQUIRE_EQ(sb.size(), size_t{1});

  s.resize(3, 2);
  CHECK_EQ(sb.at(0).size(), size_t{8});
}
