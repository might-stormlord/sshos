#include <string>

#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/menu.hpp"

using sshos::Border;
using sshos::Menu;
using sshos::MenuHit;
using sshos::MenuHitResult;
using sshos::MenuItem;
using sshos::Rect;
using sshos::Surface;
using sshos::Theme;
using sshos::View;

namespace {

int index_of(const Menu& m, const std::string& id) {
  const auto& v = m.visible();
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

TEST(menu_starts_closed_and_lists_everything_when_opened) {
  Menu m;
  CHECK(!m.is_open());
  CHECK(m.selected() == nullptr);

  m.open();
  CHECK(m.is_open());
  // Le catalogue, les quatre bords de panneau, les deux commandes de
  // Battement, et de quoi partir.
  CHECK(index_of(m, "app:editeur") >= 0);
  CHECK(index_of(m, "app:fichiers") >= 0);
  CHECK(index_of(m, "panel:top") >= 0);
  CHECK(index_of(m, "panel:bottom") >= 0);
  CHECK(index_of(m, "panel:left") >= 0);
  CHECK(index_of(m, "panel:right") >= 0);
  CHECK(index_of(m, "wm:tile") >= 0);
  // LES DEUX SORTIES, et elles doivent être distinctes : l'une rend la
  // main en gardant tout, l'autre détruit.
  CHECK(index_of(m, "session:detach") >= 0);
  CHECK(index_of(m, "session:quit") >= 0);
  CHECK(index_of(m, "session:quit") >= 0);
  REQUIRE(m.selected() != nullptr);
  CHECK_EQ(m.selection(), 0);

  m.close();
  CHECK(!m.is_open());
  CHECK(m.selected() == nullptr);
}

TEST(menu_filters_on_a_case_insensitive_substring) {
  Menu m;
  m.open();
  const size_t all = m.visible().size();

  m.type(U'P');
  m.type(U'A');
  m.type(U'N');
  // « Panneau : ... » quatre fois, et rien d'autre.
  CHECK_EQ(m.visible().size(), static_cast<size_t>(4));
  CHECK(index_of(m, "panel:top") >= 0);
  CHECK(index_of(m, "app:editeur") < 0);

  m.backspace();
  m.backspace();
  m.backspace();
  CHECK_EQ(m.visible().size(), all);

  // Une sous-chaîne qui n'est le préfixe de rien filtre quand même : c'est
  // une recherche, pas une complétion.
  m.type(U't');
  m.type(U'e');
  CHECK(index_of(m, "app:editeur") >= 0);

  // Et un filtre qui ne rend rien ne rend rien -- sans désigner d'entrée.
  m.backspace();
  m.backspace();
  m.type(U'z');
  m.type(U'z');
  CHECK(m.visible().empty());
  CHECK(m.selected() == nullptr);
}

TEST(menu_keeps_its_selection_inside_the_filtered_list) {
  Menu m;
  m.open();
  m.move(1);
  m.move(1);
  m.move(1);
  m.move(1);
  m.move(1);
  m.move(1);
  m.move(1);
  m.move(1);  // loin dans la liste complète
  const int deep = m.selection();
  CHECK(deep > 3);

  m.type(U'p');
  m.type(U'a');
  m.type(U'n');
  // Filtrer sous les pieds de l'utilisateur ne doit jamais désigner une
  // entrée qui n'existe plus.
  CHECK(m.selection() < static_cast<int>(m.visible().size()));
  REQUIRE(m.selected() != nullptr);
  CHECK(m.selected()->id.compare(0, 6, "panel:") == 0);
}

TEST(menu_selection_wraps_in_both_directions) {
  Menu m;
  m.open();
  const int n = static_cast<int>(m.visible().size());
  REQUIRE(n > 1);

  m.move(-1);
  CHECK_EQ(m.selection(), n - 1);
  m.move(1);
  CHECK_EQ(m.selection(), 0);
}

// Le hit-test du menu est l'inverse de sa disposition : ce qu'on clique est
// ce qu'on voit, la même discipline que le panneau et les décorations.
TEST(menu_hit_test_matches_its_layout) {
  Menu m;
  CHECK(m.hit(1, 20).what == MenuHit::None);  // fermé : rien ne se clique

  m.open();
  m.layout(80, 24);
  const Rect r = m.rect(80, 24);

  CHECK(m.hit(r.x - 1, r.y).what == MenuHit::None);
  CHECK(m.hit(r.x, r.y + r.h).what == MenuHit::None);
  CHECK(m.hit(r.x + 1, r.y + 1).what == MenuHit::Search);

  // Chaque entrée visible a sa ligne, et l'index rendu la désigne.
  int seen = 0;
  for (int y = r.y; y < r.y + r.h; ++y) {
    const MenuHitResult h = m.hit(r.x + 1, y);
    CHECK(h.what != MenuHit::None);
    if (h.what == MenuHit::Item) {
      CHECK(h.index >= 0);
      CHECK(h.index < static_cast<int>(m.visible().size()));
      ++seen;
    }
  }
  CHECK_EQ(seen, static_cast<int>(m.visible().size()));
}

// Ce que le hit-test appelle Item doit porter le libellé de cette entrée.
TEST(menu_hit_test_agrees_with_the_glyphs_actually_painted) {
  Menu m;
  m.open();
  m.layout(80, 24);
  const Rect r = m.rect(80, 24);

  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Ascii);

  for (int y = r.y; y < r.y + r.h; ++y) {
    const MenuHitResult h = m.hit(r.x + 1, y);
    if (h.what != MenuHit::Item) continue;
    const std::string row = s.text_row(y);
    CHECK(row.find(m.visible()[static_cast<size_t>(h.index)].label) !=
          std::string::npos);
  }
}

// Le menu tient dans l'écran quoi qu'il arrive, y compris sur un terminal
// tout juste assez grand pour le bureau.
TEST(menu_never_leaves_the_screen) {
  Menu m;
  m.open();
  for (const auto dim : {std::pair<int, int>{80, 24}, {40, 12}, {40, 6}}) {
    const Rect r = m.rect(dim.first, dim.second);
    CHECK(r.x >= 0);
    CHECK(r.y >= 0);
    CHECK(r.x + r.w <= dim.first);
    CHECK(r.y + r.h <= dim.second);
  }
}

// ---------------------------------------------------------------------------
// L'ancrage au curseur. C'est toute la différence entre un menu contextuel
// et un menu de panneau : sans lui, `open_at` ne serait qu'un `open` plus
// long à écrire.
// ---------------------------------------------------------------------------

TEST(menu_anchors_itself_to_the_cursor) {
  Menu m;
  m.open_at(30, 5);
  const Rect r = m.rect(80, 24);
  CHECK_EQ(r.x, 30);
  CHECK_EQ(r.y, 5);
}

// Un menu contextuel qui déborde par la droite est le défaut le plus banal
// du genre, et le seul que personne ne pardonne : la moitié des entrées
// devient illisible.
TEST(menu_pulls_itself_back_inside_the_screen) {
  const std::pair<int, int> corners[] = {{78, 22}, {79, 23}, {0, 23}, {78, 0}};
  for (const auto& c : corners) {
    Menu m;
    m.open_at(c.first, c.second);
    const Rect r = m.rect(80, 24);
    CHECK(r.x >= 0);
    CHECK(r.y >= 0);
    CHECK(r.x + r.w <= 80);
    CHECK(r.y + r.h <= 24);
  }
}

// Une coordonnée négative n'est pas théorique : le parseur SGR fait
// `param - 1`, donc un client qui envoie `CSI <2;0;0M` livre (-1, -1).
TEST(menu_survives_a_negative_anchor) {
  Menu m;
  m.open_at(-1, -1);
  const Rect r = m.rect(80, 24);
  CHECK(r.x >= 0);
  CHECK(r.y >= 0);
}

// Sans ancre, le menu reste collé à son bouton et pousse vers le haut : il
// ne recouvre pas le panneau d'où il sort.
TEST(menu_without_an_anchor_grows_from_its_panel_button) {
  Menu m;
  m.open();
  const Rect r = m.rect(80, 24);
  CHECK_EQ(r.x, 0);
  CHECK_EQ(r.y + r.h, 23);
}

// open() après open_at() doit OUBLIER l'ancre, sinon le menu du panneau
// resterait collé au dernier clic droit, longtemps après.
TEST(menu_forgets_its_anchor_when_reopened_from_the_button) {
  Menu m;
  m.open_at(30, 5);
  REQUIRE_EQ(m.rect(80, 24).x, 30);
  m.open();
  CHECK_EQ(m.rect(80, 24).x, 0);
}

// Et open_at() construit la MÊME table que open() : une entrée ajoutée au
// menu du panneau doit apparaître au clic droit sans que personne y pense.
TEST(menu_opened_at_the_cursor_carries_the_same_entries) {
  Menu a;
  Menu b;
  a.open();
  b.open_at(10, 10);
  REQUIRE_EQ(a.visible().size(), b.visible().size());
  for (size_t i = 0; i < a.visible().size(); ++i) {
    CHECK_EQ(a.visible()[i].id, b.visible()[i].id);
  }
}

// LE MENU NE SAIT RIEN DU SERVICE DE MISE À JOUR. Il reçoit des entrées
// supplémentaires et les rend comme les siennes : c'est ce qui évite que
// shell/ ait à connaître l'état du démon. La session pose la liste avant
// chaque ouverture ; le menu ne fait que l'afficher et rendre l'identifiant.
TEST(menu_appends_the_extra_items_after_its_own) {
  Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open();

  bool found = false;
  bool after_the_last_fixed_entry = false;
  for (const MenuItem& it : m.visible()) {
    if (it.id == "session:quit") after_the_last_fixed_entry = true;
    if (it.id == "update:check") {
      found = true;
      CHECK_EQ(it.label, std::string("Verifier les mises a jour"));
      CHECK(it.enabled);
      CHECK(after_the_last_fixed_entry);
    }
  }
  CHECK(found);
}

// UNE ENTRÉE INERTE RESTE VISIBLE : elle dit ce qui se passe. C'est la
// session qui refuse d'agir, pas le menu qui cache -- masquer l'entrée
// pendant une mise à jour ferait croire que la fonction a disparu.
TEST(menu_keeps_a_disabled_item_visible) {
  Menu m;
  m.set_extra_items({{"update:apply", "Mise a jour en cours...", false}});
  m.open();

  bool found = false;
  for (const MenuItem& it : m.visible()) {
    if (it.id == "update:apply") {
      found = true;
      CHECK(!it.enabled);
    }
  }
  CHECK(found);
}

// LES ENTRÉES FIXES RESTENT ACTIVES. Le champ a une valeur par défaut, et
// une aggrégation à deux membres doit continuer de produire une entrée
// utilisable -- sans quoi tout le menu deviendrait inerte d'un coup.
TEST(menu_leaves_its_own_entries_enabled) {
  Menu m;
  m.open();
  for (const MenuItem& it : m.visible()) {
    CHECK(it.enabled);
  }
}

// LE CLIC DROIT PASSE PAR LE MÊME CHEMIN. open_at() appelle open(), donc une
// entrée ajoutée apparaît aux trois points d'ouverture sans que personne ait
// à y penser -- l'invariant que menu.cpp décrit déjà pour ses propres
// entrées.
TEST(menu_shows_the_extra_items_when_opened_at_the_cursor) {
  Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open_at(4, 4);

  bool found = false;
  for (const MenuItem& it : m.visible()) {
    if (it.id == "update:check") found = true;
  }
  CHECK(found);
}

// REPOSER LES ENTRÉES LES REMPLACE, ELLE NE LES EMPILE PAS. Le libellé
// change à chaque changement d'état, et le menu est rouvert des dizaines de
// fois : deux ouvertures ne doivent pas donner deux lignes.
TEST(menu_replaces_the_extra_items_instead_of_accumulating_them) {
  Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open();
  m.close();
  m.set_extra_items({{"update:apply", "Mettre a jour", true}});
  m.open();

  int count = 0;
  for (const MenuItem& it : m.visible()) {
    if (it.id.rfind("update:", 0) == 0) {
      ++count;
      CHECK_EQ(it.id, std::string("update:apply"));
    }
  }
  CHECK_EQ(count, 1);
}

// UNE LISTE VIDE N'AJOUTE RIEN. C'est l'etat d'un bureau ou la mise a jour
// n'a pas encore ete cablee.
TEST(menu_without_extra_items_is_unchanged) {
  Menu a;
  Menu b;
  b.set_extra_items({});
  a.open();
  b.open();
  REQUIRE_EQ(a.visible().size(), b.visible().size());
}

// UNE ENTRÉE INERTE SE VOIT. Peinte à l'identique, elle ferait croire qu'un
// clic va faire quelque chose. Elle reste sélectionnable -- la flèche du bas
// ne saute pas par-dessus -- mais sa couleur dit qu'elle attend.
TEST(menu_draws_a_disabled_item_dimmed) {
  Menu m;
  m.set_extra_items({{"update:apply", "Mise a jour en cours...", false}});
  m.open();

  Surface s(60, 24);
  m.layout(60, 24);
  const Theme th = Theme::defaults();
  m.draw(s.root(), th, sshos::Border::Ascii);

  // On cherche la ligne de l'entrée inerte, et on lit la couleur de sa
  // première lettre.
  const Rect r = m.rect(60, 24);
  bool checked = false;
  for (int y = r.y; y < r.y + r.h; ++y) {
    if (s.text_row(y).find("Mise a jour en cours") == std::string::npos) continue;
    const int x = static_cast<int>(s.text_row(y).find("Mise a jour en cours"));
    CHECK(s.at(x, y).fg == th.border_blur);
    CHECK(!(s.at(x, y).fg == th.modal_fg));
    checked = true;
  }
  CHECK(checked);
}

TEST(menu_draws_an_enabled_item_normally) {
  Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open();
  // On deplace la selection pour que l'entree ne soit pas celle qui a la
  // main : la selection a sa propre couleur et masquerait le cas.
  m.move(1);

  Surface s(60, 24);
  m.layout(60, 24);
  const Theme th = Theme::defaults();
  m.draw(s.root(), th, sshos::Border::Ascii);

  const Rect r = m.rect(60, 24);
  bool checked = false;
  for (int y = r.y; y < r.y + r.h; ++y) {
    const std::string row = s.text_row(y);
    const size_t at = row.find("Verifier les mises a jour");
    if (at == std::string::npos) continue;
    CHECK(s.at(static_cast<int>(at), y).fg == th.modal_fg);
    checked = true;
  }
  CHECK(checked);
}
