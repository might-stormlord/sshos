#include <string>

#include "apps/editor/buffer.hpp"
#include "harness.hpp"

using sshos::TextBuffer;
using sshos::TextPos;

// ------------------------------------------------------------ le chargement

// UN FICHIER VIDE VAUT UNE LIGNE VIDE, pas zéro : sans cet invariant, le
// curseur n'a nulle part où être et chaque accès demande sa garde.
TEST(buffer_starts_with_one_empty_line) {
  const TextBuffer b;
  CHECK_EQ(b.line_count(), size_t{1});
  CHECK_EQ(b.line(0), std::string(""));
  CHECK(!b.modified());
}

TEST(buffer_loads_lines) {
  TextBuffer b;
  b.load("un\ndeux\ntrois\n");
  REQUIRE_EQ(b.line_count(), size_t{3});
  CHECK_EQ(b.line(0), std::string("un"));
  CHECK_EQ(b.line(2), std::string("trois"));
}

// Le saut de ligne FINAL ne crée pas de ligne vide de plus.
TEST(buffer_does_not_add_a_line_for_the_trailing_newline) {
  TextBuffer b;
  b.load("un\n");
  CHECK_EQ(b.line_count(), size_t{1});
}

// Et son ABSENCE est retenue : en rajouter un qui n'y était pas fait
// grossir le fichier d'un octet à chaque enregistrement, et rend un diff
// bruyant.
TEST(buffer_gives_back_exactly_what_it_was_given) {
  TextBuffer a;
  a.load("un\ndeux\n");
  CHECK_EQ(a.text(), std::string("un\ndeux\n"));

  TextBuffer b;
  b.load("un\ndeux");
  CHECK_EQ(b.text(), std::string("un\ndeux"));
}

TEST(buffer_loads_an_empty_text_as_one_empty_line) {
  TextBuffer b;
  b.load("");
  CHECK_EQ(b.line_count(), size_t{1});
  CHECK_EQ(b.line(0), std::string(""));
}

TEST(buffer_reads_nothing_out_of_its_bounds) {
  TextBuffer b;
  b.load("un\n");
  CHECK_EQ(b.line(99), std::string(""));
}

// ------------------------------------------------------------ les éditions

TEST(buffer_inserts_a_character) {
  TextBuffer b;
  b.load("ac");
  const TextPos p = b.insert(TextPos{0, 1}, "b");
  CHECK_EQ(b.line(0), std::string("abc"));
  CHECK_EQ(p.col, size_t{2});
  CHECK(b.modified());
}

TEST(buffer_splits_a_line) {
  TextBuffer b;
  b.load("abcd");
  const TextPos p = b.split_line(TextPos{0, 2});
  REQUIRE_EQ(b.line_count(), size_t{2});
  CHECK_EQ(b.line(0), std::string("ab"));
  CHECK_EQ(b.line(1), std::string("cd"));
  CHECK_EQ(p.line, size_t{1});
  CHECK_EQ(p.col, size_t{0});
}

TEST(buffer_erases_the_character_before) {
  TextBuffer b;
  b.load("abc");
  const TextPos p = b.erase_before(TextPos{0, 2});
  CHECK_EQ(b.line(0), std::string("ac"));
  CHECK_EQ(p.col, size_t{1});
}

// En DÉBUT de ligne, le retour arrière fusionne avec la précédente. C'est
// ce que fait tout éditeur, et l'oublier laisse une touche sans effet une
// fois sur vingt.
TEST(buffer_merges_with_the_line_above_on_backspace_at_column_zero) {
  TextBuffer b;
  b.load("ab\ncd");
  const TextPos p = b.erase_before(TextPos{1, 0});
  REQUIRE_EQ(b.line_count(), size_t{1});
  CHECK_EQ(b.line(0), std::string("abcd"));
  CHECK_EQ(p.line, size_t{0});
  CHECK_EQ(p.col, size_t{2});  // là où la jointure s'est faite
}

