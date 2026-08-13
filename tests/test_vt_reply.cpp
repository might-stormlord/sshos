#include <string>
#include <string_view>

#include "harness.hpp"
#include "vt/modes.hpp"
#include "vt/parser.hpp"
#include "vt/reply.hpp"

using sshos::Modes;
using sshos::Params;
using sshos::Parser;
using sshos::ParserSink;

namespace {

// On passe par le VRAI parseur : une requête est une séquence, et c'est
// lui qui décide où tombent le marqueur privé et les intermédiaires.
// Fabriquer les arguments à la main ne mesurerait que ma lecture de la
// spec -- et `CSI ? 1 $ p` en porte DEUX, un de chaque sorte.
std::string ask(const std::string& seq, const Modes& modes = Modes{},
                int cx = 0, int cy = 0) {
  struct Sink : ParserSink {
    std::string out;
    const Modes* modes = nullptr;
    int cx = 0;
    int cy = 0;
    void csi(const Params& p, std::string_view intermediates,
             uint8_t final_byte) override {
      out += sshos::reply_for_csi(p, intermediates, final_byte, cx, cy, *modes);
    }
  } sink;
  sink.modes = &modes;
  sink.cx = cx;
  sink.cy = cy;
  Parser parser(sink);
  parser.feed(seq);
  return sink.out;
}

}  // namespace

// ------------------------------------------------------------- l'identité

// Elle décrit NOTRE émulateur, jamais celui du client -- et elle est
// stable : un invité qui la relit lit la même chose.
TEST(reply_gives_a_stable_identity_to_device_attributes) {
  const std::string first = ask("\033[c");
  CHECK_EQ(first, std::string("\033[?62;22c"));
  CHECK_EQ(ask("\033[0c"), first);
}

TEST(reply_answers_the_secondary_device_attributes) {
  CHECK_EQ(ask("\033[>c"), std::string("\033[>1;20;0c"));
}

// Un marqueur privé qu'on ne connaît pas n'est PAS une demande d'identité.
// Répondre à tort mettrait des octets dans le fil de quelqu'un qui
// n'attendait rien.
TEST(reply_says_nothing_to_a_private_marker_it_does_not_know) {
  CHECK_EQ(ask("\033[<c"), std::string(""));
}

// --------------------------------------------------------- la position

// Le fil compte à partir de UN, la grille à partir de zéro. Se tromper
// d'une unité place le curseur d'un `vim` une ligne trop haut.
TEST(reply_reports_the_cursor_position_one_indexed) {
  CHECK_EQ(ask("\033[6n", Modes{}, 0, 0), std::string("\033[1;1R"));
  CHECK_EQ(ask("\033[6n", Modes{}, 4, 9), std::string("\033[10;5R"));
}

TEST(reply_says_the_device_has_no_fault) {
  CHECK_EQ(ask("\033[5n"), std::string("\033[0n"));
}

TEST(reply_says_nothing_about_a_status_it_does_not_know) {
  CHECK_EQ(ask("\033[99n"), std::string(""));
}

// ------------------------------------------------------------- DECRQM

// `1` veut dire posé, `2` éteint. Le mode 25 naît allumé.
TEST(reply_reports_a_dec_mode_that_is_set) {
  CHECK_EQ(ask("\033[?25$p"), std::string("\033[?25;1$y"));
}

TEST(reply_reports_a_dec_mode_that_is_reset) {
  Modes m;
  m.set(25, false);
  CHECK_EQ(ask("\033[?25$p", m), std::string("\033[?25;2$y"));
}

// `0` veut dire NON RECONNU, ce qui n'est pas « éteint » : un invité qui
// lirait « éteint » pour un mode que nous n'avons pas croirait pouvoir
// l'allumer, et attendrait un effet qui ne viendra jamais.
TEST(reply_reports_zero_for_a_dec_mode_it_does_not_know) {
  CHECK_EQ(ask("\033[?9999$p"), std::string("\033[?9999;0$y"));
}

// Sans le marqueur privé, c'est un mode ANSI. Nous n'en gérons aucun, et
// la réponse doit le dire au lieu de se taire -- l'invité attend.
TEST(reply_does_not_recognize_an_ansi_mode) {
  CHECK_EQ(ask("\033[4$p"), std::string("\033[4;0$y"));
}

// ------------------------------------------------------- ce qui n'est rien

TEST(reply_says_nothing_about_a_sequence_that_is_not_a_query) {
  CHECK_EQ(ask("\033[1;4m"), std::string(""));
  CHECK_EQ(ask("\033[2J"), std::string(""));
  CHECK_EQ(ask("\033[?25h"), std::string(""));
}

// Un `$ p` sans paramètre ne désigne aucun mode : il n'y a rien à dire.
TEST(reply_says_nothing_about_a_mode_query_without_a_mode) {
  CHECK_EQ(ask("\033[?$p"), std::string(""));
}

// `CSI ? 6 n` est une AUTRE question que `CSI 6 n` : la position étendue,
// qui porte le numéro de page. Y répondre la forme ordinaire donnerait à
// l'invité un format qu'il n'attend pas ; ne pas y répondre le laisserait
// bloqué.
TEST(reply_answers_the_extended_cursor_position_with_its_page) {
  CHECK_EQ(ask("\033[?6n", Modes{}, 4, 9), std::string("\033[?10;5;1R"));
}

// Le contrat de lecture d'un mode : un mode inconnu se lit ÉTEINT. Sans
// cette garde, il se lirait comme le premier de la table -- c'est-à-dire
// n'importe quoi.
TEST(modes_read_an_unknown_mode_as_off) {
  Modes m;
  m.set(1, true);  // la première entrée de la table, posée
  CHECK(!m.get(9999));
  CHECK(!m.knows(9999));
}
