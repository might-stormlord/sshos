#include <optional>
#include <string>

#include "harness.hpp"
#include "render/diff.hpp"
#include "render/surface.hpp"

using sshos::Color;
using sshos::Differ;
using sshos::OutputProfile;
using sshos::Pos;
using sshos::Style;
using sshos::Surface;

static OutputProfile tc() {
  return OutputProfile::detect("xterm-256color", "truecolor", true);
}

TEST(diff_first_frame_is_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;1H"));
}

// La propriété « bureau au repos = zéro octet ».
TEST(diff_emits_nothing_when_nothing_changed) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);
  CHECK_EQ(d.frame(s, std::nullopt), std::string(""));
}

TEST(diff_touches_only_the_changed_run) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  Style red;
  red.fg = Color::rgb(255, 0, 0);
  s.root().put(1, 0, U'X', red);
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;2H\033[38;2;255;0;0mX\033[1;1H"));
}

TEST(diff_skips_identical_rows) {
  Surface s(3, 2);
  s.root().text(0, 0, "abc", Style{});
  s.root().text(0, 1, "def", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().put(0, 1, U'Z', Style{});
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[2;1HZ\033[1;1H"));
}

// Règle 1 du §4.1 : un segment ne démarre JAMAIS sur une cellule de
// continuation. Ici seule la continuation change, et le diffeur doit
// remonter à la cellule de tête et réémettre la paire entière.
TEST(diff_never_starts_a_run_on_a_continuation_cell) {
  Surface s(3, 1);
  s.root().text(0, 0, "\xe6\x97\xa5" "a", Style{});  // 日a
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.at(1, 0).bg = Color::indexed(4);  // seule la continuation change
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\xe6\x97\xa5\033[1;1H"));
}

// Règle 3 du §4.1 : après un graphème non-ASCII, la position implicite du
// curseur n'est pas fiable — le run se termine et la reprise est absolue.
TEST(diff_reanchors_after_a_non_ascii_glyph) {
  Surface s(4, 1);
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().text(0, 0, "\xe6\x97\xa5" "ab", Style{});  // 日ab
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\xe6\x97\xa5\033[1;3Hab\033[1;1H"));
}

TEST(diff_shows_the_cursor_only_when_asked) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, Pos{2, 0}),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;3H\033[?25h"));
}

TEST(diff_invalidate_forces_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);
  CHECK_EQ(d.frame(s, std::nullopt), std::string(""));
  d.invalidate();
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;1H"));
}

TEST(diff_resize_forces_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  Surface bigger(4, 1);
  bigger.root().text(0, 0, "abcd", Style{});
  CHECK_EQ(d.frame(bigger, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habcd\033[1;1H"));
}

// §4.3 de la spec : « Les fins de ligne à fond par défaut se font en CSI K
// plutôt qu'en espaces. » Cas emblématique : une ligne entièrement vide,
// sur une surface bien plus large que son contenu (ici vide), s'effondre
// en un unique CSI K — et le résultat ne dépend pas de la largeur réelle
// de la surface, c'est tout l'intérêt (200 colonnes coûtent le même octet
// que 5).
TEST(diff_first_frame_collapses_empty_line_to_csi_k) {
  Surface s(200, 1);
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\033[K\033[1;1H"));
}

// Seuil de rentabilité (kMinErasableTail = 4 dans diff.cpp) : à 3 cellules,
// CSI K (3 octets) ne ferait pas mieux que 3 espaces littérales (3 octets)
// — la queue reste donc du texte ordinaire.
TEST(diff_tail_one_cell_short_of_threshold_stays_literal) {
  Surface s(5, 1);
  s.root().text(0, 0, "ab", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Hab   \033[1;1H"));
}

// Un cran plus loin (4 cellules, exactement le seuil), l'effacement gagne
// strictement (3 octets contre 4) : la queue devient un CSI K.
TEST(diff_tail_at_threshold_becomes_csi_k) {
  Surface s(6, 1);
  s.root().text(0, 0, "ab", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Hab\033[K\033[1;1H"));
}

// Le prédicat d'effaçabilité est `cell == Cell{}`, une égalité totale — pas
// « c'est un espace ». Un espace en Reverse reste visible (barre inversée)
// malgré son glyphe vide ; l'effacer serait un bug spectaculaire. Ce test
// échouerait si le prédicat était un jour « simplifié » en un test naïf sur
// `ch` et la couleur seuls.
TEST(diff_does_not_erase_a_tail_with_visible_attributes) {
  Surface s(6, 1);
  s.root().text(0, 0, "ab", Style{});
  Style rev;
  rev.attrs = sshos::attr::Reverse;
  for (int x = 2; x < 6; ++x) s.root().put(x, 0, U' ', rev);
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Hab\033[7m    \033[1;1H"));
}

// Piège du fond hérité : CSI K efface avec le fond SGR courant, pas le
// fond par défaut du terminal. Le pinceau doit revenir au fond par défaut
// (\033[49m) AVANT le CSI K, sinon on peint une barre colorée jusqu'en
// bordure droite. On vérifie les octets exacts, dans cet ordre précis.
TEST(diff_resets_background_before_erasing_the_tail) {
  Surface s(6, 1);
  Style colored;
  colored.bg = Color::rgb(10, 20, 30);
  s.root().text(0, 0, "AB", colored);
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\033[48;2;10;20;30mAB\033[49m\033[K\033[1;1H"));
}

// Propriété « bureau au repos = zéro octet », étendue à une ligne qui
// possède une queue effaçable : si rien n'a changé, y compris la queue,
// la deuxième frame ne doit toujours rien émettre.
TEST(diff_emits_nothing_when_the_erasable_tail_is_unchanged) {
  Surface s(6, 1);
  s.root().text(0, 0, "ab", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);
  CHECK_EQ(d.frame(s, std::nullopt), std::string(""));
}

// Variante plus fine : quand seul le contenu change, la queue déjà propre
// ne doit pas être réeffacée pour rien — le drapeau « frame non vide » ne
// doit pas être mis par une queue qui n'avait pas besoin d'être repeinte.
TEST(diff_does_not_repaint_an_unchanged_tail_when_content_changes) {
  Surface s(6, 1);
  s.root().text(0, 0, "ab", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().put(0, 0, U'Z', Style{});
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1HZ\033[1;1H"));
}

// Symétrique du test précédent : la tête est byte-identique à la frame
// précédente, et c'est la queue qui change seule — ici en s'allongeant
// jusqu'à franchir kMinErasableTail. La tête ne doit pas être repeinte,
// et le seul CUP émis doit sauter directement à tail_start. Ce test
// échouerait si une future simplification de la comptabilité du curseur
// en venait à réémettre "ab" avant d'atteindre la queue.
TEST(diff_erases_tail_alone_when_prefix_is_unchanged) {
  Surface s(6, 1);
  s.root().text(0, 0, "abXY", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().text(2, 0, "  ", Style{});
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;3H\033[K\033[1;1H"));
}
