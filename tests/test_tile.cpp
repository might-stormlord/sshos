#include <string>
#include <vector>

#include "harness.hpp"
#include "wm/tile.hpp"

using sshos::Rect;
using sshos::tile_rects;

namespace {

// Une transcription lisible d'un coup d'œil : « x,y wxh » par fenêtre.
std::string dump(const std::vector<Rect>& v) {
  std::string out;
  for (const Rect& r : v) {
    if (!out.empty()) out += " | ";
    out += std::to_string(r.x) + "," + std::to_string(r.y) + " " +
           std::to_string(r.w) + "x" + std::to_string(r.h);
  }
  return out;
}

// La zone couverte, en cellules, sans compter deux fois un chevauchement.
int covered(const std::vector<Rect>& v, const Rect& work) {
  int n = 0;
  for (int y = work.y; y < work.y + work.h; ++y) {
    for (int x = work.x; x < work.x + work.w; ++x) {
      int hits = 0;
      for (const Rect& r : v) {
        if (r.contains(x, y)) ++hits;
      }
      if (hits > 1) return -1;  // chevauchement : la sanction est visible
      n += hits;
    }
  }
  return n;
}

constexpr Rect kWork{0, 0, 80, 20};

}  // namespace

TEST(tile_places_nothing_for_no_window) {
  CHECK(tile_rects(kWork, 0).empty());
  CHECK(tile_rects(kWork, -3).empty());
}

// Une seule fenêtre prend TOUTE la zone. C'est le cas le plus fréquent, et
// lui laisser une marge donnerait l'impression d'un rangement raté.
TEST(tile_gives_the_whole_area_to_a_single_window) {
  CHECK_EQ(dump(tile_rects(kWork, 1)), std::string("0,0 80x20"));
}

// LE CAS QUE TOUT LE MONDE ATTEND : deux fenêtres, deux moitiés côte à
// côte, pleine hauteur.
TEST(tile_splits_the_area_in_two_halves_side_by_side) {
  CHECK_EQ(dump(tile_rects(kWork, 2)), std::string("0,0 40x20 | 40,0 40x20"));
}

// Trois : la colonne de gauche coupée en deux, celle de droite pleine
// hauteur. Le reste de division va à la PREMIÈRE colonne.
TEST(tile_gives_three_windows_two_columns) {
  CHECK_EQ(dump(tile_rects(kWork, 3)),
           std::string("0,0 40x10 | 0,10 40x10 | 40,0 40x20"));
}

TEST(tile_gives_four_windows_a_two_by_two_grid) {
  CHECK_EQ(dump(tile_rects(kWork, 4)),
           std::string("0,0 40x10 | 0,10 40x10 | 40,0 40x10 | 40,10 40x10"));
}

// La zone est remplie SANS TROU ET SANS CHEVAUCHEMENT, quel que soit le
// nombre. C'est la propriété qui compte, et elle se vérifie cellule par
// cellule plutôt qu'à la lecture des rectangles.
TEST(tile_covers_the_area_exactly_whatever_the_count) {
  for (int n = 1; n <= 9; ++n) {
    const std::vector<Rect> v = tile_rects(kWork, n);
    CHECK_EQ(static_cast<int>(v.size()), n);
    CHECK_EQ(covered(v, kWork), kWork.w * kWork.h);
  }
}

// Une zone qui ne se divise pas rond : les restes vont aux PREMIÈRES
// colonnes et aux PREMIÈRES lignes. Répartis autrement, un aller-retour de
// rangement ferait glisser les fenêtres d'une cellule à chaque fois.
TEST(tile_gives_the_remainder_to_the_first_column) {
  const Rect odd{0, 0, 81, 21};
  const std::vector<Rect> v = tile_rects(odd, 2);
  REQUIRE_EQ(v.size(), size_t{2});
  CHECK_EQ(v[0].w, 41);
  CHECK_EQ(v[1].w, 40);
  CHECK_EQ(covered(v, odd), odd.w * odd.h);
}

