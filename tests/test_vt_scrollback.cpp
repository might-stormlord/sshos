#include <string>
#include <vector>

#include "harness.hpp"
#include "vt/screen.hpp"
#include "vt/scrollback.hpp"

using sshos::Screen;
using sshos::ScreenCell;
using sshos::Scrollback;
using sshos::ScrollbackLine;

namespace {

// Une ligne de grille fabriquée à la main, blancs de fin compris. Les
// tests du rognage ont besoin de dire EXACTEMENT ce qu'on leur donne.
std::vector<ScreenCell> row(const std::string& text, size_t cols) {
  std::vector<ScreenCell> line(cols);
  for (size_t i = 0; i < text.size() && i < cols; ++i) {
    line[i].ch = static_cast<char32_t>(text[i]);
  }
  return line;
}

// Le texte d'une ligne d'historique, pour comparer d'un coup d'œil.
std::string text_of(const ScrollbackLine& line) {
  std::string out;
  for (const ScreenCell& c : line) {
    if (c.width == 0) continue;
    out.push_back(static_cast<char>(c.ch));
  }
  return out;
}

void push_text(Scrollback& sb, const std::string& text, size_t cols = 8) {
  const std::vector<ScreenCell> line = row(text, cols);
  sb.push(line.data(), line.size());
}

void puts_ascii(Screen& s, const std::string& text) {
  for (char c : text) s.print(static_cast<char32_t>(c));
}

}  // namespace

// -------------------------------------------------------------- le tampon

TEST(scrollback_starts_empty) {
  const Scrollback sb(4);
  CHECK_EQ(sb.size(), size_t{0});
  CHECK_EQ(sb.capacity(), size_t{4});
  CHECK(sb.empty());
}

TEST(scrollback_keeps_the_lines_it_is_given_in_order) {
  Scrollback sb(4);
  push_text(sb, "un");
  push_text(sb, "deux");

  REQUIRE_EQ(sb.size(), size_t{2});
  CHECK_EQ(text_of(sb.at(0)), std::string("un"));
  CHECK_EQ(text_of(sb.at(1)), std::string("deux"));
}

// Le tampon est CIRCULAIRE : au-delà de sa capacité, c'est la plus
// ancienne qui s'en va, et l'index 0 suit.
TEST(scrollback_drops_the_oldest_line_past_its_capacity) {
  Scrollback sb(3);
  push_text(sb, "a");
  push_text(sb, "b");
  push_text(sb, "c");
  push_text(sb, "d");

  CHECK_EQ(sb.size(), size_t{3});
  CHECK_EQ(text_of(sb.at(0)), std::string("b"));
  CHECK_EQ(text_of(sb.at(2)), std::string("d"));
}

// Le tour complet : après avoir dépassé la capacité d'un tour entier,
// l'ordre doit encore être le bon. C'est là qu'un index circulaire mal
// écrit se voit.
TEST(scrollback_keeps_the_order_after_a_full_turn) {
  Scrollback sb(3);
  for (const char* s : {"a", "b", "c", "d", "e", "f", "g"}) push_text(sb, s);

  CHECK_EQ(text_of(sb.at(0)), std::string("e"));
  CHECK_EQ(text_of(sb.at(1)), std::string("f"));
  CHECK_EQ(text_of(sb.at(2)), std::string("g"));
}

TEST(scrollback_reads_nothing_out_of_its_bounds) {
  Scrollback sb(3);
  push_text(sb, "a");

  CHECK(sb.at(1).empty());
  CHECK(sb.at(99).empty());
}

TEST(scrollback_forgets_everything_on_clear) {
  Scrollback sb(3);
  push_text(sb, "a");
  sb.scroll_back(1);

  sb.clear();
  CHECK(sb.empty());
  CHECK_EQ(sb.offset(), size_t{0});
}

// Une capacité nulle désactive l'historique. Il ne doit RIEN garder, et
// surtout pas diviser par sa capacité pour trouver un emplacement.
TEST(scrollback_stores_nothing_when_its_capacity_is_zero) {
  Scrollback sb(0);
  push_text(sb, "a");

  CHECK(sb.empty());
  CHECK(sb.at(0).empty());
}

// --------------------------------------------------------------- le rognage

TEST(scrollback_trims_the_blanks_at_the_end_of_a_line) {
  Scrollback sb(2);
  push_text(sb, "abc", 8);

  REQUIRE_EQ(sb.size(), size_t{1});
  CHECK_EQ(sb.at(0).size(), size_t{3});
}

