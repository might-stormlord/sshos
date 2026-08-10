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
