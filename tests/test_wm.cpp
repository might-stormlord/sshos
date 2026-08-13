#include <memory>
#include <string>

#include "app/app.hpp"
#include "fake_apps.hpp"
#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/decor.hpp"
#include "wm/window.hpp"

using sshos::App;
using sshos::Bloc;
using sshos::Border;
using sshos::DecorMetrics;
using sshos::Rect;
using sshos::Size;
using sshos::Style;
using sshos::Surface;
using sshos::Theme;
using sshos::View;
using sshos::Window;
using sshos::WindowId;
using sshos::clamp_to;
using sshos::client_rect;
using sshos::decor_metrics;
using sshos::draw_decor;
using sshos::frame_min;

namespace {

Window make_window(WindowId id, std::string title, Rect r) {
  Window w;
  w.id = id;
  w.title = std::move(title);
  w.user_rect = r;
  w.display_rect = r;
  w.app = std::make_unique<Bloc>();
  return w;
}

}  // namespace

TEST(client_rect_removes_the_decorations) {
  CHECK(client_rect(Rect{10, 4, 20, 8}) == (Rect{11, 5, 18, 6}));
}

TEST(client_rect_never_reports_a_negative_size) {
  const Rect c = client_rect(Rect{0, 0, 1, 1});
  CHECK(c.w >= 0);
  CHECK(c.h >= 0);
}

// min_size() porte sur la zone CLIENTE ; le plancher 16x5 porte sur le
// CADRE. Confondre les deux donnerait des fenêtres de 14 colonnes dont la
// zone utile n'en ferait que 12.
TEST(frame_min_adds_the_decorations_and_honours_the_floor) {
  Bloc small;
  CHECK(frame_min(small) == (Size{16, 5}));

  struct Big : App {
    void render(View) override {}
    Size min_size() const override { return {30, 20}; }
  };
  Big big;
  CHECK(frame_min(big) == (Size{32, 22}));
}

TEST(clamp_to_pushes_a_window_back_inside_the_work_area) {
  const Rect work{0, 0, 80, 23};
  CHECK(clamp_to(Rect{70, 20, 44, 14}, work, Size{16, 5}) == (Rect{36, 9, 44, 14}));
}

// Quand la zone de travail est plus petite que le plancher, le plancher
// gagne : une fenêtre qui déborde reste utilisable, une fenêtre écrasée à
// 4x2 ne l'est pas.
TEST(clamp_to_lets_the_floor_win_over_a_tiny_work_area) {
  const Rect r = clamp_to(Rect{0, 0, 100, 100}, Rect{0, 0, 20, 6}, Size{16, 5});
  CHECK_EQ(r.w, 20);
  CHECK_EQ(r.h, 6);

  const Rect tiny = clamp_to(Rect{0, 0, 100, 100}, Rect{0, 0, 4, 2}, Size{16, 5});
  CHECK_EQ(tiny.w, 16);
  CHECK_EQ(tiny.h, 5);
}

TEST(decor_metrics_places_three_buttons_flush_right) {
  const DecorMetrics m = decor_metrics(Rect{2, 1, 30, 10});
  CHECK_EQ(m.button_count, 3);
  CHECK(m.title_bar == (Rect{2, 1, 30, 1}));
  CHECK_EQ(m.buttons.x + m.buttons.w, 2 + 30 - 1);
  CHECK_EQ(m.buttons.w, 9);
  CHECK(m.client == (Rect{3, 2, 28, 8}));
}

// « Les boutons à toutes les largeurs » de la spec, §12.1 famille 1. Les
// seuils tombent de la même règle : n boutons demandent 7 + 3n colonnes
// (2 de marge, 3 de titre lisible, 1 de marge à droite, 1 de séparation).
TEST(decor_metrics_elides_buttons_in_order_as_the_frame_narrows) {
  int previous = 3;
  for (int w = 40; w >= 4; --w) {
    const DecorMetrics m = decor_metrics(Rect{0, 0, w, 5});
    CHECK(m.button_count <= previous);
    CHECK(m.button_count >= 0);
    CHECK(m.button_count <= 3);
    if (m.button_count > 0) {
      CHECK_EQ(m.buttons.x + m.buttons.w, w - 1);
      CHECK(m.title_text.x + m.title_text.w <= m.buttons.x);
    }
    CHECK(m.title_text.w >= 0);
    previous = m.button_count;
  }
  CHECK_EQ(decor_metrics(Rect{0, 0, 16, 5}).button_count, 3);
  CHECK_EQ(decor_metrics(Rect{0, 0, 15, 5}).button_count, 2);
  CHECK_EQ(decor_metrics(Rect{0, 0, 13, 5}).button_count, 2);
  CHECK_EQ(decor_metrics(Rect{0, 0, 12, 5}).button_count, 1);
  CHECK_EQ(decor_metrics(Rect{0, 0, 10, 5}).button_count, 1);
  CHECK_EQ(decor_metrics(Rect{0, 0, 9, 5}).button_count, 0);
}

