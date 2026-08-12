#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "apps/bloc.hpp"
#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/clock.hpp"
#include "shell/panel.hpp"
#include "wm/manager.hpp"

using sshos::Bloc;
using sshos::Clock;
using sshos::Panel;
using sshos::PanelEdge;
using sshos::PanelHit;
using sshos::PanelHitResult;
using sshos::Rect;
using sshos::Surface;
using sshos::Theme;
using sshos::View;
using sshos::WinMode;
using sshos::Window;
using sshos::WindowId;
using sshos::WindowManager;

namespace {

// Double local : FakePlatformAt est enfermé dans le namespace anonyme de
// test_session.cpp.
struct ClockPlatform : sshos::Platform {
  explicit ClockPlatform(std::int64_t epoch_seconds) : t_(epoch_seconds) {}
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(t_));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point{};
  }
  std::string read_file(std::string_view) const override { return {}; }

 private:
  std::int64_t t_;
};

}  // namespace

TEST(panel_thickness_depends_on_the_edge) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  CHECK_EQ(p.thickness(), 1);
  p.set_edge(PanelEdge::Left);
  CHECK_EQ(p.thickness(), 16);
}

TEST(panel_occupies_its_edge_and_nothing_else) {
  Panel p;
  p.set_edge(PanelEdge::Top);
  CHECK(p.rect(80, 24) == (Rect{0, 0, 80, 1}));
  p.set_edge(PanelEdge::Right);
  CHECK(p.rect(80, 24) == (Rect{64, 0, 16, 24}));
  p.set_edge(PanelEdge::Bottom);
  CHECK(p.rect(80, 24) == (Rect{0, 23, 80, 1}));
  p.set_edge(PanelEdge::Left);
  CHECK(p.rect(80, 24) == (Rect{0, 0, 16, 24}));
}

// Chaque fenêtre a son entrée, et l'entrée dit son état : ● pour la
// fenêtre active, _ pour une réduite.
TEST(panel_marks_the_active_and_the_minimized_windows) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  const Rect work{0, 0, 80, 23};
  auto* a = wm.open(std::make_unique<Bloc>(), work);
  auto* b = wm.open(std::make_unique<Bloc>(), work);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  a->title = "Bloc";
  b->title = "Bloc";
  wm.set_mode(a->id, WinMode::Minimized, work);
  wm.focus(b->id);
  p.layout(wm, 80, 24, true);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  CHECK(row.find("\xe2\x97\x8f") != std::string::npos);  // ● la fenêtre active

  // Les marques sont cherchées à la cellule que le hit-test désigne, PAS
  // dans la ligne entière : « ☰ ssh_os » contient déjà un souligné, et un
  // row.find('_') resterait vert même sans aucune marque de réduction.
  int ax = -1;
  int bx = -1;
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what != PanelHit::Task) continue;
    if (h.win == a->id && ax < 0) ax = x;
    if (h.win == b->id && bx < 0) bx = x;
  }
  // Une fenêtre réduite garde son entrée : c'est par elle qu'on la rappelle.
  REQUIRE(ax >= 0);
  REQUIRE(bx >= 0);
  CHECK_EQ(s.at(ax, 23).ch, U'_');
  CHECK_EQ(s.at(bx, 23).ch, U'●');
}

// Le débordement : au-delà de ce que le bord peut montrer, les libellés
// s'élident jusqu'à huit cellules, et si ça ne suffit toujours pas le
// reste se replie sur un compteur.
TEST(panel_elides_labels_before_it_folds_the_rest_into_a_counter) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  for (int i = 0; i < 30; ++i) {
    REQUIRE(wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23}) != nullptr);
  }
  p.layout(wm, 80, 24, true);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  CHECK(row.find("\xc2\xbb") != std::string::npos);  // le repli »N

  // Le compteur dit la vérité : le nombre replié plus le nombre montré fait
  // bien le compte.
  int shown = 0;
  int folded = 0;
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Overflow) folded = h.index;
  }
  sshos::WindowId last = 0;
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Task && h.win != last) {
      last = h.win;
      ++shown;
    }
  }
  CHECK_EQ(shown + folded, 30);

  // Et rien ne déborde sur l'horloge, qui garde ses cinq cellules.
  CHECK(s.text_row(23).find("10:05") != std::string::npos);
}

