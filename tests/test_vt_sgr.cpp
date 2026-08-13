#include <string>
#include <string_view>

#include "harness.hpp"
#include "vt/attrs.hpp"
#include "vt/parser.hpp"

using sshos::Color;
using sshos::Params;
using sshos::Parser;
using sshos::ParserSink;
using sshos::Style;
using sshos::apply_sgr;
namespace attr = sshos::attr;

namespace {

// On passe par le VRAI parseur au lieu de fabriquer des `Params` à la
// main. C'est lui qui décide où tombe le drapeau `sub`, et un test qui
// s'en remettrait à ma lecture de la spec ne mesurerait que ma lecture.
Style sgr(const std::string& body, Style start = Style{}) {
  struct Sink : ParserSink {
    Style st;
    void csi(const Params& p, std::string_view, uint8_t final_byte) override {
      if (final_byte == 'm') apply_sgr(p, st);
    }
  } sink;
  sink.st = start;
  Parser parser(sink);
  parser.feed("\033[" + body + "m");
  return sink.st;
}

uint16_t attrs_of(const std::string& body) { return sgr(body).attrs; }

}  // namespace

// ---------------------------------------------------------------- les bases

TEST(sgr_starts_from_a_blank_style) {
  const Style s = sgr("");
  CHECK(s == Style{});
}

// `CSI m` sans le moindre paramètre vaut `SGR 0`. C'est la forme que
// produisent quantité de scripts pour « éteindre tout », et la traiter
// comme une liste vide sans effet laisserait la couleur précédente courir
// sur le reste de l'écran.
TEST(sgr_with_no_parameter_at_all_means_reset) {
  Style painted;
  painted.fg = Color::indexed(3);
  painted.bg = Color::rgb(1, 2, 3);
  painted.attrs = attr::Bold | attr::Underline;

  CHECK(sgr("", painted) == Style{});
}

TEST(sgr_zero_resets_colours_and_attributes_together) {
  Style painted;
  painted.fg = Color::indexed(3);
  painted.attrs = attr::Bold | attr::Blink;

  CHECK(sgr("0", painted) == Style{});
}

// Un paramètre ABSENT vaut zéro, ce qui n'est pas la même chose qu'une
// liste vide : `\033[;1m` porte deux paramètres, dont le premier est vide.
TEST(sgr_treats_a_missing_parameter_as_a_reset) {
  Style painted;
  painted.fg = Color::indexed(3);

  const Style s = sgr(";1", painted);
  CHECK(s.fg == Color::def());
  CHECK_EQ(s.attrs, attr::Bold);
}

// ---------------------------------------------------------- les attributs

TEST(sgr_sets_each_attribute_on_its_own_code) {
  CHECK_EQ(attrs_of("1"), attr::Bold);
  CHECK_EQ(attrs_of("2"), attr::Dim);
  CHECK_EQ(attrs_of("3"), attr::Italic);
  CHECK_EQ(attrs_of("4"), attr::Underline);
  CHECK_EQ(attrs_of("5"), attr::Blink);
  CHECK_EQ(attrs_of("7"), attr::Reverse);
  CHECK_EQ(attrs_of("8"), attr::Hidden);
  CHECK_EQ(attrs_of("9"), attr::Strike);
}

// 6 est le clignotement RAPIDE. Aucun terminal moderne ne le distingue du
// lent, et le rendre équivalent vaut mieux que de l'ignorer : une
// application qui l'emploie attend quelque chose de visible.
TEST(sgr_folds_rapid_blink_into_blink) {
  CHECK_EQ(attrs_of("6"), attr::Blink);
}

TEST(sgr_accumulates_attributes_across_one_sequence) {
  CHECK_EQ(attrs_of("1;4;7"), attr::Bold | attr::Underline | attr::Reverse);
}

// Chaque extinction ne doit toucher QUE son attribut. Une extinction qui
// ratisse large est invisible tant qu'un seul attribut est posé, et se
// remarque le jour où du gras souligné perd son gras en cessant d'être
// souligné.
TEST(sgr_turns_off_each_attribute_without_touching_the_others) {
  const uint16_t all = attr::Bold | attr::Dim | attr::Italic | attr::Underline |
                       attr::Blink | attr::Reverse | attr::Hidden | attr::Strike;
  Style painted;
  painted.attrs = all;

  CHECK_EQ(sgr("23", painted).attrs, all & ~attr::Italic);
  CHECK_EQ(sgr("24", painted).attrs, all & ~attr::Underline);
  CHECK_EQ(sgr("25", painted).attrs, all & ~attr::Blink);
  CHECK_EQ(sgr("27", painted).attrs, all & ~attr::Reverse);
  CHECK_EQ(sgr("28", painted).attrs, all & ~attr::Hidden);
  CHECK_EQ(sgr("29", painted).attrs, all & ~attr::Strike);
}