TEST(buffer_does_nothing_on_backspace_at_the_very_start) {
  TextBuffer b;
  b.load("ab");
  const TextPos p = b.erase_before(TextPos{0, 0});
  CHECK_EQ(b.line(0), std::string("ab"));
  CHECK(p == (TextPos{0, 0}));
  CHECK(!b.modified());
}

TEST(buffer_erases_the_character_under_the_cursor) {
  TextBuffer b;
  b.load("abc");
  b.erase_at(TextPos{0, 1});
  CHECK_EQ(b.line(0), std::string("ac"));
}

// En FIN de ligne, la suppression fusionne avec la suivante.
TEST(buffer_merges_with_the_line_below_on_delete_at_the_end) {
  TextBuffer b;
  b.load("ab\ncd");
  b.erase_at(TextPos{0, 2});
  REQUIRE_EQ(b.line_count(), size_t{1});
  CHECK_EQ(b.line(0), std::string("abcd"));
}

TEST(buffer_does_nothing_on_delete_at_the_very_end) {
  TextBuffer b;
  b.load("ab");
  b.erase_at(TextPos{0, 2});
  CHECK_EQ(b.line(0), std::string("ab"));
  CHECK(!b.modified());
}

// ----------------------------------------------------------- l'état modifié

TEST(buffer_forgets_it_was_modified_once_saved) {
  TextBuffer b;
  b.load("a");
  b.insert(TextPos{0, 1}, "b");
  REQUIRE(b.modified());

  b.mark_saved();
  CHECK(!b.modified());
}

// Charger REMET le drapeau à zéro : un tampon fraîchement ouvert n'est pas
// modifié, et prétendre le contraire poserait la question à chaque
// fermeture.
TEST(buffer_is_not_modified_right_after_a_load) {
  TextBuffer b;
  b.insert(TextPos{0, 0}, "x");
  REQUIRE(b.modified());

  b.load("neuf");
  CHECK(!b.modified());
}

// ------------------------------------------------------------ la recherche

TEST(buffer_finds_what_it_is_asked_for) {
  TextBuffer b;
  b.load("un\ndeux\ntrois");
  TextPos found;
  REQUIRE(b.find("deux", TextPos{0, 0}, found));
  CHECK_EQ(found.line, size_t{1});
  CHECK_EQ(found.col, size_t{0});
}

// La recherche BOUCLE par le début : sans cela, la deuxième occurrence
// d'un mot devient introuvable dès qu'on l'a dépassée.
TEST(buffer_wraps_around_when_it_searches) {
  TextBuffer b;
  b.load("cible\nautre\n");
  TextPos found;
  REQUIRE(b.find("cible", TextPos{1, 0}, found));
  CHECK_EQ(found.line, size_t{0});
}

TEST(buffer_finds_the_next_occurrence_not_the_same_one) {
  TextBuffer b;
  b.load("aa\naa");
  TextPos found;
  REQUIRE(b.find("aa", TextPos{0, 0}, found));
  // On repart d'APRÈS le début de la trouvaille précédente.
  REQUIRE(b.find("aa", TextPos{0, 1}, found));
  CHECK_EQ(found.line, size_t{1});
}

TEST(buffer_finds_nothing_that_is_not_there) {
  TextBuffer b;
  b.load("un\ndeux");
  TextPos found;
  CHECK(!b.find("zzz", TextPos{0, 0}, found));
}

// ---------------------------------------------------------------- le bornage

TEST(buffer_clamps_a_position_that_left_the_buffer) {
  TextBuffer b;
  b.load("ab\ncd");
  const TextPos p = b.clamp(TextPos{99, 99});
  CHECK_EQ(p.line, size_t{1});
  CHECK_EQ(p.col, size_t{2});
}

// Une recherche VIDE ne trouve rien. Sans cette garde, `find("")` rend la
// position de départ à chaque appel : l'éditeur croirait avoir trouvé
// quelque chose à l'infini.
TEST(buffer_finds_nothing_for_an_empty_needle) {
  TextBuffer b;
  b.load("un\ndeux");
  TextPos found;
  CHECK(!b.find("", TextPos{0, 0}, found));
}