// Le hit-test du panneau est l'inverse de sa disposition, exactement comme
// celui des fenêtres l'est de leurs décorations. Le bouton de menu occupe
// des cellules précises : cliquer une colonne à côté ne doit pas l'ouvrir.
TEST(panel_hit_test_matches_its_layout) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  REQUIRE(wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23}) != nullptr);
  p.layout(wm, 80, 24, true);

  // Hors du panneau : rien.
  CHECK(p.hit(10, 0).what == PanelHit::None);
  CHECK(p.hit(10, 22).what == PanelHit::None);

  // Sur le panneau : jamais None, chaque cellule appartient à quelque chose.
  int menu_cells = 0;
  int task_cells = 0;
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    CHECK(h.what != PanelHit::None);
    if (h.what == PanelHit::MenuButton) ++menu_cells;
    if (h.what == PanelHit::Task) ++task_cells;
  }
  // « ☰ ssh_os » : huit cellules d'affichage, le glyphe en valant une (la
  // politique de largeur ambiguë est à faux, cf. width.cpp:46).
  CHECK_EQ(menu_cells, 8);
  CHECK(task_cells > 0);

  // La fenêtre ouverte ici l'a été sans passer par le catalogue : son app_id
  // est vide, donc aucune entrée épinglée ne la revendique et les deux du
  // catalogue restent vierges. « Battement » fait neuf cellules : la seconde
  // doit être coupée à huit, marque comprise, plus la cellule de marque
  // d'état -- neuf en tout. Sans cette assertion, un panneau qui n'élide
  // rien reste vert.
  int pinned_cells[2] = {0, 0};
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Pinned && h.index >= 0 && h.index < 2) {
      ++pinned_cells[h.index];
    }
  }
  CHECK_EQ(pinned_cells[0], 5);  // marque + « Bloc »
  CHECK_EQ(pinned_cells[1], 9);  // marque + « Batteme… »
}

// Le point de la fusion : une application épinglée QU'ON LANCE ne se
// dédouble pas. Avant, la barre montrait « [Bloc] » d'un côté et « ●Bloc »
// de l'autre -- deux cibles pour une seule application, à trente cellules
// d'écart.
TEST(panel_merges_a_pinned_application_with_the_window_it_opened) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  Window* w = wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23});
  REQUIRE(w != nullptr);
  w->app_id = "bloc";  // ce que pose open_from_catalog
  p.layout(wm, 80, 24, true);

  // Plus aucune cellule ne répond « épinglée numéro 0 » : l'entrée de Bloc
  // est devenue la tâche. Battement, lui, n'est pas lancé et reste épinglé.
  int bloc_pinned = 0;
  int battement_pinned = 0;
  int tasks = 0;
  WindowId hit_win = 0;
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Pinned && h.index == 0) ++bloc_pinned;
    if (h.what == PanelHit::Pinned && h.index == 1) ++battement_pinned;
    if (h.what == PanelHit::Task) {
      ++tasks;
      hit_win = h.win;
    }
  }
  CHECK_EQ(bloc_pinned, 0);
  CHECK(battement_pinned > 0);
  CHECK_EQ(tasks, 5);  // « ●Bloc »
  CHECK_EQ(hit_win, w->id);
}

// Deux fenêtres de la même application partagent UNE entrée, qui dit
// combien, et dont les clics successifs font le tour du groupe.
TEST(panel_folds_several_windows_of_one_application_into_one_entry) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  Window* a = wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23});
  Window* b = wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23});
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  a->app_id = "bloc";
  b->app_id = "bloc";
  wm.focus(b->id);
  p.layout(wm, 80, 24, false);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  CHECK(row.find("*Bloc(2)") != std::string::npos);
  // Une seule entrée, donc un seul « Bloc » dans toute la barre.
  CHECK_EQ(row.find("Bloc"), row.rfind("Bloc"));

  // Le clic vise la SUIVANTE du groupe, pas celle qui a déjà la main :
  // sans quoi cliquer une entrée à deux fenêtres réduirait la fenêtre au
  // lieu de montrer l'autre, et la seconde deviendrait inatteignable.
  WindowId target = 0;
  for (int x = 0; x < 80 && target == 0; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Task) target = h.win;
  }
  CHECK_EQ(target, a->id);
}

