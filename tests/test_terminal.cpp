#include <ctime>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "apps/terminal.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Host;
using sshos::Key;
using sshos::KeyEvent;
using sshos::MouseAction;
using sshos::MouseEvent;
using sshos::Pos;
using sshos::Rect;
using sshos::Size;
using sshos::Surface;
using sshos::Terminal;
using sshos::View;
namespace mod = sshos::mod;

namespace {

// Hôte muet : le Terminal n'ouvre pas de PTY dans ces cas -- `attach()`
// n'est jamais appelée -- et tout ce qu'il écrirait à l'invité est retenu,
// donc lisible. C'est ce qui permet de vérifier le liant sans lancer un
// shell à chaque cas.
struct FakeHost : Host {
  std::string title;
  int close_requests = 0;
  int invalidations = 0;
  void set_title(std::string t) override { title = std::move(t); }
  void request_close() override { ++close_requests; }
  void invalidate() override { ++invalidations; }
  int watches = 0;
  std::vector<pid_t> children;
  uint64_t watch(int, uint32_t) override {
    ++watches;
    return 1;
  }
  void unwatch(uint64_t) override {}
  void watch_child(pid_t p) override { children.push_back(p); }
};

// La grille du terminal, ligne par ligne, telle qu'elle serait peinte.
std::string painted(Terminal& t, int w, int h) {
  Surface s(w, h);
  t.render(View(s, Rect{0, 0, w, h}));
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

// ------------------------------------------------------- la grille arrive

TEST(terminal_paints_what_the_guest_writes) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("bonjour");

  CHECK_EQ(painted(t, 10, 3), std::string("bonjour//"));
}

TEST(terminal_obeys_a_cursor_position_from_the_guest) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[2;3Hici");

  CHECK_EQ(painted(t, 10, 3), std::string("/  ici/"));
}

TEST(terminal_carries_the_colour_the_guest_asked_for) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[31mrouge");

  CHECK(t.screen_for_tests().at(0, 0).style.fg == sshos::Color::indexed(1));
}

// Les semi-graphiques DEC : un cadre de `mc` doit être des traits, pas des
// `qqqq`.
TEST(terminal_switches_to_the_dec_graphics_set) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033(0qqq\033(Bqqq");

  CHECK_EQ(painted(t, 10, 3), std::string("───qqq//"));
}

TEST(terminal_scrolls_and_feeds_its_history) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("une\r\ndeux\r\ntrois");

  CHECK_EQ(t.scrollback_for_tests().size(), size_t{1});
  CHECK_EQ(painted(t, 10, 2), std::string("deux/trois"));
}

// ------------------------------------------------------------- le clavier

TEST(terminal_sends_what_the_user_types) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.on_key(KeyEvent{Key::Char, U'a', 0});
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("a\r"));
}

// DECCKM change les flèches. Le liant doit lire le registre, pas décider
// tout seul : un `vim` en mode insertion en dépend.
TEST(terminal_follows_decckm_when_it_encodes_an_arrow) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.on_key(KeyEvent{Key::Up, 0, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string("\033[A"));

  t.feed_for_tests("\033[?1h");
  t.on_key(KeyEvent{Key::Up, 0, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string("\033OA"));
}

// ----------------------------------------------------- les questions posées

// Une réponse part sur le MAÎTRE, jamais vers le client : elle décrit
// notre émulateur, et s'intercaler dans le flux du client mettrait des
// octets au milieu des frappes de l'utilisateur.
TEST(terminal_answers_a_cursor_position_request_towards_the_guest) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[2;5H\033[6n");

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[2;5R"));
}

TEST(terminal_answers_a_device_attributes_request) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[c");

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[?62;22c"));
}

// --------------------------------------------------------------- le titre

// Sans hôte -- avant `attach()` -- un titre ne remonte nulle part, et
// surtout ne plante pas.
TEST(terminal_survives_an_osc_title_before_it_is_attached) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033]2;mon titre\033\\");
  CHECK_EQ(painted(t, 10, 3), std::string("//"));
}

// ------------------------------------------------------------- le curseur

TEST(terminal_puts_the_cursor_where_the_grid_has_it) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[3;4H");

  Pos p{};
  CHECK(t.wants_cursor(p));
  CHECK_EQ(p.x, 3);
  CHECK_EQ(p.y, 2);
}

// Le mode 25 le cache. Une application qui l'éteint ne veut pas d'un
// curseur qui clignote au milieu de son dessin.
TEST(terminal_hides_the_cursor_when_the_guest_asks) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[?25l");

  Pos p{};
  CHECK(!t.wants_cursor(p));
}

// --------------------------------------------------------------- la souris

// Rien ne part tant que l'invité n'a pas demandé la souris : lui envoyer
// des rapports qu'il n'attend pas lui ferait afficher des caractères
// parasites.
TEST(terminal_sends_no_mouse_report_until_the_guest_asks) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 1, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string(""));
}

