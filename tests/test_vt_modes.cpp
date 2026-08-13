#include <string>
#include <string_view>

#include "harness.hpp"
#include "vt/modes.hpp"
#include "vt/parser.hpp"
#include "vt/screen.hpp"

using sshos::Modes;
using sshos::MouseTracking;
using sshos::Params;
using sshos::Parser;
using sshos::ParserSink;
using sshos::Screen;
using sshos::apply_dec_private;

namespace {

// On passe par le VRAI parseur au lieu de fabriquer des `Params` à la
// main : c'est lui qui décide où tombe le marqueur privé `?`, et un test
// qui s'en remettrait à ma lecture de la spec ne mesurerait que ma lecture.
void feed(Modes& modes, const std::string& seq) {
  struct Sink : ParserSink {
    Modes* modes = nullptr;
    void csi(const Params& p, std::string_view intermediates,
             uint8_t final_byte) override {
      if (intermediates == "?" && (final_byte == 'h' || final_byte == 'l')) {
        apply_dec_private(p, final_byte == 'h', *modes);
      }
    }
  } sink;
  sink.modes = &modes;
  Parser parser(sink);
  parser.feed(seq);
}

// La correspondance code → drapeau, RÉÉCRITE ici exprès. Un test qui
// appellerait `Modes::set` pour se relire ne vérifierait que sa propre
// cohérence ; celui-ci dit la table une seconde fois, et une inversion de
// deux modes se voit.
bool flag_of(const Modes& m, int code) {
  switch (code) {
    case 1: return m.cursor_keys_application;
    case 7: return m.autowrap;
    case 25: return m.cursor_visible;
    case 1000: return m.mouse_click;
    case 1002: return m.mouse_drag;
    case 1003: return m.mouse_any;
    case 1006: return m.mouse_sgr;
    case 1049: return m.alt_screen;
    case 2004: return m.bracketed_paste;
    default: return false;
  }
}

constexpr int kAllModes[] = {1, 7, 25, 1000, 1002, 1003, 1006, 1049, 2004};

constexpr char32_t kWide = U'一';

void puts_ascii(Screen& s, const std::string& text) {
  for (char c : text) s.print(static_cast<char32_t>(c));
}

}  // namespace

// ------------------------------------------------------------ le registre

// Les défauts sont ceux d'un terminal qui vient de s'allumer. Se tromper
// ici ne se voit pas tout de suite : c'est au premier invité qui ne pose
// PAS le mode 7 que la page se mettrait à sauter une ligne sur deux.
TEST(modes_start_as_a_terminal_that_just_woke_up) {
  const Modes m;
  CHECK(m.autowrap);
  CHECK(m.cursor_visible);
  CHECK(!m.cursor_keys_application);
  CHECK(!m.mouse_click);
  CHECK(!m.mouse_drag);
  CHECK(!m.mouse_any);
  CHECK(!m.mouse_sgr);
  CHECK(!m.alt_screen);
  CHECK(!m.bracketed_paste);
  CHECK(m.tracking() == MouseTracking::None);
}

TEST(modes_set_every_mode_they_know) {
  for (int code : kAllModes) {
    Modes m;
    feed(m, "\033[?" + std::to_string(code) + "h");
    CHECK(flag_of(m, code));
  }
}

TEST(modes_reset_every_mode_they_know) {
  for (int code : kAllModes) {
    Modes m;
    feed(m, "\033[?" + std::to_string(code) + "h");
    feed(m, "\033[?" + std::to_string(code) + "l");
    CHECK(!flag_of(m, code));
  }
}

// Poser un mode ne doit toucher QUE le sien. Une table mal écrite est
// invisible tant qu'on ne pose qu'un mode à la fois.
TEST(modes_leave_every_other_mode_alone) {
  for (int code : kAllModes) {
    Modes m;
    feed(m, "\033[?" + std::to_string(code) + "h");
    for (int other : kAllModes) {
      if (other == code) continue;
      Modes fresh;
      CHECK_EQ(flag_of(m, other), flag_of(fresh, other));
    }
  }
}

// La façon normale d'allumer la souris est une seule séquence à trois
// modes. Les traiter un par un est ce qui distingue un registre d'une
// table de correspondance.
TEST(modes_apply_every_mode_of_one_sequence) {
  Modes m;
  feed(m, "\033[?1000;1002;1006h");

  CHECK(m.mouse_click);
  CHECK(m.mouse_drag);
  CHECK(m.mouse_sgr);
}