// Le bord vertical distingue lui aussi une entrée lancée d'une entrée qui ne
// l'est pas. Rien ne le vérifiait : une mutation qui rendait TOUT « épinglé »
// sur ce bord passait la suite entière, et cliquer la fenêtre ouverte aurait
// relancé l'application au lieu de la rappeler.
TEST(panel_tells_a_running_entry_from_an_idle_one_on_a_vertical_edge) {
  Panel p;
  p.set_edge(PanelEdge::Left);
  WindowManager wm;
  Window* w = wm.open(std::make_unique<Bloc>(), Rect{0, 0, 64, 24});
  REQUIRE(w != nullptr);
  w->app_id = "bloc";
  p.layout(wm, 80, 24, true);

  // Ligne 0 : le bouton de menu. Puis une ligne par entrée, catalogue en
  // tête -- Bloc lancé, Battement non.
  const PanelHitResult live = p.hit(2, 1);
  CHECK(live.what == PanelHit::Task);
  CHECK_EQ(live.win, w->id);

  const PanelHitResult idle = p.hit(2, 2);
  CHECK(idle.what == PanelHit::Pinned);
  CHECK_EQ(idle.index, 1);
  CHECK_EQ(idle.win, 0u);
}

// Une fenêtre qu'aucune entrée du catalogue ne revendique garde la sienne :
// sans cette boucle elle disparaîtrait de la barre.
TEST(panel_keeps_an_entry_for_a_window_no_catalog_entry_claims) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  Window* w = wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23});
  REQUIRE(w != nullptr);
  w->app_id = "venu-d-ailleurs";
  w->title = "Ailleurs";
  p.layout(wm, 80, 24, false);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  CHECK(s.text_row(23).find("*Ailleurs") != std::string::npos);
}

// Le hit-test dit ce que le dessin montre. Sur toute la longueur du
// panneau, une cellule marquée MenuButton doit porter une lettre du bouton
// de menu, et une cellule marquée Body doit être vide.
TEST(panel_hit_test_agrees_with_the_glyphs_actually_painted) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  REQUIRE(wm.open(std::make_unique<Bloc>(), Rect{0, 0, 80, 23}) != nullptr);
  p.layout(wm, 80, 24, false);  // ASCII : un octet par cellule

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  REQUIRE_EQ(static_cast<int>(row.size()), 80);

  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    const char ch = row[static_cast<size_t>(x)];
    if (h.what == PanelHit::Body) CHECK_EQ(ch, ' ');
    if (h.what == PanelHit::MenuButton) CHECK(ch != ' ');
  }
}

// Sur un bord vertical, les entrées s'empilent et l'horloge tient les deux
// dernières lignes : la date au-dessus de l'heure.
TEST(panel_stacks_its_entries_on_a_vertical_edge) {
  Panel p;
  p.set_edge(PanelEdge::Left);
  WindowManager wm;
  REQUIRE(wm.open(std::make_unique<Bloc>(), Rect{16, 0, 64, 24}) != nullptr);
  p.layout(wm, 80, 24, true);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05", "lun 10 aou");

  CHECK(s.text_row(0).find("ssh_os") != std::string::npos);
  CHECK(s.text_row(22).find("lun 10 aou") != std::string::npos);
  CHECK(s.text_row(23).find("10:05") != std::string::npos);
  CHECK(p.hit(3, 23).what == PanelHit::Clock);
  CHECK(p.hit(3, 22).what == PanelHit::Clock);
  CHECK(p.hit(3, 0).what == PanelHit::MenuButton);
  // Rien du panneau ne déborde à droite de sa seizième colonne.
  CHECK(p.hit(16, 0).what == PanelHit::None);
}

// L'horloge ne salit la frame que lorsque son TEXTE change. Sans cette
// garde, le bureau se repeindrait trente fois par seconde pour rien, et la
// contre-pression du jalon 1 finirait par s'en apercevoir.
TEST(clock_reports_a_change_only_when_the_rendered_text_changes) {
  ClockPlatform at(1786370700);  // 2026-08-10 14:05:00 UTC
  Clock c;
  CHECK(c.update(at));           // première fois : toujours un changement
  CHECK(!c.update(at));          // même instant, même texte
  ClockPlatform later(1786370700 + 30);
  CHECK(!c.update(later));       // même minute
  ClockPlatform next_minute(1786370700 + 60);
  CHECK(c.update(next_minute));
  CHECK(!c.text().empty());
  CHECK(!c.date().empty());
}