// Coordonnées LOCALES et 1-indexées : l'invité croit occuper tout un
// terminal, et lui donner celles de l'écran ferait cliquer `htop` à côté.
TEST(terminal_reports_a_click_in_local_one_indexed_coordinates) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 1, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<0;3;2M"));
}

// Le relâchement se distingue par son `m` final : c'est TOUT l'intérêt de
// l'encodage 1006.
TEST(terminal_marks_a_release_with_a_lowercase_final) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Release, 0, 2, 1, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<0;3;2m"));
}

// 1000 ne rapporte pas le mouvement ; 1002 si. Le rapporter quand même
// noierait le lien SSH d'un paquet par cellule parcourue.
TEST(terminal_does_not_report_motion_under_click_tracking) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Motion, 0, 2, 1, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string(""));

  t.feed_for_tests("\033[?1002h");
  t.on_mouse(MouseEvent{MouseAction::Motion, 0, 2, 1, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<32;3;2M"));
}

// La molette fait défiler NOTRE historique tant que l'invité ne l'a pas
// demandée.
TEST(terminal_scrolls_its_own_history_with_the_wheel) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("un\r\ndeux\r\ntrois\r\nquatre");
  REQUIRE(t.scrollback_for_tests().size() >= size_t{2});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});
  CHECK(t.scrollback_for_tests().offset() > size_t{0});
  CHECK_EQ(t.take_written_for_tests(), std::string(""));
}

// Mais elle part à l'invité dès qu'il suit la souris : c'est ce qui fait
// défiler `less` sans que notre historique s'en mêle.
TEST(terminal_hands_the_wheel_to_a_guest_that_tracks_the_mouse) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 1, 1, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<64;2;2M"));
}

// Écrire, c'est revenir au présent : personne ne tape en aveugle dans une
// page d'historique.
TEST(terminal_comes_back_to_the_present_when_the_user_types) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("un\r\ndeux\r\ntrois");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});
  REQUIRE(t.scrollback_for_tests().offset() > size_t{0});

  t.on_key(KeyEvent{Key::Char, U'x', 0});
  CHECK_EQ(t.scrollback_for_tests().offset(), size_t{0});
}

// L'historique montre ce qu'on remonte : sans cela, la molette ne ferait
// rien de visible.
TEST(terminal_paints_the_history_it_scrolled_back_to) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("une\r\ndeux\r\ntrois");
  REQUIRE_EQ(t.scrollback_for_tests().size(), size_t{1});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});
  CHECK_EQ(painted(t, 10, 2), std::string("une/deux"));
}

// Remonté dans l'historique, le curseur n'est pas à l'écran : le montrer
// ailleurs qu'où il est serait pire que ne pas le montrer.
TEST(terminal_hides_the_cursor_while_looking_at_the_history) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("une\r\ndeux\r\ntrois");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});

  Pos p{};
  CHECK(!t.wants_cursor(p));
}

// ------------------------------------------------- la fin du processus

