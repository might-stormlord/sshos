#include <memory>
#include <set>

#include "apps/bloc.hpp"
#include "harness.hpp"
#include "wm/manager.hpp"

using sshos::Bloc;
using sshos::Rect;
using sshos::WinMode;
using sshos::Window;
using sshos::WindowId;
using sshos::WindowManager;

namespace {
const Rect kWork{0, 0, 80, 23};
Window* open_bloc(WindowManager& wm) {
  return wm.open(std::make_unique<Bloc>(), kWork);
}
}  // namespace

TEST(manager_gives_every_window_a_fresh_identifier) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(a->id != b->id);

  // On ferme la DERNIÈRE ouverte, pas la première. C'est le seul endroit où
  // une numérotation dérivée de la taille de la pile -- le raccourci le plus
  // tentant -- se trahit : en fermant la première, l'identifiant recyclé
  // entre en collision avec celui d'une AUTRE fenêtre encore vivante, et les
  // deux assertions sur `gone` passent sans avoir rien vu.
  const WindowId gone = b->id;
  CHECK(wm.close(gone));
  Window* c = open_bloc(wm);
  REQUIRE(c != nullptr);
  // Un identifiant n'est JAMAIS réutilisé : une action différée sur `gone`
  // ne doit pas frapper la nouvelle venue.
  CHECK(c->id != gone);
  CHECK(wm.find(gone) == nullptr);

  // Et l'invariant entier, qu'aucune des deux assertions ci-dessus ne
  // couvre : deux fenêtres vivantes ne partagent jamais un identifiant.
  std::set<WindowId> seen;
  for (const auto& w : wm.stack()) CHECK(seen.insert(w->id).second);
}

TEST(manager_cascades_new_windows_instead_of_stacking_them_exactly) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK_EQ(b->user_rect.x, a->user_rect.x + 2);
  CHECK_EQ(b->user_rect.y, a->user_rect.y + 1);
}

// La cascade doit REPARTIR à zéro avant de sortir de la zone, et c'est
// l'assertion sur les doublons qui le prouve, pas celle sur les bornes :
// clamp_to() garantit déjà à lui seul que display_rect tient dans la zone,
// donc une cascade qui marcherait indéfiniment vers le bas-droite passerait
// une vérification de bornes les doigts dans le nez -- en empilant en
// réalité toutes les fenêtres au-delà de la dix-septième EXACTEMENT au même
// endroit, saturées contre le coin. C'est ce défaut-là qu'on cherche.
TEST(manager_wraps_the_cascade_before_it_walks_off_the_work_area) {
  WindowManager wm;
  Rect previous{-1, -1, 0, 0};
  for (int i = 0; i < 30; ++i) {
    Window* w = open_bloc(wm);
    REQUIRE(w != nullptr);
    CHECK(w->display_rect.x >= kWork.x);
    CHECK(w->display_rect.y >= kWork.y);
    CHECK(w->display_rect.x + w->display_rect.w <= kWork.x + kWork.w);
    CHECK(w->display_rect.y + w->display_rect.h <= kWork.y + kWork.h);
    CHECK(!(w->display_rect == previous));
    previous = w->display_rect;
  }
}

TEST(manager_refuses_to_open_more_than_sixty_four_windows) {
  WindowManager wm;
  for (size_t i = 0; i < WindowManager::kMaxWindows; ++i) {
    REQUIRE(open_bloc(wm) != nullptr);
  }
  CHECK(open_bloc(wm) == nullptr);
  CHECK_EQ(wm.stack().size(), WindowManager::kMaxWindows);

  // Le plafond porte sur les fenêtres VIVANTES : en fermer une doit
  // rouvrir la porte.
  const WindowId first = wm.stack().front()->id;
  CHECK(wm.close(first));
  CHECK(open_bloc(wm) != nullptr);
}

TEST(manager_raises_the_window_it_focuses) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK_EQ(wm.focused(), b->id);
  CHECK_EQ(wm.stack().back()->id, b->id);

  wm.focus(a->id);
  CHECK_EQ(wm.focused(), a->id);
  CHECK_EQ(wm.stack().back()->id, a->id);
}

