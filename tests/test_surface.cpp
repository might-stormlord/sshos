#include <string>

#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Cell;
using sshos::Rect;
using sshos::Style;
using sshos::Surface;
using sshos::View;

TEST(surface_starts_blank) {
  Surface s(4, 2);
  CHECK_EQ(s.w(), 4);
  CHECK_EQ(s.h(), 2);
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(3, 1).width), 1);
}

TEST(surface_view_translates_coordinates) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  CHECK_EQ(v.w(), 3);
  CHECK_EQ(v.h(), 2);
  v.put(0, 0, U'X', Style{});
  CHECK_EQ(s.at(2, 1).ch, U'X');
  CHECK_EQ(s.at(0, 0).ch, U' ');
}

// La propriété qui rend une application incapable de nuire à ses voisines.
TEST(surface_view_silently_drops_out_of_clip_writes) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  v.put(-1, 0, U'A', Style{});
  v.put(3, 0, U'B', Style{});
  v.put(0, 2, U'C', Style{});
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 10; ++x) CHECK_EQ(s.at(x, y).ch, U' ');
  }
}

TEST(surface_nested_sub_clips_to_parent) {
  Surface s(10, 4);
  View outer = s.root().sub(Rect{2, 0, 4, 1});
  View inner = outer.sub(Rect{2, 0, 10, 1});  // déborde volontairement
  CHECK_EQ(inner.w(), 2);
  inner.put(0, 0, U'Z', Style{});
  CHECK_EQ(s.at(4, 0).ch, U'Z');
}

TEST(surface_text_writes_utf8) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "abc", Style{});
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'a');
  CHECK_EQ(s.at(2, 0).ch, U'c');
}

TEST(surface_text_marks_wide_and_continuation) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "\xe6\x97\xa5x", Style{});  // 日x
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'日');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);
  CHECK_EQ(s.at(2, 0).ch, U'x');
}

// Règle 2 du §4.1 : jamais de glyphe large en dernière colonne. Sans elle,
// le terminal replie la ligne et le modele de frame precedente est perdu.
TEST(surface_never_places_wide_glyph_in_last_column) {
  Surface s(3, 2);
  View v = s.root();
  const int cols = v.text(2, 0, "\xe6\x97\xa5", Style{});  // 日 en derniere colonne
  CHECK_EQ(cols, 0);
  CHECK_EQ(s.at(2, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 1);
}

TEST(surface_fill_respects_clip) {
  Surface s(6, 3);
  View v = s.root().sub(Rect{1, 1, 2, 1});
  Style red;
  red.bg = sshos::Color::indexed(1);
  v.fill(Rect{0, 0, 100, 100}, red);
  CHECK_EQ(s.at(1, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(2, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(3, 1).bg, sshos::Color::def());
  CHECK_EQ(s.at(1, 0).bg, sshos::Color::def());
}

TEST(utf8_decode_handles_truncated_input) {
  char32_t cp = 0;
  const std::string truncated = "\xe6\x97";  // moitie de 日
  const size_t used = sshos::utf8_decode(truncated, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(2));
  CHECK_EQ(cp, U'�');
}

// Écrire par-dessus la tête d'une paire large orpheline la continuation
TEST(surface_overwrite_wide_glyph_head) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Écrire à la position 0 (la tête) orpheline la continuation
  v.put(0, 0, U'A', Style{});
  CHECK_EQ(s.at(0, 0).ch, U'A');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}

// Écrire par-dessus la continuation d'une paire large orpheline la tête
TEST(surface_overwrite_wide_glyph_continuation) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Écrire à la position 1 (la continuation) orpheline la tête
  v.put(1, 0, U'B', Style{});
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U'B');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}

// Remplir une zone qui traverse une paire large orpheline les deux moitiés
TEST(surface_fill_wide_glyph_pair) {
  Surface s(4, 1);
  View v = s.root();
  v.put(0, 0, U'日', Style{});
  CHECK_EQ(s.at(0, 0).width, 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);

  // Remplir depuis 0 à 2 (couvre la tête et la continuation)
  Style empty;
  v.fill(Rect{0, 0, 2, 1}, empty);
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 1);
  CHECK_EQ(s.at(1, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 1);
}
