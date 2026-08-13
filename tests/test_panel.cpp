#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "apps/bloc.hpp"
#include "app/catalog.hpp"
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

// L'index d'une application dans le catalogue. Les tests du panneau
// parlent d'entrées épinglées, qui sont les entrées du catalogue : les
// désigner par leur identifiant les rend indifférents à son ordre.
int catalog_index(std::string_view id) {
  const auto& entries = sshos::catalog();
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

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
  // LARGE À DESSEIN : ce cas parle des MARQUES, pas de la capacité du
  // panneau. À 80 colonnes, le catalogue à cinq entrées ne laisse plus la
  // place aux deux fenêtres, et le panneau les replie -- ce qui est son
  // travail, mais qui ferait échouer un test qui ne parle pas de ça.
  p.layout(wm, 140, 24, true);

  Surface s(140, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  CHECK(row.find("\xe2\x97\x8f") != std::string::npos);  // ● la fenêtre active

  // Les marques sont cherchées à la cellule que le hit-test désigne, PAS
  // dans la ligne entière : « ☰ ssh_os » contient déjà un souligné, et un
  // row.find('_') resterait vert même sans aucune marque de réduction.
  int ax = -1;
  int bx = -1;
  for (int x = 0; x < 140; ++x) {
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
  // Les index sont ceux du CATALOGUE : les chercher par identifiant plutôt
  // que les écrire en dur, sinon toute application ajoutée au catalogue
  // casse un test qui ne parle pas d'elle.
  std::vector<int> pinned_cells(sshos::catalog().size(), 0);
  for (int x = 0; x < 80; ++x) {
    const PanelHitResult h = p.hit(x, 23);
    if (h.what == PanelHit::Pinned && h.index >= 0 &&
        static_cast<size_t>(h.index) < pinned_cells.size()) {
      ++pinned_cells[static_cast<size_t>(h.index)];
    }
  }
  CHECK_EQ(pinned_cells[catalog_index("bloc")], 5);       // marque + « Bloc »
  CHECK_EQ(pinned_cells[catalog_index("battement")], 9);  // + « Batteme… »
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
    if (h.what == PanelHit::Pinned && h.index == catalog_index("bloc")) {
      ++bloc_pinned;
    }
    if (h.what == PanelHit::Pinned && h.index == catalog_index("battement")) {
      ++battement_pinned;
    }
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
  // tête -- Bloc lancé, Battement non. On CHERCHE les deux lignes au lieu
  // de les écrire en dur : leur numéro dépend du catalogue, pas de ce que
  // ce cas vérifie.
  bool found_live = false;
  bool found_idle = false;
  PanelHitResult live{};
  PanelHitResult idle{};
  for (int y = 1; y < 24; ++y) {
    const PanelHitResult h = p.hit(2, y);
    if (!found_live && h.what == PanelHit::Task) {
      live = h;
      found_live = true;
    }
    if (!found_idle && h.what == PanelHit::Pinned &&
        h.index == catalog_index("battement")) {
      idle = h;
      found_idle = true;
    }
  }

  REQUIRE(found_live);
  CHECK(live.what == PanelHit::Task);
  CHECK_EQ(live.win, w->id);

  REQUIRE(found_idle);
  CHECK(idle.what == PanelHit::Pinned);
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

// ---------------------------------------------------------------------------
// Le rappel de la touche leader. C'est la moitié « permanente » de la parade
// du §16 : l'aide dit quoi faire, le rappel dit qu'elle existe. Il ne doit
// jamais coûter une entrée de tâche -- une barre pleine appartient à
// quelqu'un qui n'a plus besoin qu'on lui rappelle la touche, et un bureau
// vide, l'état du débutant, a toute la place du monde.
// ---------------------------------------------------------------------------

TEST(panel_shows_the_leader_reminder_when_there_is_room) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  p.set_hint("^A = aide");
  WindowManager wm;
  // « quand il y a de la place » : à 80 colonnes le catalogue à cinq
  // entrées n'en laisse plus, et le panneau retire l'aide EN PREMIER --
  // c'est exactement ce que vérifie le cas suivant.
  p.layout(wm, 140, 24, true);

  Surface s(140, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  CHECK(s.text_row(23).find("^A = aide") != std::string::npos);
}

TEST(panel_drops_the_reminder_before_dropping_a_task) {
  WindowManager wm;
  const Rect work{0, 0, 80, 23};

  // Assez de fenêtres hors catalogue pour saturer la barre : chaque titre
  // distinct produit sa propre entrée.
  for (int i = 0; i < 6; ++i) {
    auto* w = wm.open(std::make_unique<Bloc>(), work);
    REQUIRE(w != nullptr);
    w->title = "Fenetre" + std::to_string(i);
  }

  // La preuve tient en une comparaison : sur une barre pleine, le panneau
  // qui porte un rappel doit rendre EXACTEMENT le même dessin que celui qui
  // n'en porte pas. Le rappel a donc cédé la place entièrement, sans coûter
  // ni une entrée ni une cellule de repli.
  Panel with_hint;
  with_hint.set_edge(PanelEdge::Bottom);
  with_hint.set_hint("^A = aide");
  with_hint.layout(wm, 80, 24, true);
  Surface a(80, 24);
  View va = a.root();
  with_hint.draw(va, Theme::mono16(), "10:05");

  Panel bare;
  bare.set_edge(PanelEdge::Bottom);
  bare.layout(wm, 80, 24, true);
  Surface b(80, 24);
  View vb = b.root();
  bare.draw(vb, Theme::mono16(), "10:05");

  CHECK(a.text_row(23) == b.text_row(23));
  CHECK(a.text_row(23).find("^A = aide") == std::string::npos);
}

// Le rappel est cliquable : c'est ce qui le rend utile à qui essaie la
// souris avant le clavier. Un rappel qui ne répondrait pas au clic
// enseignerait surtout que le bureau ne réagit pas.
TEST(panel_reports_a_click_on_the_leader_reminder) {
  Panel p;
  p.set_edge(PanelEdge::Bottom);
  p.set_hint("^A = aide");
  WindowManager wm;
  // Profil ASCII exprès : text_row() rend des OCTETS, et le ☰ du bouton de
  // menu en prend trois à lui seul. Sous UTF-8, l'indice rendu par find()
  // ne serait pas la colonne, et ce test viserait deux cellules à côté.
  p.layout(wm, 80, 24, false);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  const size_t at = row.find("^A = aide");
  REQUIRE(at != std::string::npos);

  CHECK(p.hit(static_cast<int>(at), 23).what == PanelHit::Hint);
  CHECK(p.hit(static_cast<int>(at) + 8, 23).what == PanelHit::Hint);
  // Et le hit-test ne déborde ni d'un côté ni de l'autre.
  CHECK(p.hit(static_cast<int>(at) - 1, 23).what != PanelHit::Hint);
  CHECK(p.hit(static_cast<int>(at) + 9, 23).what != PanelHit::Hint);
}

// Sur un bord vertical le rappel prend la ligne juste au-dessus de
// l'horloge, avec la même règle de priorité.
TEST(panel_puts_the_reminder_above_the_clock_on_a_vertical_edge) {
  Panel p;
  p.set_edge(PanelEdge::Left);
  p.set_hint("^A = aide");
  WindowManager wm;
  p.layout(wm, 16, 24, true);

  Surface s(80, 24);
  View v = s.root();
  p.draw(v, Theme::mono16(), "10:05", "Mon 10 Aug");
  // Horloge sur les deux dernières lignes, rappel juste au-dessus.
  CHECK(s.text_row(21).find("^A = aide") != std::string::npos);
  CHECK(p.hit(3, 21).what == PanelHit::Hint);
}

// Sans rappel posé, rien ne change : Panel::set_hint() n'est pas obligatoire
// et un panneau qui n'en a pas ne doit pas réserver de place fantôme.
TEST(panel_reserves_nothing_when_no_reminder_is_set) {
  Panel bare;
  bare.set_edge(PanelEdge::Bottom);
  WindowManager wm;
  bare.layout(wm, 80, 24, true);

  Surface s(80, 24);
  View v = s.root();
  bare.draw(v, Theme::mono16(), "10:05");
  const std::string row = s.text_row(23);
  CHECK(row.find("aide") == std::string::npos);
  CHECK(row.find("^") == std::string::npos);
}

// Le pendant vertical du test précédent : sur un bord vertical la place se
// compte en LIGNES, et le rappel doit céder la sienne à une tâche plutôt
// que de la lui prendre.
TEST(panel_drops_the_vertical_reminder_before_dropping_a_task) {
  WindowManager wm;
  const Rect work{0, 0, 64, 24};
  for (int i = 0; i < 24; ++i) {
    auto* w = wm.open(std::make_unique<Bloc>(), work);
    REQUIRE(w != nullptr);
    w->title = "F" + std::to_string(i);
  }

  Panel with_hint;
  with_hint.set_edge(PanelEdge::Left);
  with_hint.set_hint("^A = aide");
  with_hint.layout(wm, 80, 24, true);
  Surface a(80, 24);
  View va = a.root();
  with_hint.draw(va, Theme::mono16(), "10:05", "Mon 10 Aug");

  Panel bare;
  bare.set_edge(PanelEdge::Left);
  bare.layout(wm, 80, 24, true);
  Surface b(80, 24);
  View vb = b.root();
  bare.draw(vb, Theme::mono16(), "10:05", "Mon 10 Aug");

  for (int y = 0; y < 24; ++y) {
    if (a.text_row(y) != b.text_row(y)) {
      th::fail(__FILE__, __LINE__,
               "le rappel a coute une ligne de tache, ligne " +
                   std::to_string(y) + " : |" + a.text_row(y) + "| vs |" +
                   b.text_row(y) + "|");
    }
  }
}