// Dans LES DEUX SENS. Ne l'essayer qu'en `h` ne prouve rien : un mode
// inconnu qui irait poser un drapeau déjà actif -- le curseur visible, le
// retour automatique -- ne se verrait pas. C'est le `l` qui mord.
TEST(modes_ignore_one_they_do_not_know) {
  Modes m;
  feed(m, "\033[?9999h");
  CHECK(m == Modes{});

  feed(m, "\033[?9999l");
  CHECK(m == Modes{});
}

// Un mode inconnu au MILIEU d'une liste ne doit pas emporter ses voisins :
// c'est le cas qui distingue « on saute celui-là » de « on abandonne la
// séquence ».
TEST(modes_keep_reading_past_a_mode_they_do_not_know) {
  Modes m;
  feed(m, "\033[?1000;9999;1006h");

  CHECK(m.mouse_click);
  CHECK(m.mouse_sgr);
}

// `\033[?h` n'a pas de paramètre, et un paramètre absent vaut -1, pas 0 :
// aucun mode ne porte ces numéros, il ne se passe donc rien.
TEST(modes_do_nothing_without_a_parameter) {
  Modes m;
  feed(m, "\033[?h");
  CHECK(m == Modes{});
}

// ------------------------------------------------------------- la souris

// Le liant n'a pas besoin des trois drapeaux, il a besoin du niveau. Le
// plus permissif gagne : une application qui pose 1002 PUIS 1003 attend
// bien qu'on lui rapporte le mouvement à vide.
TEST(modes_report_the_most_permissive_mouse_tracking) {
  Modes m;
  CHECK(m.tracking() == MouseTracking::None);

  feed(m, "\033[?1000h");
  CHECK(m.tracking() == MouseTracking::Click);

  feed(m, "\033[?1002h");
  CHECK(m.tracking() == MouseTracking::Drag);

  feed(m, "\033[?1003h");
  CHECK(m.tracking() == MouseTracking::Any);
}

// Les trois modes sont INDÉPENDANTS : éteindre le plus permissif rend le
// niveau à celui d'en dessous, il ne coupe pas la souris.
TEST(modes_fall_back_to_the_lower_tracking_when_the_top_one_goes_off) {
  Modes m;
  feed(m, "\033[?1000;1002;1003h");
  REQUIRE(m.tracking() == MouseTracking::Any);

  feed(m, "\033[?1003l");
  CHECK(m.tracking() == MouseTracking::Drag);

  feed(m, "\033[?1002l");
  CHECK(m.tracking() == MouseTracking::Click);

  feed(m, "\033[?1000l");
  CHECK(m.tracking() == MouseTracking::None);
}

// 1006 ne rapporte RIEN tout seul : il ne choisit que la façon d'écrire ce
// que 1000/1002/1003 rapportent. Le confondre avec un mode de suivi ferait
// croire le liant à une souris active.
TEST(modes_do_not_take_the_sgr_encoding_for_a_tracking_mode) {
  Modes m;
  feed(m, "\033[?1006h");

  CHECK(m.mouse_sgr);
  CHECK(m.tracking() == MouseTracking::None);
}

// ------------------------------------------------- le retour automatique

// Mode 7 éteint : la dernière colonne s'ÉCRASE au lieu de descendre. Une
// application qui trace un cadre jusqu'au bord droit compte là-dessus pour
// ne pas faire défiler sa page.
TEST(screen_overwrites_the_last_column_when_autowrap_is_off) {
  Screen s(4, 3);
  s.set_autowrap(false);

  puts_ascii(s, "abcde");
  CHECK_EQ(s.cursor().y, 0);
  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.line_text(0), std::string("abce"));
  CHECK_EQ(s.line_text(1), std::string(""));
}

TEST(screen_wraps_again_when_autowrap_comes_back) {
  Screen s(4, 3);
  s.set_autowrap(false);
  puts_ascii(s, "abcd");

  s.set_autowrap(true);
  puts_ascii(s, "e");

  CHECK_EQ(s.cursor().y, 1);
  CHECK_EQ(s.line_text(1), std::string("e"));
}

// Une pleine chasse qui ne tient plus ne peut ni se couper en deux ni
// descendre : elle n'est pas écrite. Elle ne doit surtout pas écraser la
// dernière colonne d'une moitié orpheline.
TEST(screen_refuses_a_wide_character_that_cannot_fit_when_autowrap_is_off) {
  Screen s(4, 3);
  s.set_autowrap(false);
  puts_ascii(s, "abcd");  // le curseur reste en 3

  s.print(kWide);

  CHECK_EQ(s.cursor().y, 0);
  CHECK_EQ(s.line_text(0), std::string("abcd"));
  CHECK_EQ(s.at(3, 0).width, 1);
}

