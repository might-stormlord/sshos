#include <memory>
#include <vector>

#include "apps/bloc.hpp"
#include "harness.hpp"
#include "wm/layout.hpp"
#include "wm/manager.hpp"

using sshos::Bloc;
using sshos::PanelEdge;
using sshos::Rect;
using sshos::WinMode;
using sshos::WindowManager;

TEST(work_area_carves_the_panel_out_of_the_right_edge) {
  CHECK(sshos::work_area(80, 24, PanelEdge::Bottom, 1) == (Rect{0, 0, 80, 23}));
  CHECK(sshos::work_area(80, 24, PanelEdge::Top, 1) == (Rect{0, 1, 80, 23}));
  CHECK(sshos::work_area(80, 24, PanelEdge::Left, 16) == (Rect{16, 0, 64, 24}));
  CHECK(sshos::work_area(80, 24, PanelEdge::Right, 16) == (Rect{0, 0, 64, 24}));
}

// LA propriété de la tâche. Une disposition riche -- plusieurs fenêtres,
// des modes différents -- traverse un rétrécissement violent et doit
// revenir intacte.
TEST(layout_is_reversible_across_a_terminal_shrink_and_regrow) {
  WindowManager wm;
  for (int i = 0; i < 6; ++i) {
    REQUIRE(wm.open(std::make_unique<Bloc>(),
                    sshos::work_area(160, 50, PanelEdge::Bottom, 1)) != nullptr);
  }
  wm.set_mode(wm.stack()[1]->id, WinMode::Maximized,
              sshos::work_area(160, 50, PanelEdge::Bottom, 1));
  wm.set_mode(wm.stack()[3]->id, WinMode::Minimized,
              sshos::work_area(160, 50, PanelEdge::Bottom, 1));

  const Rect big = sshos::work_area(160, 50, PanelEdge::Bottom, 1);
  sshos::relayout(wm, big, 160, 50);

  std::vector<Rect> before_user;
  std::vector<Rect> before_display;
  for (const auto& w : wm.stack()) {
    before_user.push_back(w->user_rect);
    before_display.push_back(w->display_rect);
  }

  const Rect small = sshos::work_area(80, 24, PanelEdge::Bottom, 1);
  sshos::relayout(wm, small, 80, 24);
  for (size_t i = 0; i < wm.stack().size(); ++i) {
    // Les user_rect ne bougent JAMAIS : c'est eux la mémoire de la
    // disposition, et les écraser est le seul moyen de rendre le
    // rétrécissement irréversible.
    CHECK(wm.stack()[i]->user_rect == before_user[i]);
  }

  // Le rétrécissement doit tout de même AVOIR eu un effet visible, sans
  // quoi la réversibilité serait celle d'une fonction qui ne fait rien.
  bool any_moved = false;
  for (size_t i = 0; i < wm.stack().size(); ++i) {
    if (!(wm.stack()[i]->display_rect == before_display[i])) any_moved = true;
  }
  CHECK(any_moved);

  sshos::relayout(wm, big, 160, 50);
  for (size_t i = 0; i < wm.stack().size(); ++i) {
    CHECK(wm.stack()[i]->user_rect == before_user[i]);
    CHECK(wm.stack()[i]->display_rect == before_display[i]);
  }
}

TEST(display_rect_follows_the_mode) {
  WindowManager wm;
  const Rect work = sshos::work_area(80, 24, PanelEdge::Bottom, 1);
  auto* w = wm.open(std::make_unique<Bloc>(), work);
  REQUIRE(w != nullptr);

  wm.set_mode(w->id, WinMode::Maximized, work);
  CHECK(sshos::display_rect_for(*w, work, 80, 24) == work);

  // Le plein écran recouvre TOUT, panneau compris : c'est ce qui le
  // distingue du maximisé.
  wm.set_mode(w->id, WinMode::Fullscreen, work);
  CHECK(sshos::display_rect_for(*w, work, 80, 24) == (Rect{0, 0, 80, 24}));

  // Et une fenêtre normale reste projetée dans la zone, plancher compris.
  wm.set_mode(w->id, WinMode::Normal, work);
  CHECK(sshos::display_rect_for(*w, work, 80, 24) == (Rect{2, 1, 44, 14}));
  CHECK(sshos::display_rect_for(*w, Rect{0, 0, 20, 8}, 20, 9) ==
        (Rect{0, 0, 20, 8}));
}