// 22 est la seule extinction qui en vise DEUX : l'ECMA-48 n'a pas de code
// pour éteindre le gras seul, ni le faible seul.
TEST(sgr_twenty_two_clears_bold_and_dim_at_once) {
  Style painted;
  painted.attrs = attr::Bold | attr::Dim | attr::Italic;

  CHECK_EQ(sgr("22", painted).attrs, attr::Italic);
}

// 21 est le point où l'ECMA-48 et xterm divergent : la norme y met « gras
// éteint », xterm le double soulignement. On promet `xterm-256color` à
// l'invité, donc on tient la promesse -- et le gras posé avant reste posé.
TEST(sgr_twenty_one_underlines_rather_than_clearing_bold) {
  Style painted;
  painted.attrs = attr::Bold;

  const Style s = sgr("21", painted);
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
}

// ------------------------------------------------------ les couleurs de base

TEST(sgr_sets_the_eight_basic_foreground_colours) {
  for (int i = 0; i < 8; ++i) {
    CHECK(sgr(std::to_string(30 + i)).fg == Color::indexed(static_cast<uint8_t>(i)));
  }
}

TEST(sgr_sets_the_eight_basic_background_colours) {
  for (int i = 0; i < 8; ++i) {
    CHECK(sgr(std::to_string(40 + i)).bg == Color::indexed(static_cast<uint8_t>(i)));
  }
}

// Les vives sont les indices 8 à 15, pas « la même couleur en gras ». Les
// confondre est ce qui rend un thème sombre illisible sur la moitié des
// terminaux.
TEST(sgr_maps_the_bright_colours_onto_indices_eight_to_fifteen) {
  for (int i = 0; i < 8; ++i) {
    CHECK(sgr(std::to_string(90 + i)).fg == Color::indexed(static_cast<uint8_t>(8 + i)));
    CHECK(sgr(std::to_string(100 + i)).bg == Color::indexed(static_cast<uint8_t>(8 + i)));
  }
}

// 39 et 49 rendent la couleur PAR DÉFAUT, qui n'est aucun indice : c'est
// tout l'intérêt de `ColorKind::Default`.
TEST(sgr_returns_to_the_default_colour_with_thirty_nine_and_forty_nine) {
  Style painted;
  painted.fg = Color::indexed(1);
  painted.bg = Color::rgb(9, 9, 9);

  CHECK(sgr("39", painted).fg == Color::def());
  CHECK(sgr("49", painted).bg == Color::def());
  // Chacun ne touche que le sien.
  CHECK(sgr("39", painted).bg == Color::rgb(9, 9, 9));
  CHECK(sgr("49", painted).fg == Color::indexed(1));
}

// ---------------------------------------------------- les couleurs étendues

TEST(sgr_reads_a_256_colour_index) {
  CHECK(sgr("38;5;196").fg == Color::indexed(196));
  CHECK(sgr("48;5;17").bg == Color::indexed(17));
}

TEST(sgr_reads_a_truecolour_triplet) {
  CHECK(sgr("38;2;10;20;30").fg == Color::rgb(10, 20, 30));
  CHECK(sgr("48;2;255;0;127").bg == Color::rgb(255, 0, 127));
}

// La forme ISO 8613-6, en sous-paramètres. Plusieurs bibliothèques
// modernes ne produisent que celle-ci.
TEST(sgr_reads_the_colon_form_of_an_indexed_colour) {
  CHECK(sgr("38:5:196").fg == Color::indexed(196));
}

// Avec son champ d'espace colorimétrique vide au milieu, qui est la forme
// litérale de la norme -- et sans, qui est la forme abrégée courante. Lire
// les TROIS DERNIERS éléments du groupe traite les deux sans compter les
// champs vides.
TEST(sgr_reads_both_colon_spellings_of_a_truecolour_triplet) {
  CHECK(sgr("38:2::10:20:30").fg == Color::rgb(10, 20, 30));
  CHECK(sgr("38:2:10:20:30").fg == Color::rgb(10, 20, 30));
}

TEST(sgr_reads_a_colon_form_in_the_middle_of_a_sequence) {
  const Style s = sgr("1;38:2::10:20:30;4");
  CHECK(s.fg == Color::rgb(10, 20, 30));
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
}

// ------------------------------------------------- ne rien décaler, jamais

// LE piège de SGR. Une couleur étendue consomme des paramètres ; si la
// boucle avance d'un pas fixe, tout ce qui suit se lit décalé -- ici le
// `4` deviendrait un `196` sans signification, ou pire.
TEST(sgr_does_not_shift_what_follows_an_extended_colour) {
  const Style s = sgr("1;38;5;196;4");
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
  CHECK(s.fg == Color::indexed(196));
}

