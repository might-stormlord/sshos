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

// --- la progression chiffree ---------------------------------------------
//
// Cinq libelles couvraient une a deux minutes d'attente : la boite disait
// « compilation... » et ne bougeait plus. Une barre et un chiffre disent
// AUSSI que ca avance, ce qu'un libelle fige ne dit pas.

namespace {

std::string tout_le_cadre(const Surface& s) {
  std::string g;
  for (int y = 0; y < 24; ++y) g += s.text_row(y) + "\n";
  return g;
}

}  // namespace

TEST(modal_draws_a_bar_and_a_figure_for_a_progress_it_can_measure) {
  Modal m;
  m.progress("Mise a jour en cours : compilation...");
  m.set_progress(47);
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Unicode);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("47%") != std::string::npos);
  CHECK(g.find("█") != std::string::npos);  // du plein
  CHECK(g.find("░") != std::string::npos);  // du vide
}

// SANS CHIFFRE, PAS DE BARRE. Une installation mise a jour par un script
// plus ancien n'en depose aucun, et une barre a zero laisserait croire
// qu'il ne se passe rien. La boite reste alors exactement celle d'avant.
TEST(modal_draws_no_bar_when_the_progress_is_unknown) {
  Modal m;
  m.progress("Mise a jour en cours : compilation...");
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Unicode);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("%") == std::string::npos);
  CHECK(g.find("█") == std::string::npos);
}

// Le repli ASCII vaut ici comme pour les cadres : Modal ne sait pas si le
// client accepte l'UTF-8, c'est la bordure qui le lui dit.
TEST(modal_falls_back_to_ascii_for_its_bar) {
  Modal m;
  m.progress("Mise a jour en cours : suite de tests...");
  m.set_progress(80);
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Ascii);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("80%") != std::string::npos);
  CHECK(g.find("#") != std::string::npos);
  CHECK(g.find("█") == std::string::npos);
}

// La barre n'appartient qu'a une progression : une question ou un constat
// n'ont rien a mesurer, et un chiffre pose la ferait lire comme un travail
// en cours.
TEST(modal_keeps_its_bar_out_of_a_question) {
  Modal m;
  m.ask("Installer la mise a jour ?", 0);
  m.set_progress(47);
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Unicode);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("47%") == std::string::npos);
}

// Une boite DEJA ouverte suit le travail sans clignoter : c'est la meme
// boite, son chiffre change. Sans quoi il faudrait la fermer et la rouvrir.
TEST(modal_lets_a_running_progress_move_its_figure) {
  Modal m;
  m.progress("Mise a jour en cours : compilation...");
  m.set_progress(10);
  m.set_progress(63);
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Unicode);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("63%") != std::string::npos);
  CHECK(g.find("10%") == std::string::npos);
}

// UN TRAVAIL NEUF REPART SANS CHIFFRE. La boite peut etre reutilisee sans
// passer par dismiss() -- une progression cede la place a une autre -- et le
// pourcentage du travail precedent ferait alors demarrer le suivant a 93%.
// (Campagne de mutation, M9.)
TEST(modal_starts_a_new_progress_without_a_figure) {
  Modal m;
  m.progress("un premier travail");
  m.set_progress(93);
  m.progress("un second travail");
  m.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  m.draw(v, Theme::mono16(), Border::Unicode);

  const std::string g = tout_le_cadre(s);
  CHECK(g.find("second travail") != std::string::npos);
  CHECK(g.find("93%") == std::string::npos);
  CHECK(g.find("█") == std::string::npos);
}