// Ce qui est rogné est le blanc PAR DÉFAUT, pas l'espace. Un blanc au
// milieu d'une ligne est du texte.
TEST(scrollback_keeps_a_blank_in_the_middle_of_a_line) {
  Scrollback sb(2);
  push_text(sb, "a b", 8);

  CHECK_EQ(sb.at(0).size(), size_t{3});
  CHECK_EQ(text_of(sb.at(0)), std::string("a b"));
}

// Une cellule vide mais PEINTE est du contenu visible. La rogner ferait
// disparaître la marge de couleur d'une application en remontant dans
// l'historique.
TEST(scrollback_keeps_a_painted_blank_at_the_end_of_a_line) {
  std::vector<ScreenCell> line = row("ab", 8);
  line[7].style.bg = sshos::Color::indexed(4);

  Scrollback sb(2);
  sb.push(line.data(), line.size());

  CHECK_EQ(sb.at(0).size(), size_t{8});
  CHECK(sb.at(0)[7].style.bg == sshos::Color::indexed(4));
}

TEST(scrollback_stores_an_entirely_blank_line_as_an_empty_one) {
  Scrollback sb(2);
  push_text(sb, "", 8);

  CHECK_EQ(sb.size(), size_t{1});
  CHECK(sb.at(0).empty());
}

// --------------------------------------------------------- la consultation

TEST(scrollback_starts_at_the_bottom) {
  Scrollback sb(4);
  push_text(sb, "a");
  CHECK_EQ(sb.offset(), size_t{0});
}

TEST(scrollback_does_not_scroll_past_the_top) {
  Scrollback sb(4);
  push_text(sb, "a");
  push_text(sb, "b");

  sb.scroll_back(99);
  CHECK_EQ(sb.offset(), size_t{2});
}

TEST(scrollback_does_not_scroll_past_the_bottom) {
  Scrollback sb(4);
  push_text(sb, "a");
  sb.scroll_back(1);

  sb.scroll_forward(99);
  CHECK_EQ(sb.offset(), size_t{0});
}

TEST(scrollback_comes_straight_back_to_the_bottom) {
  Scrollback sb(4);
  push_text(sb, "a");
  push_text(sb, "b");
  sb.scroll_back(2);

  sb.scroll_to_bottom();
  CHECK_EQ(sb.offset(), size_t{0});
}

// Collé au présent, on suit la sortie : c'est le cas normal, et le
// décalage ne bouge pas.
TEST(scrollback_follows_the_live_output_when_it_sits_at_the_bottom) {
  Scrollback sb(4);
  push_text(sb, "a");
  push_text(sb, "b");

  CHECK_EQ(sb.offset(), size_t{0});
}

// REMONTÉ dans l'historique, on reste sur CE QU'ON LIT. Sans cela, une
// compilation qui continue de cracher des lignes emporte sous les yeux
// l'erreur qu'on était en train de lire -- le décalage compte depuis le
// bas, il doit donc grandir d'autant que la sortie.
TEST(scrollback_stays_on_what_it_shows_when_new_lines_arrive) {
  Scrollback sb(10);
  push_text(sb, "a");
  push_text(sb, "b");
  sb.scroll_back(2);
  REQUIRE_EQ(sb.offset(), size_t{2});

  push_text(sb, "c");
  CHECK_EQ(sb.offset(), size_t{3});
}

// Sauf quand il n'y a plus rien à quoi se raccrocher : tampon plein et
// consultation tout en haut, la ligne regardée s'en va pour de bon.
TEST(scrollback_offset_cannot_outlive_the_lines_it_points_at) {
  Scrollback sb(2);
  push_text(sb, "a");
  push_text(sb, "b");
  sb.scroll_back(2);
  REQUIRE_EQ(sb.offset(), size_t{2});

  push_text(sb, "c");
  CHECK_EQ(sb.offset(), size_t{2});
}

// ------------------------------------------------- ce que la grille pousse

TEST(screen_pushes_the_line_that_scrolls_off_the_top) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  puts_ascii(s, "une");
  s.move_to(0, 2);
  s.line_feed();  // au bas de la page : la page entière défile

  REQUIRE_EQ(sb.size(), size_t{1});
  CHECK_EQ(text_of(sb.at(0)), std::string("une"));
}

// L'écran alterné n'alimente JAMAIS l'historique : le défilement de
// `vim` n'appartient pas à celui du shell.
TEST(screen_pushes_nothing_from_the_alternate_page) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  s.enter_alt_screen();
  puts_ascii(s, "vim");
  s.move_to(0, 2);
  s.line_feed();

  CHECK(sb.empty());
}