// Le rangement est IDEMPOTENT : ranger deux fois de suite ne bouge plus
// rien. Sans cela, la commande deviendrait un tic nerveux.
TEST(tile_is_idempotent) {
  const std::vector<Rect> once = tile_rects(kWork, 5);
  const std::vector<Rect> twice = tile_rects(kWork, 5);
  CHECK_EQ(dump(once), dump(twice));
}

// La zone de travail n'est pas toujours en haut à gauche : le panneau peut
// être en haut ou à gauche, et le rangement doit s'y caler.
TEST(tile_respects_an_offset_work_area) {
  const Rect shifted{10, 3, 40, 12};
  const std::vector<Rect> v = tile_rects(shifted, 2);
  REQUIRE_EQ(v.size(), size_t{2});
  CHECK_EQ(v[0].x, 10);
  CHECK_EQ(v[0].y, 3);
  CHECK_EQ(covered(v, shifted), shifted.w * shifted.h);
}

// Une zone dégénérée ne doit pas produire de rectangles de largeur nulle
// -- une fenêtre de zéro colonne serait invisible et impossible à
// rattraper à la souris.
TEST(tile_never_produces_an_empty_rectangle) {
  const std::vector<Rect> v = tile_rects(Rect{0, 0, 3, 2}, 6);
  REQUIRE_EQ(v.size(), size_t{6});
  for (const Rect& r : v) {
    CHECK(r.w >= 1);
    CHECK(r.h >= 1);
  }
}

// ------------------------------------------------------------- l'ancrage

using sshos::snap_opposite;
using sshos::snap_rect;
using sshos::SnapDir;

TEST(snap_gives_half_the_width_at_full_height) {
  CHECK_EQ(dump({snap_rect(kWork, SnapDir::Left)}), std::string("0,0 40x20"));
  CHECK_EQ(dump({snap_rect(kWork, SnapDir::Right)}), std::string("40,0 40x20"));
}

TEST(snap_gives_half_the_height_at_full_width) {
  CHECK_EQ(dump({snap_rect(kWork, SnapDir::Up)}), std::string("0,0 80x10"));
  CHECK_EQ(dump({snap_rect(kWork, SnapDir::Down)}), std::string("0,10 80x10"));
}

// DOS À DOS SANS TROU NI CHEVAUCHEMENT, y compris sur une largeur impaire
// -- c'est tout l'intérêt de donner le reste à la moitié gauche.
TEST(snap_halves_meet_exactly_on_an_odd_size) {
  const Rect odd{0, 0, 81, 21};
  const std::vector<Rect> pair = {snap_rect(odd, SnapDir::Left),
                                  snap_rect(odd, SnapDir::Right)};
  CHECK_EQ(covered(pair, odd), odd.w * odd.h);

  const std::vector<Rect> stack = {snap_rect(odd, SnapDir::Up),
                                   snap_rect(odd, SnapDir::Down)};
  CHECK_EQ(covered(stack, odd), odd.w * odd.h);
}

// La moitié OPPOSÉE est exactement celle qui reste : c'est là que le
// bureau proposera l'autre fenêtre.
TEST(snap_opposite_is_the_half_that_stays_free) {
  for (SnapDir d : {SnapDir::Left, SnapDir::Right, SnapDir::Up, SnapDir::Down}) {
    const std::vector<Rect> pair = {snap_rect(kWork, d), snap_opposite(kWork, d)};
    CHECK_EQ(covered(pair, kWork), kWork.w * kWork.h);
  }
}

TEST(snap_respects_an_offset_work_area) {
  const Rect shifted{10, 3, 40, 12};
  CHECK_EQ(dump({snap_rect(shifted, SnapDir::Left)}), std::string("10,3 20x12"));
  CHECK_EQ(dump({snap_rect(shifted, SnapDir::Down)}), std::string("10,9 40x6"));
}
