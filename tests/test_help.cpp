#include <string>

#include "harness.hpp"
#include "input/shortcuts.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "render/width.hpp"
#include "shell/help.hpp"

using sshos::Border;
using sshos::Help;
using sshos::Rect;
using sshos::Surface;
using sshos::Theme;
using sshos::View;

namespace {

Help opened() {
  Help h;
  h.open();
  return h;
}

}  // namespace

TEST(help_draws_nothing_while_closed) {
  Help h;
  Surface s(80, 24);
  View v = s.root();
  h.layout(80, 24);
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", false);
  for (int y = 0; y < 24; ++y) {
    CHECK(s.text_row(y).find_first_not_of(' ') == std::string::npos);
  }
}

TEST(help_is_centred_and_sized_to_its_table) {
  Help h = opened();
  const Rect r = h.rect(80, 24);
  // Une ligne par accord, une par geste direct, plus le cadre, l'en-tête et
  // sa ligne vide, plus la ligne vide et le titre de la seconde section.
  CHECK_EQ(r.h, static_cast<int>(sshos::binding_help().size()) +
                    static_cast<int>(sshos::direct_help().size()) + 5);
  CHECK_EQ(r.x, (80 - r.w) / 2);
  CHECK_EQ(r.y, (24 - r.h) / 2);
  CHECK(r.w < 80);  // elle ne s'étire pas à tout l'écran
}

TEST(help_names_the_leader_it_was_given) {
  Help h = opened();
  Surface s(80, 24);
  View v = s.root();
  h.layout(80, 24);
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+B", false);

  bool found = false;
  for (int y = 0; y < 24; ++y) {
    found = found || s.text_row(y).find("Ctrl+B puis :") != std::string::npos;
  }
  CHECK(found);
}

TEST(help_lists_every_row_of_the_table_when_it_fits) {
  Help h = opened();
  Surface s(100, 30);
  View v = s.root();
  h.layout(100, 30);
  h.draw(v, Theme::mono16(), Border::Unicode, "Ctrl+A", true);

  std::string all;
  for (int y = 0; y < 30; ++y) all += s.text_row(y);
  for (const auto& row : sshos::binding_help()) {
    if (all.find(row.keys) == std::string::npos) {
      th::fail(__FILE__, __LINE__,
               std::string("l'aide n'affiche pas la ligne « ") + row.keys + " »");
    }
  }
}

// Sans UTF-8, les accents sont rabattus plutôt qu'envoyés en octets bruts :
// le terminal du client afficherait sinon du charabia là où il doit lire
// une aide.
TEST(help_folds_its_accents_when_the_client_has_no_utf8) {
  Help h = opened();
  Surface s(100, 30);
  View v = s.root();
  h.layout(100, 30);
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", false);

  std::string all;
  for (int y = 0; y < 30; ++y) all += s.text_row(y);
  CHECK(all.find("Deplacer la fenetre") != std::string::npos);
  CHECK(all.find("Detacher") != std::string::npos);
  // Et pas un seul octet hors ASCII n'est parti sur le fil.
  for (const char c : all) {
    CHECK((static_cast<unsigned char>(c) & 0x80) == 0);
  }
}

