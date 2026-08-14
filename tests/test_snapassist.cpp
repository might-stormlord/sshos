#include <string>
#include <vector>

#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/snapassist.hpp"

using sshos::Border;
using sshos::Rect;
using sshos::SnapAssist;
using sshos::SnapCandidate;
using sshos::Surface;
using sshos::Theme;
using sshos::View;

namespace {

std::vector<SnapCandidate> two() {
  return {SnapCandidate{7, "Fichiers"}, SnapCandidate{9, "Editeur"}};
}

std::string painted(const SnapAssist& a, int w, int h) {
  Surface s(w, h);
  View v = s.root();
  a.draw(v, Theme::mono16(), Border::Ascii);
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = s.text_row(y);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

}  // namespace

TEST(snapassist_opens_in_the_free_half_and_names_the_candidates) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, two());

  REQUIRE(a.is_open());
  const std::string screen = painted(a, 40, 12);
  CHECK(screen.find("Fichiers") != std::string::npos);
  CHECK(screen.find("Editeur") != std::string::npos);
}

// RIEN À PROPOSER, RIEN À MONTRER : un cadre vide au milieu de l'écran ne
// dit pas « il n'y a pas d'autre fenêtre », il dit « quelque chose ne va
// pas ».
TEST(snapassist_stays_shut_without_a_candidate) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, {});

  CHECK(!a.is_open());
}

// UNE MOITIÉ TROP PETITE NE REÇOIT RIEN. Le cadre déborderait, et View
// clipperait -- l'utilisateur verrait un demi-cadre sans savoir sur quoi
// cliquer.
TEST(snapassist_stays_shut_when_the_free_half_cannot_hold_it) {
  SnapAssist a;
  a.open(Rect{0, 0, 8, 12}, two());
  CHECK(!a.is_open());

  SnapAssist b;
  b.open(Rect{0, 0, 30, 2}, two());
  CHECK(!b.is_open());
}

// LE CLIC EST L'INVERSE DU DESSIN, ligne par ligne. C'est la même
// discipline que pour le panneau et les décorations : ce qu'on clique doit
// être ce qu'on voit.
TEST(snapassist_hit_test_matches_the_rows_it_paints) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, two());
  REQUIRE(a.is_open());

  Surface s(40, 12);
  View v = s.root();
  a.draw(v, Theme::mono16(), Border::Ascii);

  int found = 0;
  for (int y = 0; y < 12; ++y) {
    const std::string row = s.text_row(y);
    for (const SnapCandidate& c : two()) {
      if (row.find(c.title) == std::string::npos) continue;
      ++found;
      const int x = static_cast<int>(row.find(c.title));
      CHECK_EQ(a.hit(x, y), c.win);
    }
  }
  CHECK_EQ(found, 2);
}

TEST(snapassist_answers_nothing_outside_its_frame) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, two());
  REQUIRE(a.is_open());

  CHECK_EQ(a.hit(a.rect().x - 1, a.rect().y + 1), sshos::WindowId{0});
  CHECK_EQ(a.hit(a.rect().x + 1, a.rect().y - 1), sshos::WindowId{0});
  CHECK_EQ(a.hit(a.rect().x + a.rect().w, a.rect().y + 1), sshos::WindowId{0});
}

// LA BORDURE ET LE TITRE NE SONT PAS DES CHOIX. Cliquer le cadre ancrerait
// la première fenêtre de la liste sans que personne l'ait désignée.
TEST(snapassist_answers_nothing_on_its_border_or_its_heading) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, two());
  REQUIRE(a.is_open());

  const Rect r = a.rect();
  CHECK_EQ(a.hit(r.x, r.y), sshos::WindowId{0});
  CHECK_EQ(a.hit(r.x + 2, r.y), sshos::WindowId{0});
  CHECK_EQ(a.hit(r.x + 2, r.y + 1), sshos::WindowId{0});
}

TEST(snapassist_closes_and_forgets_what_it_offered) {
  SnapAssist a;
  a.open(Rect{20, 0, 20, 12}, two());
  REQUIRE(a.is_open());

  a.close();

  CHECK(!a.is_open());
  CHECK(a.choices().empty());
  CHECK_EQ(a.hit(a.rect().x + 2, a.rect().y + 2), sshos::WindowId{0});
}

// Un titre plus long que le cadre est ÉLIDÉ, pas débordé : la bordure
// droite doit rester une bordure.
TEST(snapassist_elides_a_title_wider_than_its_frame) {
  SnapAssist a;
  a.open(Rect{0, 0, 24, 12},
         {SnapCandidate{3, "Terminal (une-machine-au-nom-tres-long)"}});
  REQUIRE(a.is_open());

  Surface s(24, 12);
  View v = s.root();
  a.draw(v, Theme::mono16(), Border::Ascii);
  const Rect r = a.rect();
  for (int y = r.y; y < r.y + r.h; ++y) {
    CHECK_EQ(s.at(r.x + r.w - 1, y).ch, s.at(r.x, y).ch);
  }
}

// ON NE PROPOSE QUE CE QU'ON MONTRE. Une liste plus longue que le cadre
// peindrait ses dernières lignes hors du cadre, et le clic répondrait pour
// des fenêtres qu'on ne voit nulle part.
TEST(snapassist_offers_only_what_its_frame_can_hold) {
  SnapAssist a;
  a.open(Rect{0, 0, 30, 6},
         {SnapCandidate{1, "Un"}, SnapCandidate{2, "Deux"},
          SnapCandidate{3, "Trois"}, SnapCandidate{4, "Quatre"}});
  REQUIRE(a.is_open());

  const Rect r = a.rect();
  CHECK_EQ(a.choices().size(), static_cast<size_t>(r.h - 3));
  // Et rien ne répond sous la bordure basse.
  for (int y = r.y + r.h - 1; y < 6; ++y) {
    CHECK_EQ(a.hit(r.x + 2, y), sshos::WindowId{0});
  }
}
