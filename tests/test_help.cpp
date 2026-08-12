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
  // Une ligne par accord, plus le cadre, l'en-tête et sa ligne vide.
  CHECK_EQ(r.h, static_cast<int>(sshos::binding_help().size()) + 4);
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