// Le défaut trouvé à la sonde, à 40x12 : les colonnes sont plus larges que
// le cadre, View::text clippe à la SURFACE et non au cadre, et le texte
// mange la bordure droite. Un cadre troué se lit comme un bug d'affichage,
// et c'est le seul écran que l'utilisateur perdu aura sous les yeux.
TEST(help_never_lets_its_text_eat_the_frame) {
  const struct {
    int w;
    int h;
  } sizes[] = {{40, 12}, {44, 14}, {50, 16}, {60, 20}, {80, 24}, {30, 10}};

  for (const auto& sz : sizes) {
    Help h = opened();
    Surface s(sz.w, sz.h);
    View v = s.root();
    h.layout(sz.w, sz.h);
    h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", false);

    const Rect r = h.rect(sz.w, sz.h);
    const int right = r.x + r.w - 1;
    const int bottom = r.y + r.h - 1;
    for (int y = r.y + 1; y < bottom; ++y) {
      if (s.at(right, y).ch != U'|' || s.at(r.x, y).ch != U'|') {
        th::fail(__FILE__, __LINE__,
                 "cadre troue en " + std::to_string(sz.w) + "x" +
                     std::to_string(sz.h) + " ligne " + std::to_string(y));
      }
    }
    // Et rien n'est écrit hors du cadre.
    for (int y = 0; y < sz.h; ++y) {
      if (y >= r.y && y <= bottom) continue;
      CHECK(s.text_row(y).find_first_not_of(' ') == std::string::npos);
    }
  }
}

// Un terminal trop court coupe la table plutôt que de déborder. Les accords
// les plus courants sont en tête : ce sont les derniers à disparaître qui
// comptent le moins.
TEST(help_clips_its_table_on_a_short_terminal) {
  Help h = opened();
  const Rect r = h.rect(80, 10);
  CHECK_EQ(r.h, 10);
  CHECK_EQ(r.y, 0);

  Surface s(80, 10);
  View v = s.root();
  h.layout(80, 10);
  // En UTF-8 : la table s'y lit telle qu'elle est écrite, sans repli, ce
  // qui laisse comparer aux libellés d'origine.
  h.draw(v, Theme::mono16(), Border::Unicode, "Ctrl+A", true);

  std::string all;
  for (int y = 0; y < 10; ++y) all += s.text_row(y);
  // La première ligne de la table survit toujours ; la dernière, non.
  CHECK(all.find(sshos::binding_help().front().what) != std::string::npos);
  CHECK(all.find(sshos::binding_help().back().what) == std::string::npos);
}

// La marque « s'enchaîne » est dérivée de la table, jamais recopiée. Elle
// doit donc se poser sur exactement les lignes qui s'enchaînent -- ni une de
// plus, sinon elle ment ; ni une de moins, sinon elle est inutile.
TEST(help_marks_exactly_the_gestures_that_chain) {
  Help h = opened();
  Surface s(80, 30);
  View v = s.root();
  h.layout(80, 30);
  const Rect r = h.rect(80, 30);
  h.draw(v, Theme::mono16(), Border::Unicode, "Ctrl+A", true);

  int marked = 0;
  int expected = 0;
  const auto& rows = sshos::binding_help();
  for (size_t i = 0; i < rows.size(); ++i) {
    const int y = r.y + 3 + static_cast<int>(i);
    if (y >= r.y + r.h - 1) break;
    const std::string line = s.text_row(y);
    const bool chains = !rows[i].actions.empty() &&
                        sshos::is_repeatable(rows[i].actions.front());
    const bool has_mark = line.find("∙") != std::string::npos;
    CHECK(has_mark == chains);
    if (has_mark) ++marked;
    if (chains) ++expected;
  }
  // Et la légende de l'en-tête porte la même marque, sans quoi personne ne
  // saurait ce qu'elle veut dire.
  CHECK(s.text_row(r.y + 1).find("∙") != std::string::npos);
  CHECK_EQ(marked, expected);
  CHECK(expected >= 3);
}

// LES GESTES SANS ACCORD ONT LEUR SECTION. Ils ne passent pas par la touche
// leader, donc ce ne sont pas des `Action` : les deux gardes de couverture
// de test_shortcuts.cpp ne les voient pas, et sans table à eux ils
// n'existaient nulle part -- j'ai livré les onglets du terminal avec quatre
// raccourcis que rien ne citait.
TEST(help_lists_the_gestures_that_need_no_chord) {
  Help h;
  h.open();
  h.layout(100, 40);
  Surface s(100, 40);
  View v = s.root();
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", true);

  std::string screen;
  for (int y = 0; y < 40; ++y) screen += s.text_row(y) + "\n";
  CHECK(screen.find("Ctrl+fleches") != std::string::npos ||
        screen.find("Ctrl+flèches") != std::string::npos);
  CHECK(screen.find("Alt+t") != std::string::npos);
  CHECK(screen.find("F2") != std::string::npos);
}

