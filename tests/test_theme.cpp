#include "harness.hpp"
#include "render/profile.hpp"
#include "render/theme.hpp"

using sshos::Color;
using sshos::ColorDepth;
using sshos::ColorKind;
using sshos::OutputProfile;
using sshos::Theme;

TEST(quantize_color_leaves_truecolor_untouched) {
  const Color c = Color::rgb(0x2e, 0x74, 0xb5);
  CHECK(sshos::quantize_color(c, ColorDepth::TrueColor) == c);
}

TEST(quantize_color_maps_rgb_into_the_216_cube) {
  const Color q = sshos::quantize_color(Color::rgb(255, 0, 0), ColorDepth::Indexed256);
  CHECK(q.kind == ColorKind::Indexed);
  CHECK_EQ(static_cast<int>(q.idx), 196);  // 16 + 36*5 + 6*0 + 0
}

TEST(quantize_color_keeps_an_indexed_color_indexed_on_256) {
  const Color q = sshos::quantize_color(Color::indexed(129), ColorDepth::Indexed256);
  CHECK_EQ(static_cast<int>(q.idx), 129);
}

// La propriété qui motive les trois palettes : en 16 couleurs, tout canal
// sous 128 tombe à zéro. Deux gris sombres différents deviennent le même
// noir -- c'est ce que le thème ne doit jamais laisser arriver.
TEST(quantize_color_collapses_every_dark_tone_onto_black) {
  const Color a = sshos::quantize_color(Color::rgb(0x18, 0x20, 0x30), ColorDepth::Mono16);
  const Color b = sshos::quantize_color(Color::rgb(0x2c, 0x32, 0x40), ColorDepth::Mono16);
  CHECK_EQ(static_cast<int>(a.idx), 0);
  CHECK_EQ(static_cast<int>(b.idx), 0);
}

TEST(quantize_color_leaves_the_default_color_alone) {
  const Color q = sshos::quantize_color(Color::def(), ColorDepth::Mono16);
  CHECK(q.kind == ColorKind::Default);
}

TEST(theme_for_profile_selects_one_palette_per_depth) {
  const Theme t = Theme::defaults();
  OutputProfile mono;
  mono.depth = ColorDepth::Mono16;
  OutputProfile idx;
  idx.depth = ColorDepth::Indexed256;
  OutputProfile full;
  full.depth = ColorDepth::TrueColor;
  CHECK(t.for_profile(mono).desktop_bg == Theme::mono16().desktop_bg);
  CHECK(t.for_profile(idx).desktop_bg == Theme::indexed256().desktop_bg);
  CHECK(t.for_profile(full).desktop_bg == Theme::defaults().desktop_bg);
}

// LE test du thème. Chaque paire ci-dessous porte une distinction que
// l'utilisateur doit voir : sans elle, une fenêtre active ne se distingue
// plus d'une inactive, ou le panneau se fond dans le bureau. On vérifie la
// distinction sur la couleur RÉELLEMENT affichée, donc après quantification.
TEST(theme_keeps_every_meaningful_distinction_on_every_depth) {
  const ColorDepth depths[] = {ColorDepth::Mono16, ColorDepth::Indexed256,
                               ColorDepth::TrueColor};
  for (ColorDepth d : depths) {
    OutputProfile p;
    p.depth = d;
    const Theme t = Theme::defaults().for_profile(p);
    const auto q = [d](Color c) { return sshos::quantize_color(c, d); };

    CHECK(q(t.border_focus) != q(t.border_blur));
    CHECK(q(t.title_focus_bg) != q(t.title_blur_bg));
    CHECK(q(t.title_focus_fg) != q(t.title_focus_bg));
    CHECK(q(t.title_blur_fg) != q(t.title_blur_bg));
    CHECK(q(t.panel_bg) != q(t.desktop_bg));
    CHECK(q(t.panel_fg) != q(t.panel_bg));
    CHECK(q(t.modal_bg) != q(t.desktop_bg));
    CHECK(q(t.modal_fg) != q(t.modal_bg));
  }
}
