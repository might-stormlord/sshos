#include <string>

#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/modal.hpp"

using sshos::Border;
using sshos::Modal;
using sshos::ModalHit;
using sshos::Rect;
using sshos::Surface;
using sshos::Theme;
using sshos::View;

// Une seule modale à la fois. Une seconde demande pendant qu'une première
// attend est ignorée : empiler des dialogues sur un bureau texte ne mène
// nulle part, et l'utilisateur ne saurait plus lequel il répond.
TEST(modal_ignores_a_second_question_while_the_first_is_pending) {
  Modal m;
  CHECK(!m.is_open());
  m.ask("premiere ?", 1);
  m.ask("seconde ?", 2);
  CHECK(m.is_open());
  CHECK_EQ(m.target(), static_cast<sshos::WindowId>(1));
  CHECK_EQ(m.question(), std::string("premiere ?"));

  // Et refermer libère bien la place pour la suivante.
  m.dismiss();
  CHECK(!m.is_open());
  m.ask("seconde ?", 2);
  CHECK_EQ(m.target(), static_cast<sshos::WindowId>(2));
}

TEST(modal_defaults_to_cancel) {
  Modal m;
  m.ask("fermer ?", 1);
  CHECK(!m.confirm_focused());
  m.focus_next();
  CHECK(m.confirm_focused());
  m.focus_next();
  CHECK(!m.confirm_focused());

  // Et une nouvelle question repart d'Annuler, quel que soit l'état où la
  // précédente s'est terminée.
  m.focus_next();
  m.dismiss();
  m.ask("encore ?", 2);
  CHECK(!m.confirm_focused());
}

TEST(modal_is_centred_and_never_leaves_the_screen) {
  Modal m;
  m.ask("une question passablement longue a poser a l'utilisateur", 1);
  for (const auto dim : {std::pair<int, int>{80, 24}, {60, 20}, {40, 12}}) {
    const Rect r = m.rect(dim.first, dim.second);
    CHECK(r.x >= 0);
    CHECK(r.y >= 0);
    CHECK(r.x + r.w <= dim.first);
    CHECK(r.y + r.h <= dim.second);
    // Centrée : autant de place à gauche qu'à droite, à une cellule près.
    const int left = r.x;
    const int right = dim.first - (r.x + r.w);
    CHECK(left - right <= 1);
    CHECK(right - left <= 1);
  }
}

// Le hit-test de la modale est l'inverse de son dessin, et rien de ce qui
// est dessous n'est atteignable : c'est ce que « modal » veut dire.
TEST(modal_hit_test_matches_the_buttons_it_paints) {
  Modal m;
  CHECK(m.hit(40, 12) == ModalHit::None);  // fermée : rien ne se clique

  m.ask("fermer la fenetre ?", 1);
  m.layout(80, 24);
  const Rect r = m.rect(80, 24);

  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Ascii);

  CHECK(m.hit(r.x - 1, r.y) == ModalHit::None);
  CHECK(m.hit(r.x, r.y + r.h) == ModalHit::None);

  int cancel = 0;
  int confirm = 0;
  for (int y = r.y; y < r.y + r.h; ++y) {
    for (int x = r.x; x < r.x + r.w; ++x) {
      const ModalHit h = m.hit(x, y);
      CHECK(h != ModalHit::None);
      if (h == ModalHit::Cancel) ++cancel;
      if (h == ModalHit::Confirm) ++confirm;
    }
  }
  CHECK_EQ(cancel, 11);   // « [ Annuler ] »
  CHECK_EQ(confirm, 13);  // « [ Confirmer ] »

  // Et les glyphes sont bien là où le hit-test les annonce.
  const std::string row = s.text_row(r.y + 3);
  CHECK(row.find("[ Annuler ]") != std::string::npos);
  CHECK(row.find("[ Confirmer ]") != std::string::npos);
  CHECK(s.text_row(r.y + 1).find("fermer la fenetre ?") != std::string::npos);
}
