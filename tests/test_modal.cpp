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

// LE CADRE DOIT TENIR LES BOUTONS QU'ON LUI A DONNES, PAS CEUX PAR DEFAUT.
//
// Le plancher de largeur etait calcule une fois pour toutes sur « Annuler »
// et « Confirmer », alors que cancel_rect() et confirm_rect() se posent a
// partir des libelles REELS. Avec « Plus tard » / « Mettre a jour » sur un
// corps court, le bouton de gauche sortait du cadre par la gauche : peint
// sur le bureau, et INCLIQUABLE puisque hit() exige d'abord que le point
// soit dans rect_. Avec « Reinstaller depuis GitHub », il sortait
// entierement -- et hit() rendait Confirm sur presque toute la largeur du
// cadre, si bien qu'un clic a cote lancait la reinstallation.
//
// C'est le defaut de 3512ffe revenu par la porte des LIBELLES au lieu du
// corps. Les trois couples ci-dessous sont ceux que la session pose vraiment
// (src/daemon/session.cpp), sur les corps les plus courts qu'elle produise.
TEST(modal_frames_the_buttons_it_was_given_not_the_default_ones) {
  const struct {
    const char* body;
    const char* cancel;
    const char* confirm;
  } cas[] = {
      {"Echec, voir update.log", "Plus tard", "Mettre a jour"},
      {"Mise a jour disponible.", "Plus tard", "Reinstaller depuis GitHub"},
      {"Mise a jour installee.", "Plus tard", "Redemarrer"},
      {"fermer ?", "Annuler", "Confirmer"},
      // Longueurs inversees : sans ce couple, la branche du partage qui
      // preserve le bouton de DROITE n'est jamais empruntee.
      {"Revenir en arriere ?", "Revenir a la version precedente", "Non"},
  };
  // 40 colonnes ne peuvent PAS porter « [ Plus tard ] [ Reinstaller depuis
  // GitHub ] » : la ou le plancher ne tient pas, les boutons se rognent, mais
  // ils restent dans le cadre. Un bouton peint dehors est perdu deux fois --
  // il salit le bureau, et hit() ne le rend jamais.
  for (const int cols : {80, 60, 40, 30}) {
    for (const auto& c : cas) {
      Modal m;
      m.ask(c.body, 1, c.cancel, c.confirm);
      m.layout(cols, 24);
      const Rect r = m.rect(cols, 24);

      Surface s(cols, 24);
      View v = s.root();
      m.draw(v, Theme::mono16(), Border::Ascii);

      const std::string annuler = std::string("[ ") + c.cancel + " ]";
      const std::string confirmer = std::string("[ ") + c.confirm + " ]";

      // 1. RIEN N'EST PEINT HORS DU CADRE. La vue recue est pleine largeur :
      // c'est a la modale de se tenir, personne ne la borne pour elle.
      for (int y = 0; y < 24; ++y) {
        const std::string row = s.text_row(y);
        for (int x = 0; x < static_cast<int>(row.size()); ++x) {
          if (row[x] == ' ') continue;
          const bool dedans =
              x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
          CHECK(dedans);
        }
      }

      // 2. CHAQUE CELLULE PEINTE D'UN BOUTON REPOND AU CLIC, et le compte est
      // la largeur ENTIERE du libelle encadre des que le cadre peut la
      // porter : un bouton a cheval sur la bordure en rendrait moins, un
      // bouton entierement dehors en rendrait zero.
      int cancel = 0;
      int confirm = 0;
      int ca = -1, cb = -1, ka = -1, kb = -1;  // les deux plages, [a, b)
      int ligne = -1;
      for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
          const ModalHit h = m.hit(x, y);
          if (h == ModalHit::Cancel) {
            ++cancel;
            if (ca < 0) { ca = x; ligne = y; }
            cb = x + 1;
          }
          if (h == ModalHit::Confirm) {
            ++confirm;
            if (ka < 0) ka = x;
            kb = x + 1;
          }
        }
      }
      REQUIRE(ca >= 0);
      REQUIRE(ka >= 0);
      REQUIRE(ligne >= 0);

      // 3. LES DEUX BOUTONS NE SE CHEVAUCHENT PAS, ET IL RESTE UN BLANC
      // ENTRE EUX. C'est ce que draw() romprait s'il peignait le libelle
      // ENTIER la ou la geometrie n'a reserve qu'une partie : le texte du
      // bouton de gauche mordrait sur celui de droite, et l'on cliquerait
      // « Confirmer » sur des lettres qui disent « Plus tard ».
      CHECK(cb <= ka);
      const std::string row = s.text_row(ligne);
      CHECK(cb < static_cast<int>(row.size()) && row[cb] == ' ');

      // 4. LA MEME MARGE DES DEUX COTES. Le bouton de droite s'arrete a deux
      // colonnes de la bordure ; celui de gauche ne doit pas s'en approcher
      // davantage, sans quoi il se colle au liseré.
      CHECK(ca >= r.x + 2);
      CHECK(kb <= r.x + r.w - 2);
      const int besoin =
          static_cast<int>(annuler.size() + confirmer.size()) + 5;
      if (cols >= besoin + 4) {
        CHECK_EQ(cancel, static_cast<int>(annuler.size()));
        CHECK_EQ(confirm, static_cast<int>(confirmer.size()));
      } else {
        // 5. LE PARTAGE, QUAND LE CADRE NE PEUT PAS TOUT PORTER. Aucun des
        // deux n'est efface -- un bouton de largeur nulle n'est plus
        // cliquable, et c'est la SORTIE de la question que l'utilisateur
        // perdrait -- et celui qui tient dans sa moitie garde son libelle
        // ENTIER : on ne rogne que ce qui deborde.
        CHECK(cancel > 0);
        CHECK(confirm > 0);
        const int place = r.w - 5;  // les deux boutons, sans le chrome
        if (static_cast<int>(annuler.size()) <= place / 2) {
          CHECK_EQ(cancel, static_cast<int>(annuler.size()));
        }
        if (static_cast<int>(confirmer.size()) <= place - place / 2) {
          CHECK_EQ(confirm, static_cast<int>(confirmer.size()));
        }
      }

      // 6. DRAW() NE PEINT QUE CE QUE HIT() ATTRIBUE. Une cellule de
      // l'interieur qui n'appartient a aucun bouton doit etre blanche : un
      // libelle peint plus large que le rectangle que la geometrie lui a
      // reserve mordrait sur l'espace de separation, sur son voisin, ou sur
      // le liseré -- et l'on cliquerait « Confirmer » sur des lettres qui
      // disent « Plus tard ».
      for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
        const ModalHit h = m.hit(x, ligne);
        if (h == ModalHit::Cancel || h == ModalHit::Confirm) continue;
        CHECK(x < static_cast<int>(row.size()) && row[x] == ' ');
      }
    }
  }
}