TEST(draw_decor_paints_the_title_bar_across_the_full_width) {
  Surface s(30, 10);
  Window w = make_window(1, "Bloc", Rect{2, 1, 20, 7});
  View v = s.root();
  const Theme th = Theme::mono16();
  draw_decor(v, w, true, th, Border::Ascii);

  CHECK(s.text_row(1).find("Bloc") != std::string::npos);
  CHECK(s.at(2, 1).bg == th.title_focus_bg);
  CHECK(s.at(21, 1).bg == th.title_focus_bg);
  CHECK(!(s.at(1, 1).bg == th.title_focus_bg));  // rien en dehors du cadre
}

TEST(draw_decor_distinguishes_a_focused_window_from_a_blurred_one) {
  Surface a(30, 10);
  Surface b(30, 10);
  Window w1 = make_window(1, "Bloc", Rect{0, 0, 20, 7});
  Window w2 = make_window(2, "Bloc", Rect{0, 0, 20, 7});
  const Theme th = Theme::mono16();
  View va = a.root();
  View vb = b.root();
  draw_decor(va, w1, true, th, Border::Ascii);
  draw_decor(vb, w2, false, th, Border::Ascii);

  CHECK(!(a.at(0, 3).fg == b.at(0, 3).fg));  // bordure gauche
  CHECK(!(a.at(0, 0).bg == b.at(0, 0).bg));  // barre de titre
}

TEST(draw_decor_puts_a_resize_handle_in_the_bottom_right_corner) {
  Surface s(30, 10);
  Window w = make_window(1, "Bloc", Rect{2, 1, 20, 7});
  View v = s.root();
  draw_decor(v, w, true, Theme::mono16(), Border::Unicode);
  CHECK_EQ(s.at(21, 7).ch, U'◢');
}

TEST(draw_decor_never_lets_a_long_title_reach_the_buttons) {
  Surface s(30, 10);
  Window w = make_window(1, std::string(50, 'X'), Rect{0, 0, 20, 7});
  View v = s.root();
  draw_decor(v, w, true, Theme::mono16(), Border::Ascii);

  const DecorMetrics m = decor_metrics(w.display_rect);
  REQUIRE(m.button_count == 3);
  for (int x = m.buttons.x; x < m.buttons.x + m.buttons.w; ++x) {
    CHECK(s.at(x, 0).ch != U'X');
  }
  CHECK_EQ(s.at(m.buttons.x, 0).ch, U'[');
}

TEST(draw_decor_falls_back_to_ascii) {
  Surface s(30, 10);
  Window w = make_window(1, "Bloc", Rect{0, 0, 20, 7});
  View v = s.root();
  draw_decor(v, w, true, Theme::mono16(), Border::Ascii);
  CHECK_EQ(s.at(0, 6).ch, U'+');
  CHECK_EQ(s.at(1, 6).ch, U'-');
  CHECK_EQ(s.at(0, 3).ch, U'|');
  CHECK_EQ(s.at(19, 6).ch, U'#');
}

// L'ordre de déclaration des membres de Window n'est pas cosmétique : app
// doit mourir AVANT host, sans quoi une application qui appelle
// host->unwatch() dans son destructeur (Battement, tâche 8) déréférencerait
// un hôte déjà détruit. On le vérifie ici, tant que c'est encore bon
// marché.
TEST(window_destroys_its_app_before_its_host) {
  static int order = 0;
  static int host_died = 0;
  static int app_died = 0;
  order = 0;
  host_died = 0;
  app_died = 0;

  struct NoisyHost : sshos::Host {
    ~NoisyHost() override { host_died = ++order; }
    void set_title(std::string) override {}
    void request_close() override {}
    void invalidate() override {}
    uint64_t watch(int, uint32_t) override { return 0; }
    void unwatch(uint64_t) override {}
    void watch_child(pid_t) override {}
  };
  struct NoisyApp : App {
    ~NoisyApp() override { app_died = ++order; }
    void render(View) override {}
  };

  {
    Window w;
    w.host = std::make_unique<NoisyHost>();
    w.app = std::make_unique<NoisyApp>();
  }
  CHECK_EQ(app_died, 1);
  CHECK_EQ(host_died, 2);
}

// Le plan affirme que les boutons s'élident « dans l'ordre inverse de leur
// utilité ». Les tests ci-dessus ne comptent que leur NOMBRE : sans celui-ci,
// rien n'empêcherait l'élision de sacrifier « fermer » et de laisser une
// fenêtre étroite sans moyen visible d'en sortir.
TEST(draw_decor_elides_minimize_first_and_keeps_close_last) {
  const struct {
    int w;
    const char* expected;
  } cases[] = {{20, "[_][o][x]"}, {15, "[o][x]"}, {12, "[x]"}};

  for (const auto& c : cases) {
    Surface s(40, 6);
    Window w = make_window(1, "", Rect{0, 0, c.w, 5});
    View v = s.root();
    draw_decor(v, w, true, Theme::mono16(), Border::Ascii);

    const DecorMetrics m = decor_metrics(w.display_rect);
    std::string got;
    for (int x = m.buttons.x; x < m.buttons.x + m.buttons.w; ++x)
      got += static_cast<char>(s.at(x, 0).ch);
    CHECK_EQ(got, std::string(c.expected));
  }
}
