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
  CHECK(index_of(m, "app:bloc") >= 0);
  CHECK(index_of(m, "app:battement") >= 0);
  CHECK(index_of(m, "panel:top") >= 0);
  CHECK(index_of(m, "panel:bottom") >= 0);
  CHECK(index_of(m, "panel:left") >= 0);
  CHECK(index_of(m, "panel:right") >= 0);
  CHECK(index_of(m, "cmd:beat") >= 0);
  CHECK(index_of(m, "cmd:cut") >= 0);
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
  CHECK(index_of(m, "app:bloc") < 0);

  m.backspace();
  m.backspace();
  m.backspace();
  CHECK_EQ(m.visible().size(), all);

  // Une sous-chaîne qui n'est le préfixe de rien filtre quand même : c'est
  // une recherche, pas une complétion.
  m.type(U'o');
  m.type(U'c');
  CHECK(index_of(m, "app:bloc") >= 0);

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