namespace {

void nap_ms(int ms) {
  timespec ts{ms / 1000, static_cast<long>(ms % 1000) * 1000000L};
  ::nanosleep(&ts, nullptr);
}

// Attend que l'enfant soit mort SANS le récolter : c'est le démon qui
// récolte, et le test doit reproduire cet ordre.
bool wait_until_zombie(pid_t pid, int budget_ms) {
  for (int waited = 0; waited <= budget_ms; waited += 5) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    FILE* f = ::fopen(path.c_str(), "re");
    if (f == nullptr) return false;
    char buf[512] = {0};
    const size_t n = ::fread(buf, 1, sizeof buf - 1, f);
    ::fclose(f);
    const std::string line(buf, n);
    const size_t close = line.rfind(')');
    if (close != std::string::npos && close + 2 < line.size() &&
        line[close + 2] == 'Z') {
      return true;
    }
    nap_ms(5);
  }
  return false;
}

}  // namespace

// La fenêtre RESTE ouverte : on doit pouvoir lire la dernière erreur d'une
// commande qui vient d'échouer. Une fenêtre qui se referme emporte
// justement ce qu'on avait besoin de voir.
TEST(terminal_stays_open_and_says_that_the_process_ended) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "exit 3"});
  t.on_resize(Size{40, 4});
  t.attach(host);
  REQUIRE(t.pid_for_tests() > 0);
  REQUIRE(wait_until_zombie(t.pid_for_tests(), 3000));

  t.on_child_exit(0);
  CHECK_EQ(host.close_requests, 0);
  const std::string screen = painted(t, 40, 4);
  CHECK(screen.find("processus termine") != std::string::npos);
  CHECK(screen.find("code 3") != std::string::npos);
}

TEST(terminal_closes_on_enter_once_the_process_is_dead) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "exit 0"});
  t.on_resize(Size{40, 4});
  t.attach(host);
  REQUIRE(wait_until_zombie(t.pid_for_tests(), 3000));
  t.on_child_exit(0);

  t.on_key(KeyEvent{Key::Enter, 0, 0});
  CHECK_EQ(host.close_requests, 1);
}

// Un CLIC ferme aussi : une fonction qui n'a qu'un raccourci clavier est
// une fonction incomplète.
TEST(terminal_closes_on_a_click_once_the_process_is_dead) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "exit 0"});
  t.on_resize(Size{40, 4});
  t.attach(host);
  REQUIRE(wait_until_zombie(t.pid_for_tests(), 3000));
  t.on_child_exit(0);

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 1, 0});
  CHECK_EQ(host.close_requests, 1);
}

// Un processus VIVANT fait poser la question : fermer la fenêtre tue le
// groupe, et une compilation en cours ne doit pas partir sur un clic.
TEST(terminal_asks_before_closing_a_live_process) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 4});
  t.attach(host);

  const sshos::CloseCheck c = t.can_close();
  CHECK(!c.allowed);
  CHECK(!c.question.empty());
}

// L'application confie son maître à l'hôte ET lui dit à qui appartient le
// pid : sans le second, la mort de l'enfant n'aurait aucun destinataire.
TEST(terminal_hands_its_master_and_its_child_to_the_host) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 4});
  t.attach(host);

  CHECK_EQ(host.watches, 1);
  REQUIRE_EQ(host.children.size(), size_t{1});
  CHECK_EQ(host.children[0], t.pid_for_tests());
}

// `OSC 2` pose le titre de la fenêtre : c'est ainsi qu'un shell annonce le
// répertoire courant, et qu'un `ssh` annonce la machine distante.
TEST(terminal_takes_its_title_from_osc_two_when_attached) {
  // L'hôte est déclaré EN PREMIER : il doit survivre à l'application,
  // dont le destructeur retire sa surveillance. C'est l'ordre que
  // `Window` impose en production, et le violer ici appelle une
  // méthode virtuelle pure sur un hôte déjà détruit.
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 4});
  t.attach(host);

  t.feed_for_tests("\033]2;mon titre\033\\");
  CHECK_EQ(host.title, std::string("mon titre"));
}

