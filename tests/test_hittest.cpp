#include <memory>
#include <string>

#include "fake_apps.hpp"
#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/decor.hpp"
#include "wm/hittest.hpp"
#include "wm/window.hpp"

using sshos::Bloc;
using sshos::Border;
using sshos::DecorMetrics;
using sshos::Rect;
using sshos::Surface;
using sshos::Theme;
using sshos::View;
using sshos::WinHit;
using sshos::Window;
using sshos::decor_metrics;
using sshos::draw_decor;
using sshos::hit_window;

namespace {

Window make_window(Rect r, std::string title) {
  Window w;
  w.id = 7;
  w.title = std::move(title);
  w.user_rect = r;
  w.display_rect = r;
  w.app = std::make_unique<Bloc>();
  return w;
}

}  // namespace

TEST(hit_window_ignores_a_point_outside_the_frame) {
  Window w = make_window(Rect{10, 5, 20, 8}, "Bloc");
  CHECK(hit_window(w, 9, 5).what == WinHit::None);
  CHECK(hit_window(w, 30, 5).what == WinHit::None);
  CHECK(hit_window(w, 10, 4).what == WinHit::None);
  CHECK(hit_window(w, 10, 13).what == WinHit::None);
}

TEST(hit_window_names_every_zone) {
  Window w = make_window(Rect{10, 5, 20, 8}, "Bloc");
  const DecorMetrics m = decor_metrics(w.display_rect);
  REQUIRE(m.button_count == 3);

  CHECK(hit_window(w, 11, 5).what == WinHit::TitleBar);
  CHECK(hit_window(w, m.buttons.x, 5).what == WinHit::ButtonMinimize);
  CHECK(hit_window(w, m.buttons.x + 3, 5).what == WinHit::ButtonMaximize);
  CHECK(hit_window(w, m.buttons.x + 6, 5).what == WinHit::ButtonClose);
  CHECK(hit_window(w, 29, 8).what == WinHit::EdgeRight);
  CHECK(hit_window(w, 15, 12).what == WinHit::EdgeBottom);
  CHECK(hit_window(w, 29, 12).what == WinHit::CornerBR);
  CHECK(hit_window(w, 10, 8).what == WinHit::Frame);  // bordure gauche

  const sshos::WinHitResult c = hit_window(w, 13, 7);
  CHECK(c.what == WinHit::Client);
  CHECK_EQ(c.win, static_cast<sshos::WindowId>(7));
  CHECK_EQ(c.lx, 2);
  CHECK_EQ(c.ly, 1);
}

// LE test de propriété du jalon. On confronte le hit-test aux glyphes
// réellement posés, pas à une seconde dérivation de la même formule : un
// décalage d'une colonne entre decor.cpp et hittest.cpp échoue ici.
// Bordures Unicode volontairement, pour que les glyphes de boutons
// (□ ×) ne puissent pas se confondre avec une lettre du titre.
TEST(hit_test_agrees_with_the_glyphs_actually_painted) {
  for (int w_px = 16; w_px <= 40; ++w_px) {
    Surface s(60, 20);
    Window w = make_window(Rect{3, 2, w_px, 9}, "Bloc");
    View v = s.root();
    draw_decor(v, w, true, Theme::mono16(), Border::Unicode);

    const int y = w.display_rect.y;
    for (int x = w.display_rect.x; x < w.display_rect.x + w_px; ++x) {
      const WinHit what = hit_window(w, x, y).what;
      const bool in_button_zone =
          (what == WinHit::ButtonMinimize || what == WinHit::ButtonMaximize ||
           what == WinHit::ButtonClose);
      const char32_t ch = s.at(x, y).ch;
      const bool looks_like_button =
          (ch == U'[' || ch == U']' || ch == U'_' || ch == U'□' || ch == U'×');
      CHECK_EQ(in_button_zone, looks_like_button);
    }
  }
}

// Toute cellule du cadre appartient à une zone : aucune ne doit tomber
// dans None, sans quoi un clic dessus traverserait la fenêtre jusqu'au
// bureau.
TEST(hit_window_leaves_no_hole_inside_the_frame) {
  Window w = make_window(Rect{2, 1, 24, 9}, "Bloc");
  for (int y = 1; y < 10; ++y) {
    for (int x = 2; x < 26; ++x) {
      CHECK(hit_window(w, x, y).what != WinHit::None);
    }
  }
}