// Une couleur étendue TRONQUÉE ne doit rien emporter derrière elle, et
// surtout pas laisser son `5` se relire comme un clignotant.
TEST(sgr_swallows_a_truncated_extended_colour_without_side_effects) {
  const Style s = sgr("1;38;5");
  CHECK_EQ(s.attrs, attr::Bold);
  CHECK(s.fg == Color::def());
}

TEST(sgr_swallows_a_truncated_truecolour_without_side_effects) {
  const Style s = sgr("1;38;2;10;20");
  CHECK_EQ(s.attrs, attr::Bold);
  CHECK(s.fg == Color::def());
}

// `38` tout seul en fin de liste : rien à lire, rien à faire, et rien à
// planter.
TEST(sgr_survives_an_extended_colour_introducer_with_nothing_after_it) {
  const Style s = sgr("1;38");
  CHECK_EQ(s.attrs, attr::Bold);
  CHECK(s.fg == Color::def());
}

// Le soulignement ondulé de kitty. Son `3` est un SOUS-paramètre du 4 :
// lu comme un code autonome, il poserait l'italique.
TEST(sgr_does_not_read_the_underline_style_as_an_italic) {
  const Style s = sgr("4:3");
  CHECK_EQ(s.attrs & attr::Italic, 0);
  CHECK_EQ(s.attrs & attr::Underline, attr::Underline);
}

// `4:0` éteint le soulignement au lieu de le poser.
TEST(sgr_reads_the_underline_style_zero_as_no_underline) {
  Style painted;
  painted.attrs = attr::Underline | attr::Bold;

  const Style s = sgr("4:0", painted);
  CHECK_EQ(s.attrs, attr::Bold);
}

// 58 colore le soulignement. On n'a pas de champ pour ça, mais il faut
// l'avaler ENTIER : ses cinq nombres lâchés dans la boucle contiennent un
// `0` qui remettrait le style à zéro en plein milieu.
TEST(sgr_consumes_the_underline_colour_without_disturbing_the_rest) {
  const Style s = sgr("1;58:2::255:0:0;4");
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
  CHECK(s.fg == Color::def());

  const Style flat = sgr("1;58;2;255;0;0;4");
  CHECK_EQ(flat.attrs, attr::Bold | attr::Underline);
}

TEST(sgr_consumes_the_flat_form_of_fifty_eight_with_an_index) {
  const Style s = sgr("1;58;5;196;4");
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
  CHECK(s.fg == Color::def());
}

TEST(sgr_ignores_fifty_nine_without_disturbing_the_rest) {
  const Style s = sgr("1;59;4");
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
}

// -------------------------------------------------- ce qui sort des bornes

// Un indice au-delà de 255 n'est pas une couleur : le rabattre par
// troncature en donnerait une autre, silencieusement fausse. On laisse la
// couleur en place et on passe.
TEST(sgr_rejects_an_out_of_range_palette_index) {
  Style painted;
  painted.fg = Color::indexed(1);

  const Style s = sgr("38;5;300", painted);
  CHECK(s.fg == Color::indexed(1));
}

TEST(sgr_rejects_an_out_of_range_truecolour_component) {
  Style painted;
  painted.fg = Color::indexed(1);

  CHECK(sgr("38;2;300;0;0", painted).fg == Color::indexed(1));
  CHECK(sgr("38;2;0;300;0", painted).fg == Color::indexed(1));
  CHECK(sgr("38;2;0;0;300", painted).fg == Color::indexed(1));
}

// Un mode d'extension inconnu : on consomme l'introducteur et son mode,
// puis on reprend la lecture normalement. Deviner le nombre d'arguments
// d'un mode qu'on ne connaît pas est impossible ; s'arrêter là est le seul
// choix qui ne décale rien de garanti.
TEST(sgr_skips_an_unknown_extended_colour_mode) {
  const Style s = sgr("1;38;9;4");
  CHECK_EQ(s.attrs & attr::Bold, attr::Bold);
  CHECK(s.fg == Color::def());
}

// Un code inconnu se saute sans rien casser autour.
TEST(sgr_ignores_an_unknown_code) {
  const Style s = sgr("1;73;4");
  CHECK_EQ(s.attrs, attr::Bold | attr::Underline);
}

// Le style survit d'une séquence à l'autre : c'est un état, pas une
// décoration locale.
TEST(sgr_carries_the_style_across_sequences) {
  Style s;
  s.attrs = attr::Bold;
  s.fg = Color::indexed(2);

  const Style after = sgr("4", s);
  CHECK_EQ(after.attrs, attr::Bold | attr::Underline);
  CHECK(after.fg == Color::indexed(2));
}