// Une région de défilement est une zone que l'application s'est réservée.
// Ce qui en sort n'est pas sorti de la PAGE : une application à en-tête
// fixe empoisonnerait l'historique du shell à chaque rafraîchissement.
TEST(screen_pushes_nothing_when_only_a_region_scrolls) {
  Screen s(8, 4);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  s.set_scroll_region(1, 2);
  s.move_to(0, 1);
  puts_ascii(s, "dedans");
  s.move_to(0, 2);
  s.line_feed();

  CHECK(sb.empty());
}

// `IL` et `DL` déplacent des lignes, ils ne font pas défiler la page.
TEST(screen_pushes_nothing_on_a_line_edit) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  puts_ascii(s, "une");
  s.move_to(0, 0);
  s.delete_lines(1);
  s.insert_lines(1);

  CHECK(sb.empty());
}

// Ce qui sort par le BAS est perdu : l'historique est au-dessus, pas
// en-dessous.
TEST(screen_pushes_nothing_when_it_scrolls_backwards) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  puts_ascii(s, "une");
  s.move_to(0, 0);
  s.reverse_index();

  CHECK(sb.empty());
}

// Le style part avec la ligne : remonter dans l'historique doit rendre
// les couleurs, pas du texte nu.
TEST(screen_pushes_the_style_along_with_the_text) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  sshos::Style pen;
  pen.fg = sshos::Color::indexed(2);
  pen.attrs = sshos::attr::Bold;
  s.set_pen(pen);
  puts_ascii(s, "une");

  s.move_to(0, 2);
  s.line_feed();

  REQUIRE_EQ(sb.size(), size_t{1});
  CHECK(sb.at(0)[0].style == pen);
}

// Quatre trous que seules les mutations ont montrés.

// La tête n'est PAS remise à zéro par un oubli, et n'a pas à l'être :
// tout se lit relativement à elle. Encore faut-il que l'écriture parte du
// même endroit -- après un tour complet suivi d'un oubli, elle ne pointe
// plus sur zéro, et une écriture qui l'ignorerait rangerait la ligne là
// où personne ne la relira.
TEST(scrollback_writes_where_it_reads_after_a_clear) {
  Scrollback sb(2);
  push_text(sb, "a");
  push_text(sb, "b");
  push_text(sb, "c");  // plein et dépassé : la tête a avancé

  sb.clear();
  push_text(sb, "z");

  REQUIRE_EQ(sb.size(), size_t{1});
  CHECK_EQ(text_of(sb.at(0)), std::string("z"));
}

// La capacité par défaut est celle de la spec. Aucun test ne s'en servait,
// et la mettre à zéro -- ce qui désactive l'historique de tout terminal
// construit sans argument -- ne se voyait nulle part.
TEST(scrollback_defaults_to_the_capacity_of_the_spec) {
  Scrollback sb;
  CHECK_EQ(sb.capacity(), sshos::kDefaultScrollbackLines);

  push_text(sb, "a");
  CHECK_EQ(sb.size(), size_t{1});
}

// Une région qui touche le HAUT sans toucher le bas n'est toujours pas la
// page : ce qui en sort reste dans la page.
TEST(screen_pushes_nothing_when_the_region_stops_short_of_the_bottom) {
  Screen s(8, 4);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  s.set_scroll_region(0, 2);
  s.move_to(0, 0);
  puts_ascii(s, "dedans");
  s.move_to(0, 2);
  s.line_feed();

  CHECK(sb.empty());
}

// Et le symétrique : une région qui touche le BAS sans toucher le haut.
TEST(screen_pushes_nothing_when_the_region_starts_below_the_top) {
  Screen s(8, 4);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  s.set_scroll_region(1, 3);
  s.move_to(0, 1);
  puts_ascii(s, "dedans");
  s.move_to(0, 3);
  s.line_feed();

  CHECK(sb.empty());
}

// La ligne part ENTIÈRE. Une ligne courte ne le prouve pas : ses blancs de
// fin sont rognés de toute façon, et une largeur amputée d'une colonne
// donne exactement le même résultat. Il faut une ligne pleine.
TEST(screen_pushes_the_whole_width_of_a_full_line) {
  Screen s(8, 3);
  Scrollback sb(10);
  s.set_scrollback(&sb);

  puts_ascii(s, "12345678");  // exactement la largeur
  s.move_to(0, 2);
  s.line_feed();

  REQUIRE_EQ(sb.size(), size_t{1});
  CHECK_EQ(text_of(sb.at(0)), std::string("12345678"));
}