// ------------------------------------------- dix trous montrés par les mutations

// Un paramètre absent vaut UN, pas zéro. La différence ne se voit pas sur
// les déplacements -- la grille borne déjà -- mais elle se voit sur les
// éditions : `CSI P` sans paramètre doit supprimer un caractère.
TEST(terminal_treats_a_missing_count_as_one) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("abc\033[H\033[P");

  CHECK_EQ(painted(t, 10, 2), std::string("bc/"));
}

// `CSI J` sans paramètre efface DU CURSEUR À LA FIN, pas tout l'écran. Se
// tromper de défaut efface la page d'un shell qui ne demandait que le bas.
TEST(terminal_erases_from_the_cursor_by_default) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("une\r\ndeux\033[2;3H\033[J");

  CHECK_EQ(painted(t, 10, 2), std::string("une/de"));
}

// Le stylo COURANT est le point de départ de chaque SGR : `\033[1m` puis
// `\033[31m` donne du rouge gras, pas du rouge seul.
TEST(terminal_accumulates_sgr_on_the_current_pen) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("\033[1m\033[31ma");

  const sshos::ScreenCell& c = t.screen_for_tests().at(0, 0);
  CHECK(c.style.fg == sshos::Color::indexed(1));
  CHECK_EQ(c.style.attrs, sshos::attr::Bold);
}

// Le mode 7 doit être RÉPERCUTÉ sur la grille : le registre seul ne fait
// rien avancer.
TEST(terminal_passes_autowrap_down_to_the_grid) {
  Terminal t;
  t.on_resize(Size{4, 2});
  t.feed_for_tests("\033[?7labcde");

  CHECK_EQ(painted(t, 4, 2), std::string("abce/"));
}

// Et l'écran alterné doit se QUITTER : un `vim` fermé rend le shell tel
// qu'on l'avait laissé.
TEST(terminal_comes_back_from_the_alternate_page) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("shell\033[?1049h\033[Hvim\033[?1049l");

  CHECK_EQ(painted(t, 10, 2), std::string("shell/"));
}

// `\0337` sauve, `\0338` rend. Les confondre ramène le curseur là où il
// n'a jamais été.
TEST(terminal_saves_and_restores_the_cursor_in_the_right_order) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[1;1H\0337\033[3;5H\0338X");

  CHECK_EQ(painted(t, 10, 3), std::string("X//"));
}

// Les deux sens de la molette ne sont pas le même sens.
TEST(terminal_scrolls_the_history_both_ways) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("un\r\ndeux\r\ntrois\r\nquatre");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});
  REQUIRE(t.scrollback_for_tests().offset() > size_t{0});

  t.on_mouse(MouseEvent{MouseAction::WheelDown, 0, 0, 0, 0});
  CHECK_EQ(t.scrollback_for_tests().offset(), size_t{0});
}

// La seconde moitié d'une pleine chasse ne se peint PAS : elle n'a pas de
// glyphe à elle, et l'écrire quand même casserait la paire dans la
// surface -- le rendu afficherait un demi idéogramme.
TEST(terminal_does_not_paint_the_second_half_of_a_wide_character) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("一x");

  CHECK_EQ(painted(t, 10, 2), std::string("一x/"));
}

// L'historique se lit dans l'ORDRE. À l'envers, la ligne du haut passe en
// bas -- et la lecture déborde par le début.
TEST(terminal_paints_the_history_in_order) {
  Terminal t;
  t.on_resize(Size{10, 2});
  t.feed_for_tests("une\r\ndeux\r\ntrois\r\nquatre");
  REQUIRE_EQ(t.scrollback_for_tests().size(), size_t{2});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 0, 0});
  REQUIRE_EQ(t.scrollback_for_tests().offset(), size_t{2});
  CHECK_EQ(painted(t, 10, 2), std::string("une/deux"));
}