// La section directe est SOUS la table des accords, et séparée d'elle :
// mélangées, on ne saurait plus lesquelles demandent le leader.
TEST(help_keeps_the_direct_gestures_below_the_chords) {
  Help h;
  h.open();
  h.layout(100, 40);
  Surface s(100, 40);
  View v = s.root();
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", true);

  int last_chord = -1;
  int first_direct = -1;
  for (int y = 0; y < 40; ++y) {
    const std::string row = s.text_row(y);
    if (row.find("Cette aide") != std::string::npos) last_chord = y;
    if (first_direct < 0 && row.find("Sans accord") != std::string::npos) {
      first_direct = y;
    }
  }
  REQUIRE(last_chord >= 0);
  REQUIRE(first_direct >= 0);
  CHECK(first_direct > last_chord);
}

// Toute ligne directe est nommée ET expliquée : une touche sans effet écrit
// à côté ne documente rien.
TEST(help_explains_every_direct_gesture_it_names) {
  for (const auto& r : sshos::direct_help()) {
    CHECK(std::string(r.keys).size() > 0);
    CHECK(std::string(r.what).size() > 0);
  }
  CHECK(sshos::direct_help().size() >= size_t{4});
}

// L'AIDE ENTIÈRE TIENT DANS UN 80x24, la taille de terminal la plus
// répandue. C'est la contrainte qui a décidé de sa mise en page : une ligne
// vide de séparation en plus, ou une ligne de raccourci de plus, et la
// dernière ligne -- celle des gestes qu'on ne peut découvrir autrement --
// tombait hors du cadre.
TEST(help_fits_whole_on_the_most_common_terminal) {
  Help h;
  h.open();
  const Rect r = h.rect(80, 24);
  CHECK(r.h <= 24);
  CHECK_EQ(r.h, static_cast<int>(sshos::binding_help().size()) +
                    static_cast<int>(sshos::direct_help().size()) + 5);

  h.layout(80, 24);
  Surface s(80, 24);
  View v = s.root();
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", true);
  std::string screen;
  for (int y = 0; y < 24; ++y) screen += s.text_row(y) + "\n";
  // La DERNIÈRE ligne de la seconde table, celle qui tombe la première.
  CHECK(screen.find("F2") != std::string::npos);
  CHECK(screen.find("renommer") != std::string::npos);
}

// AUCUNE LIGNE N'EST TRONQUÉE quand la place ne manque pas -- ni son nom de
// touche, ni son effet, dans l'une comme dans l'autre table. La colonne des
// touches se mesure sur les DEUX : aujourd'hui la plus longue de chaque
// table fait la même largeur, ce qui masquerait une mesure qui n'en
// regarderait qu'une seule. Ce cas ne dépend pas de cette coïncidence.
TEST(help_writes_every_row_of_both_tables_in_full) {
  Help h;
  h.open();
  h.layout(120, 44);
  Surface s(120, 44);
  View v = s.root();
  h.draw(v, Theme::mono16(), Border::Ascii, "Ctrl+A", true);

  std::string screen;
  for (int y = 0; y < 44; ++y) screen += s.text_row(y) + "\n";
  for (const auto& r : sshos::binding_help()) {
    CHECK(screen.find(r.keys) != std::string::npos);
    CHECK(screen.find(r.what) != std::string::npos);
  }
  for (const auto& r : sshos::direct_help()) {
    CHECK(screen.find(r.keys) != std::string::npos);
    CHECK(screen.find(r.what) != std::string::npos);
  }
}