// --------------------------------------------------------- l'écran alterné

// LE point du mode 1049 : la page principale revient au caractère près.
// C'est ce qui fait qu'un `vim` quitté rend le shell tel qu'on l'a laissé.
TEST(screen_gives_the_main_page_back_character_for_character) {
  Screen s(6, 3);
  puts_ascii(s, "shell");
  s.move_to(0, 1);
  puts_ascii(s, "ligne");

  s.enter_alt_screen();
  puts_ascii(s, "vim");
  s.move_to(0, 2);
  puts_ascii(s, "plus");

  s.leave_alt_screen();
  CHECK_EQ(s.line_text(0), std::string("shell"));
  CHECK_EQ(s.line_text(1), std::string("ligne"));
  CHECK_EQ(s.line_text(2), std::string(""));
}

TEST(screen_starts_the_alternate_page_blank) {
  Screen s(6, 3);
  puts_ascii(s, "shell");

  s.enter_alt_screen();
  CHECK_EQ(s.line_text(0), std::string(""));
}

// Entrer sauve le curseur ET le style, comme un DECSC -- et sortir rend
// les deux. Une application qui bascule au milieu d'un passage en gras le
// retrouve en gras.
TEST(screen_restores_the_cursor_and_the_pen_it_had_before_the_alternate) {
  Screen s(6, 3);
  sshos::Style pen;
  pen.fg = sshos::Color::indexed(2);
  pen.attrs = sshos::attr::Bold;
  s.set_pen(pen);
  s.move_to(3, 1);

  s.enter_alt_screen();
  s.move_to(0, 2);
  s.set_pen(sshos::Style{});

  s.leave_alt_screen();
  CHECK_EQ(s.cursor().x, 3);
  CHECK_EQ(s.cursor().y, 1);
  CHECK(s.pen() == pen);
}

// L'emplacement de sauvegarde de 1049 est SÉPARÉ de celui de DECSC : un
// `vim` qui sauve son curseur dans l'écran alterné ne doit pas écraser
// celui qui attend dehors.
TEST(screen_keeps_the_alternate_decsc_away_from_the_main_one) {
  Screen s(6, 3);
  s.move_to(1, 1);
  s.save_cursor();

  s.enter_alt_screen();
  s.move_to(4, 2);
  s.save_cursor();  // le DECSC de l'invité, dans l'écran alterné
  s.leave_alt_screen();

  s.move_to(0, 0);
  s.restore_cursor();
  CHECK_EQ(s.cursor().x, 1);
  CHECK_EQ(s.cursor().y, 1);
}

// Un second 1049 alors qu'on y est déjà ne doit PAS ranger l'écran
// alterné par-dessus la page principale : elle serait perdue pour de bon.
// Un `tmux` imbriqué le fait.
TEST(screen_keeps_the_main_page_under_a_second_entry_into_the_alternate) {
  Screen s(6, 3);
  puts_ascii(s, "shell");

  s.enter_alt_screen();
  puts_ascii(s, "vim");
  s.enter_alt_screen();  // déjà dedans

  s.leave_alt_screen();
  CHECK_EQ(s.line_text(0), std::string("shell"));
}

TEST(screen_ignores_a_leave_without_an_enter) {
  Screen s(6, 3);
  puts_ascii(s, "shell");

  s.leave_alt_screen();
  CHECK_EQ(s.line_text(0), std::string("shell"));
  CHECK(!s.alt_screen());
}

// Le scrollback (tâche 7) ne doit rien recevoir de l'écran alterné : il
// lira ce drapeau pour le savoir.
TEST(screen_says_when_the_alternate_page_is_active) {
  Screen s(6, 3);
  CHECK(!s.alt_screen());

  s.enter_alt_screen();
  CHECK(s.alt_screen());

  s.leave_alt_screen();
  CHECK(!s.alt_screen());
}

// La page alternée démarre aussi avec un DECSC VIERGE : un `\0338` reçu
// avant tout `\0337` y ramène à l'origine, et surtout pas à la position
// que le shell avait sauvée dehors.
TEST(screen_starts_the_alternate_page_with_a_blank_decsc) {
  Screen s(6, 3);
  s.move_to(2, 1);
  s.save_cursor();

  s.enter_alt_screen();
  s.move_to(4, 2);
  s.restore_cursor();

  CHECK_EQ(s.cursor().x, 0);
  CHECK_EQ(s.cursor().y, 0);
}