// Réordonner la pile ne doit JAMAIS déplacer les Window elles-mêmes : leur
// HostImpl tient un pointeur vers elles. Un vector<Window> passerait tous
// les tests ci-dessus et laisserait ces pointeurs dans le vide.
TEST(manager_never_moves_a_window_when_the_stack_is_reordered) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  Window* c = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c != nullptr);

  wm.focus(a->id);
  wm.focus(b->id);
  wm.raise(c->id);
  CHECK(wm.find(a->id) == a);
  CHECK(wm.find(b->id) == b);
  CHECK(wm.find(c->id) == c);

  // Et pas davantage quand le vector doit réallouer.
  for (int i = 0; i < 40; ++i) REQUIRE(open_bloc(wm) != nullptr);
  CHECK(wm.find(a->id) == a);
  CHECK(wm.find(b->id) == b);
  CHECK(wm.find(c->id) == c);
}

// La pile se parcourt de l'avant vers l'arrière : deux fenêtres qui se
// recouvrent, c'est celle du dessus qui reçoit le clic.
TEST(manager_hits_the_topmost_window_first) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  a->display_rect = Rect{0, 0, 30, 10};
  b->display_rect = Rect{5, 2, 30, 10};
  Window* hit = wm.hit(10, 5);
  REQUIRE(hit != nullptr);
  CHECK_EQ(hit->id, b->id);
}

// Une fenêtre réduite n'est plus composée : elle ne doit pas non plus
// intercepter les clics qui la traversent.
TEST(manager_hit_ignores_a_minimized_window) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  a->display_rect = Rect{0, 0, 30, 10};
  b->display_rect = Rect{5, 2, 30, 10};
  wm.set_mode(b->id, WinMode::Minimized, kWork);
  Window* hit = wm.hit(10, 5);
  REQUIRE(hit != nullptr);
  CHECK_EQ(hit->id, a->id);
}

TEST(manager_closing_the_focused_window_moves_focus_to_the_one_below) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(wm.close(b->id));
  CHECK_EQ(wm.focused(), a->id);
}

TEST(manager_focus_next_skips_minimized_windows) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  Window* c = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c != nullptr);
  wm.set_mode(b->id, WinMode::Minimized, kWork);
  wm.focus(a->id);
  wm.focus_next();
  CHECK_EQ(wm.focused(), c->id);
}

// Maximiser puis rétablir doit rendre EXACTEMENT la géométrie d'avant :
// c'est user_rect qui la garde, et set_mode ne doit jamais l'écraser.
TEST(manager_restores_the_exact_geometry_after_maximize) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  REQUIRE(a != nullptr);
  const Rect before = a->user_rect;
  wm.set_mode(a->id, WinMode::Maximized, kWork);
  CHECK(a->display_rect == kWork);
  CHECK(a->user_rect == before);
  wm.set_mode(a->id, WinMode::Normal, kWork);
  CHECK(a->display_rect == before);
}

// Une seule fenêtre plein écran à la fois : focaliser une autre ramène la
// précédente à l'état où elle était avant d'y passer.
TEST(manager_lets_only_one_window_be_fullscreen) {
  WindowManager wm;
  Window* a = open_bloc(wm);
  Window* b = open_bloc(wm);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  wm.set_mode(a->id, WinMode::Maximized, kWork);
  wm.set_mode(a->id, WinMode::Fullscreen, kWork);
  CHECK(a->mode == WinMode::Fullscreen);

  wm.focus(b->id);
  CHECK(a->mode == WinMode::Maximized);  // rendue à son état d'avant
}

TEST(snap_pulls_a_window_onto_a_nearby_edge_but_not_a_distant_one) {
  const Rect work{0, 0, 80, 23};
  CHECK_EQ(sshos::snap(Rect{1, 5, 20, 8}, work, 1).x, 0);
  CHECK_EQ(sshos::snap(Rect{3, 5, 20, 8}, work, 1).x, 3);
  CHECK_EQ(sshos::snap(Rect{59, 5, 20, 8}, work, 1).x, 60);
}

// Les deux bords horizontaux, que le cas ci-dessus laisse de côté.
TEST(snap_pulls_the_top_and_bottom_edges_too) {
  const Rect work{0, 0, 80, 23};
  CHECK_EQ(sshos::snap(Rect{5, 1, 20, 8}, work, 1).y, 0);
  CHECK_EQ(sshos::snap(Rect{5, 4, 20, 8}, work, 1).y, 4);
  CHECK_EQ(sshos::snap(Rect{5, 14, 20, 8}, work, 1).y, 15);
}