// UNE PROGRESSION NE PORTE PAS DE BOUTONS, DONC ELLE NE S'ELARGIT PAS POUR
// EUX -- et ce qui le garantit est que dismiss() EFFACE les libelles.
//
// C'est la precondition sur laquelle repose l'equivalence declaree dans
// min_width() : ni progress() ni inform() ne les remettent a zero, eux, et
// ils ne le peuvent pas -- ils refusent de s'ouvrir sur une boite qui n'est
// pas deja une progression. Le jour ou dismiss() cesserait de nettoyer, une
// progression herittant de « Reinstaller depuis GitHub » grandirait de
// dix-huit colonnes au moment precis ou elle perd ses boutons. Ce cas est
// donc le garde de cette propriete-la, pas de la garde sur le style.
TEST(modal_keeps_its_width_when_a_progress_follows_a_long_question) {
  Modal court;
  court.progress("compilation...");
  const Rect nu = court.rect(80, 24);

  Modal apres;
  apres.ask("Installer la mise a jour ?", 0, "Plus tard",
            "Reinstaller depuis GitHub");
  const Rect question = apres.rect(80, 24);
  apres.dismiss();
  apres.progress("compilation...");
  const Rect progression = apres.rect(80, 24);

  // La question, elle, s'elargit bien pour ses boutons.
  CHECK(question.w > nu.w);
  // La progression qui lui succede retrouve la largeur d'une progression.
  CHECK_EQ(progression.w, nu.w);
}
