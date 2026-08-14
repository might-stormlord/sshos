#include <csignal>
#include <ctime>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "apps/terminal.hpp"
#include "harness.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"

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
  // Un jeton DIFFÉRENT par surveillance : avec des onglets, deux maîtres
  // sont surveillés en même temps, et un hôte qui rendrait toujours le
  // même jeton ferait livrer les octets du second au premier.
  uint64_t next_token = 1;
  uint64_t watch(int, uint32_t) override {
    ++watches;
    return next_token++;
  }
  std::vector<uint64_t> unwatched;
  void unwatch(uint64_t t) override { unwatched.push_back(t); }
  void watch_child(pid_t p) override { children.push_back(p); }
};

// La grille du terminal, ligne par ligne, telle qu'elle serait peinte. La
// BARRE D'ONGLETS n'en fait pas partie : les cas ci-dessous parlent de ce
// que l'invité écrit, et la fenêtre est donc toujours demandée une ligne
// plus haute que la grille dont ils parlent.
std::string painted(Terminal& t, int w, int h) {
  Surface s(w, h + 1);
  t.render(View(s, Rect{0, 0, w, h + 1}));
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = s.text_row(y + 1);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

// La barre d'onglets seule, sans ses blancs de queue.
std::string bar(Terminal& t, int w) {
  Surface s(w, 4);
  t.render(View(s, Rect{0, 0, w, 4}));
  std::string row = s.text_row(0);
  while (!row.empty() && row.back() == ' ') row.pop_back();
  return row;
}

// Les attributs d'une cellule de la barre -- c'est l'inverse vidéo qui dit
// quel onglet est actif, et aucun texte ne le dit.
uint16_t bar_attrs(Terminal& t, int w, int x) {
  Surface s(w, 4);
  t.render(View(s, Rect{0, 0, w, 4}));
  return s.at(x, 0).attrs;
}

// La colonne de la première croix de fermeture, ou -1 s'il n'y en a pas.
// La chercher par octet donnerait faux : « × » en pèse deux.
int cross_column(Terminal& t, int w) {
  Surface s(w, 4);
  t.render(View(s, Rect{0, 0, w, 4}));
  for (int x = 0; x < w; ++x) {
    if (s.at(x, 0).ch == U'\u00d7') return x;
  }
  return -1;
}

// Frappe une chaîne, caractère par caractère, comme le ferait un clavier.
void type(Terminal& t, std::string_view chars) {
  for (char c : chars) {
    t.on_key(KeyEvent{Key::Char, static_cast<char32_t>(c), 0});
  }
}

}  // namespace

// ------------------------------------------------------- la grille arrive

TEST(terminal_paints_what_the_guest_writes) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("bonjour");

  CHECK_EQ(painted(t, 10, 3), std::string("bonjour//"));
}

TEST(terminal_obeys_a_cursor_position_from_the_guest) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[2;3Hici");

  CHECK_EQ(painted(t, 10, 3), std::string("/  ici/"));
}

TEST(terminal_carries_the_colour_the_guest_asked_for) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[31mrouge");

  CHECK(t.screen_for_tests().at(0, 0).style.fg == sshos::Color::indexed(1));
}

// Les semi-graphiques DEC : un cadre de `mc` doit être des traits, pas des
// `qqqq`.
TEST(terminal_switches_to_the_dec_graphics_set) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033(0qqq\033(Bqqq");

  CHECK_EQ(painted(t, 10, 3), std::string("───qqq//"));
}

TEST(terminal_scrolls_and_feeds_its_history) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("une\r\ndeux\r\ntrois");

  CHECK_EQ(t.scrollback_for_tests().size(), size_t{1});
  CHECK_EQ(painted(t, 10, 2), std::string("deux/trois"));
}

// ------------------------------------------------------------- le clavier

TEST(terminal_sends_what_the_user_types) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.on_key(KeyEvent{Key::Char, U'a', 0});
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("a\r"));
}

// DECCKM change les flèches. Le liant doit lire le registre, pas décider
// tout seul : un `vim` en mode insertion en dépend.
TEST(terminal_follows_decckm_when_it_encodes_an_arrow) {
  Terminal t;
  t.on_resize(Size{10, 4});
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
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[2;5H\033[6n");

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[2;5R"));
}

TEST(terminal_answers_a_device_attributes_request) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[c");

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[?62;22c"));
}

// --------------------------------------------------------------- le titre

// Sans hôte -- avant `attach()` -- un titre ne remonte nulle part, et
// surtout ne plante pas.
TEST(terminal_survives_an_osc_title_before_it_is_attached) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033]2;mon titre\033\\");
  CHECK_EQ(painted(t, 10, 3), std::string("//"));
}

// ------------------------------------------------------------- le curseur

TEST(terminal_puts_the_cursor_where_the_grid_has_it) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[3;4H");

  Pos p{};
  CHECK(t.wants_cursor(p));
  CHECK_EQ(p.x, 3);
  // Une ligne plus bas que dans la grille : la barre d'onglets la prend.
  CHECK_EQ(p.y, 3);
}

// Le mode 25 le cache. Une application qui l'éteint ne veut pas d'un
// curseur qui clignote au milieu de son dessin.
TEST(terminal_hides_the_cursor_when_the_guest_asks) {
  Terminal t;
  t.on_resize(Size{10, 4});
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
  t.on_resize(Size{10, 4});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 2, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string(""));
}

// Coordonnées LOCALES et 1-indexées : l'invité croit occuper tout un
// terminal, et lui donner celles de l'écran ferait cliquer `htop` à côté.
TEST(terminal_reports_a_click_in_local_one_indexed_coordinates) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 2, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<0;3;2M"));
}

// Le relâchement se distingue par son `m` final : c'est TOUT l'intérêt de
// l'encodage 1006.
TEST(terminal_marks_a_release_with_a_lowercase_final) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Release, 0, 2, 2, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<0;3;2m"));
}

// 1000 ne rapporte pas le mouvement ; 1002 si. Le rapporter quand même
// noierait le lien SSH d'un paquet par cellule parcourue.
TEST(terminal_does_not_report_motion_under_click_tracking) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::Motion, 0, 2, 2, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string(""));

  t.feed_for_tests("\033[?1002h");
  t.on_mouse(MouseEvent{MouseAction::Motion, 0, 2, 2, 0});
  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<32;3;2M"));
}

// La molette fait défiler NOTRE historique tant que l'invité ne l'a pas
// demandée.
TEST(terminal_scrolls_its_own_history_with_the_wheel) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("un\r\ndeux\r\ntrois\r\nquatre");
  REQUIRE(t.scrollback_for_tests().size() >= size_t{2});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});
  CHECK(t.scrollback_for_tests().offset() > size_t{0});
  CHECK_EQ(t.take_written_for_tests(), std::string(""));
}

// Mais elle part à l'invité dès qu'il suit la souris : c'est ce qui fait
// défiler `less` sans que notre historique s'en mêle.
TEST(terminal_hands_the_wheel_to_a_guest_that_tracks_the_mouse) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[?1000;1006h");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 1, 2, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<64;2;2M"));
}

// Écrire, c'est revenir au présent : personne ne tape en aveugle dans une
// page d'historique.
TEST(terminal_comes_back_to_the_present_when_the_user_types) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("un\r\ndeux\r\ntrois");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});
  REQUIRE(t.scrollback_for_tests().offset() > size_t{0});

  t.on_key(KeyEvent{Key::Char, U'x', 0});
  CHECK_EQ(t.scrollback_for_tests().offset(), size_t{0});
}

// L'historique montre ce qu'on remonte : sans cela, la molette ne ferait
// rien de visible.
TEST(terminal_paints_the_history_it_scrolled_back_to) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("une\r\ndeux\r\ntrois");
  REQUIRE_EQ(t.scrollback_for_tests().size(), size_t{1});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});
  CHECK_EQ(painted(t, 10, 2), std::string("une/deux"));
}

// Remonté dans l'historique, le curseur n'est pas à l'écran : le montrer
// ailleurs qu'où il est serait pire que ne pas le montrer.
TEST(terminal_hides_the_cursor_while_looking_at_the_history) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("une\r\ndeux\r\ntrois");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});

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
  t.on_resize(Size{40, 5});
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
  t.on_resize(Size{40, 5});
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
  t.on_resize(Size{40, 5});
  t.attach(host);
  REQUIRE(wait_until_zombie(t.pid_for_tests(), 3000));
  t.on_child_exit(0);

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 2, 0});
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
  t.on_resize(Size{40, 5});
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
  t.on_resize(Size{40, 5});
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
  t.on_resize(Size{40, 5});
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
  t.on_resize(Size{10, 3});
  t.feed_for_tests("abc\033[H\033[P");

  CHECK_EQ(painted(t, 10, 2), std::string("bc/"));
}

// `CSI J` sans paramètre efface DU CURSEUR À LA FIN, pas tout l'écran. Se
// tromper de défaut efface la page d'un shell qui ne demandait que le bas.
TEST(terminal_erases_from_the_cursor_by_default) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("une\r\ndeux\033[2;3H\033[J");

  CHECK_EQ(painted(t, 10, 2), std::string("une/de"));
}

// Le stylo COURANT est le point de départ de chaque SGR : `\033[1m` puis
// `\033[31m` donne du rouge gras, pas du rouge seul.
TEST(terminal_accumulates_sgr_on_the_current_pen) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("\033[1m\033[31ma");

  const sshos::ScreenCell& c = t.screen_for_tests().at(0, 0);
  CHECK(c.style.fg == sshos::Color::indexed(1));
  CHECK_EQ(c.style.attrs, sshos::attr::Bold);
}

// Le mode 7 doit être RÉPERCUTÉ sur la grille : le registre seul ne fait
// rien avancer.
TEST(terminal_passes_autowrap_down_to_the_grid) {
  Terminal t;
  t.on_resize(Size{4, 3});
  t.feed_for_tests("\033[?7labcde");

  CHECK_EQ(painted(t, 4, 2), std::string("abce/"));
}

// Et l'écran alterné doit se QUITTER : un `vim` fermé rend le shell tel
// qu'on l'avait laissé.
TEST(terminal_comes_back_from_the_alternate_page) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("shell\033[?1049h\033[Hvim\033[?1049l");

  CHECK_EQ(painted(t, 10, 2), std::string("shell/"));
}

// `\0337` sauve, `\0338` rend. Les confondre ramène le curseur là où il
// n'a jamais été.
TEST(terminal_saves_and_restores_the_cursor_in_the_right_order) {
  Terminal t;
  t.on_resize(Size{10, 4});
  t.feed_for_tests("\033[1;1H\0337\033[3;5H\0338X");

  CHECK_EQ(painted(t, 10, 3), std::string("X//"));
}

// Les deux sens de la molette ne sont pas le même sens.
TEST(terminal_scrolls_the_history_both_ways) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("un\r\ndeux\r\ntrois\r\nquatre");
  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});
  REQUIRE(t.scrollback_for_tests().offset() > size_t{0});

  t.on_mouse(MouseEvent{MouseAction::WheelDown, 0, 0, 1, 0});
  CHECK_EQ(t.scrollback_for_tests().offset(), size_t{0});
}

// La seconde moitié d'une pleine chasse ne se peint PAS : elle n'a pas de
// glyphe à elle, et l'écrire quand même casserait la paire dans la
// surface -- le rendu afficherait un demi idéogramme.
TEST(terminal_does_not_paint_the_second_half_of_a_wide_character) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("一x");

  CHECK_EQ(painted(t, 10, 2), std::string("一x/"));
}

// L'historique se lit dans l'ORDRE. À l'envers, la ligne du haut passe en
// bas -- et la lecture déborde par le début.
TEST(terminal_paints_the_history_in_order) {
  Terminal t;
  t.on_resize(Size{10, 3});
  t.feed_for_tests("une\r\ndeux\r\ntrois\r\nquatre");
  REQUIRE_EQ(t.scrollback_for_tests().size(), size_t{2});

  t.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 0, 1, 0});
  REQUIRE_EQ(t.scrollback_for_tests().offset(), size_t{2});
  CHECK_EQ(painted(t, 10, 2), std::string("une/deux"));
}

// SEULS `OSC 0` et `OSC 2` posent le titre. Un `OSC 4` (palette) ou un
// `OSC 8` (lien) renommerait sinon la fenêtre avec sa charge utile -- et
// un shell en émet sans arrêt.
TEST(terminal_only_takes_its_title_from_osc_zero_and_two) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 5});
  t.attach(host);

  t.feed_for_tests("\033]2;le bon\033\\");
  REQUIRE_EQ(host.title, std::string("le bon"));

  t.feed_for_tests("\033]4;1;rgb:00/00/00\033\\");
  CHECK_EQ(host.title, std::string("le bon"));

  t.feed_for_tests("\033]0;aussi bon\033\\");
  CHECK_EQ(host.title, std::string("aussi bon"));
}

// --------------------------------------------------------------- onglets

// LA BARRE EST TOUJOURS LÀ, même avec un seul onglet. Elle coûte une ligne
// de grille, et c'est assumé : elle porte le `+`, qui est la SEULE voie à
// la souris vers un second onglet. Une barre qui n'apparaîtrait qu'au
// deuxième onglet demanderait de connaître le raccourci pour l'obtenir.
TEST(terminal_always_shows_a_tab_bar_with_a_new_tab_button) {
  Terminal t;
  t.on_resize(Size{30, 4});

  const std::string row = bar(t, 30);
  CHECK(row.find('1') != std::string::npos);
  CHECK_EQ(row.back(), '+');
}

// La grille commence SOUS la barre : sans ce décalage, la première ligne
// de l'invité serait écrasée par les onglets.
TEST(terminal_paints_its_grid_below_the_tab_bar) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.feed_for_tests("bonjour");

  Surface s(30, 4);
  t.render(View(s, Rect{0, 0, 30, 4}));
  CHECK(s.text_row(0).find("bonjour") == std::string::npos);
  CHECK(s.text_row(1).find("bonjour") != std::string::npos);
}

TEST(terminal_opens_a_tab_when_the_plus_is_clicked) {
  Terminal t;
  t.on_resize(Size{30, 4});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{1});

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});

  CHECK_EQ(t.tab_count_for_tests(), size_t{2});
  // Le nouvel onglet passe DEVANT : on ne l'ouvre pas pour continuer à
  // regarder l'ancien.
  CHECK_EQ(t.active_tab_for_tests(), size_t{1});
}

TEST(terminal_switches_tab_when_a_label_is_clicked) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});

  CHECK_EQ(t.active_tab_for_tests(), size_t{0});
}

// DEUX ONGLETS SONT DEUX TERMINAUX, pas deux vues du même : chacun a sa
// grille, et ce qu'on écrit dans l'un ne doit pas apparaître dans l'autre.
TEST(terminal_keeps_each_tab_grid_to_itself) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.feed_for_tests("premier");

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});
  CHECK(painted(t, 30, 3).find("premier") == std::string::npos);
  t.feed_for_tests("second");

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  const std::string back = painted(t, 30, 3);
  CHECK(back.find("premier") != std::string::npos);
  CHECK(back.find("second") == std::string::npos);
}

// L'onglet actif porte l'inverse vidéo. Sans elle, la barre dit combien
// d'onglets existent mais pas lequel on regarde.
TEST(terminal_marks_the_active_tab_in_the_bar) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});

  CHECK((bar_attrs(t, 30, 1) & sshos::attr::Reverse) == 0);
  const std::string row = bar(t, 30);
  const size_t second = row.find('2');
  REQUIRE(second != std::string::npos);
  CHECK((bar_attrs(t, 30, static_cast<int>(second)) & sshos::attr::Reverse) != 0);
}

TEST(terminal_closes_a_tab_when_its_cross_is_clicked) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{2});

  // La colonne, pas l'octet : la croix est un caractère de deux octets.
  const int cross = cross_column(t, 30);
  REQUIRE(cross >= 0);
  t.on_mouse(MouseEvent{MouseAction::Press, 0, cross, 0, 0});

  CHECK_EQ(t.tab_count_for_tests(), size_t{1});
}

// UN SEUL ONGLET N'A PAS DE CROIX : la fenêtre a déjà son `[×]`, et une
// croix d'onglet qui ferme la fenêtre entière serait un piège.
TEST(terminal_shows_no_cross_on_a_lone_tab) {
  Terminal t;
  t.on_resize(Size{30, 4});

  CHECK_EQ(cross_column(t, 30), -1);
}

// `F2` renomme, et le nom saisi remplace le numéro dans la barre.
TEST(terminal_renames_the_active_tab) {
  Terminal t;
  t.on_resize(Size{30, 4});

  t.on_key(KeyEvent{Key::F2, 0, 0});
  REQUIRE(t.mode_for_tests() == Terminal::Mode::Renaming);
  type(t, "build");
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK(t.mode_for_tests() == Terminal::Mode::Normal);
  CHECK_EQ(t.tab_label_for_tests(0), std::string("build"));
  CHECK(bar(t, 30).find("build") != std::string::npos);
}

// La saisie NE VA PAS À L'INVITÉ pendant le renommage. Sans cela, taper le
// nom lancerait la commande qu'on vient d'écrire dans le shell.
TEST(terminal_keeps_the_rename_keys_away_from_the_guest) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "ls");
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string());
}

TEST(terminal_gives_up_a_rename_on_escape) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "build");
  t.on_key(KeyEvent{Key::Escape, 0, 0});

  CHECK(t.mode_for_tests() == Terminal::Mode::Normal);
  CHECK_EQ(t.tab_label_for_tests(0), std::string("1"));
}

// Un nom VIDE rend l'onglet à son titre automatique : c'est ainsi qu'on
// défait un renommage sans avoir à deviner le nom d'origine.
TEST(terminal_hands_an_emptied_name_back_to_the_guest_title) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.feed_for_tests("\033]2;du shell\033\\");
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "moi");
  t.on_key(KeyEvent{Key::Enter, 0, 0});
  REQUIRE_EQ(t.tab_label_for_tests(0), std::string("moi"));

  t.on_key(KeyEvent{Key::F2, 0, 0});
  for (int i = 0; i < 3; ++i) t.on_key(KeyEvent{Key::Backspace, 0, 0});
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK_EQ(t.tab_label_for_tests(0), std::string("du shell"));
}

// LE NOM CHOISI SURVIT AU SHELL. Un `bash` repose son titre à chaque
// invite : sans cette règle, un onglet renommé reprendrait son nom
// d'origine à la première commande.
TEST(terminal_keeps_a_renamed_tab_when_the_guest_sets_a_title) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "build");
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  t.feed_for_tests("\033]2;~/dev\033\\");

  CHECK_EQ(t.tab_label_for_tests(0), std::string("build"));
}

TEST(terminal_names_an_unrenamed_tab_after_the_guest_title) {
  Terminal t;
  t.on_resize(Size{30, 4});
  t.feed_for_tests("\033]2;~/dev\033\\");

  CHECK_EQ(t.tab_label_for_tests(0), std::string("~/dev"));
}

// LE TITRE DE LA FENÊTRE SUIT L'ONGLET REGARDÉ. Sans cela, le cadre
// annoncerait encore ce que faisait l'onglet qu'on vient de quitter.
TEST(terminal_retitles_the_window_when_the_tab_changes) {
  FakeHost host;
  Terminal t;
  t.on_resize(Size{30, 4});
  t.attach(host);
  t.feed_for_tests("\033]2;premier\033\\");
  REQUIRE_EQ(host.title, std::string("premier"));

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});
  t.feed_for_tests("\033]2;second\033\\");
  REQUIRE_EQ(host.title, std::string("second"));

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  CHECK_EQ(host.title, std::string("premier"));
}

// FERMER LE DERNIER ONGLET FERME LA FENÊTRE : un terminal sans terminal
// dedans n'a rien à montrer.
TEST(terminal_closes_the_window_with_its_last_tab) {
  FakeHost host;
  Terminal t;
  t.on_resize(Size{30, 4});
  t.attach(host);
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});

  const int cross = cross_column(t, 30);
  REQUIRE(cross >= 0);
  t.on_mouse(MouseEvent{MouseAction::Press, 0, cross, 0, 0});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{1});
  CHECK_EQ(host.close_requests, 0);

  // Il ne reste plus de croix : c'est le `[×]` de la fenêtre qui prend le
  // relais, et lui passe par la confirmation habituelle.
  CHECK_EQ(cross_column(t, 30), -1);
}

// LA BARRE MANGE UNE LIGNE POUR TOUT LE MONDE, pas seulement pour l'onglet
// qu'on regarde : un `vim` laissé dans un onglet de fond peindrait sinon
// sa dernière ligne sous la fenêtre.
TEST(terminal_gives_every_tab_the_same_grid_height) {
  Terminal t;
  t.on_resize(Size{30, 8});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 29, 0, 0});

  CHECK_EQ(t.screen_for_tests().rows(), 7);
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  CHECK_EQ(t.screen_for_tests().rows(), 7);
}

// Les coordonnées envoyées à l'invité sont celles de SA grille : lui
// donner celles de la fenêtre ferait cliquer `htop` une ligne trop bas.
TEST(terminal_reports_a_click_below_the_bar_in_grid_coordinates) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.feed_for_tests("\033[?1000h");

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 4, 3, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string("\033[<0;5;3M"));
}

TEST(terminal_puts_the_cursor_below_the_bar) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.feed_for_tests("\033[2;3Hx");

  Pos p{};
  REQUIRE(t.wants_cursor(p));
  CHECK_EQ(p.y, 2);
}

// Un clic DANS LA BARRE ne descend jamais à l'invité : il pilote le
// bureau, pas le programme.
TEST(terminal_never_reports_a_bar_click_to_the_guest) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.feed_for_tests("\033[?1000h");

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});

  CHECK_EQ(t.take_written_for_tests(), std::string());
}

// Le clavier double la souris : `Alt` plutôt que `Ctrl`, qui appartient à
// l'invité tout entier.
TEST(terminal_opens_and_cycles_tabs_from_the_keyboard) {
  Terminal t;
  t.on_resize(Size{30, 5});

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{2});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});

  t.on_key(KeyEvent{Key::Left, 0, mod::Alt});
  CHECK_EQ(t.active_tab_for_tests(), size_t{0});
  // Le cycle BOUCLE : reculer depuis le premier ramène au dernier, sans
  // quoi la moitié des onglets seraient hors d'atteinte d'un seul geste.
  t.on_key(KeyEvent{Key::Left, 0, mod::Alt});
  CHECK_EQ(t.active_tab_for_tests(), size_t{1});
  t.on_key(KeyEvent{Key::Right, 0, mod::Alt});
  CHECK_EQ(t.active_tab_for_tests(), size_t{0});
}

TEST(terminal_closes_a_tab_from_the_keyboard) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{2});

  t.on_key(KeyEvent{Key::Char, U'w', mod::Alt});

  CHECK_EQ(t.tab_count_for_tests(), size_t{1});
}

// Les gestes d'onglet NE PARTENT PAS à l'invité : `Alt+t` est une
// transposition de mots dans readline, et la laisser passer en plus
// d'ouvrir un onglet ferait les deux.
TEST(terminal_keeps_the_tab_gestures_away_from_the_guest) {
  Terminal t;
  t.on_resize(Size{30, 5});

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.on_key(KeyEvent{Key::Left, 0, mod::Alt});

  CHECK_EQ(t.take_written_for_tests(), std::string());
}

// La barre TIENT DANS LA FENÊTRE. Un titre de shell fait couramment
// quarante colonnes : sans élision, il pousserait le `+` hors de l'écran
// et rendrait le nouvel onglet inatteignable à la souris.
TEST(terminal_elides_a_long_tab_title_to_keep_the_bar_inside) {
  Terminal t;
  t.on_resize(Size{20, 5});
  t.feed_for_tests("\033]2;user@machine:~/dev/ssh_os_2.0\033\\");

  const std::string row = bar(t, 20);
  CHECK(sshos::text_cells(row) <= 20);
  CHECK_EQ(row.back(), '+');
}

// Cliquer l'onglet DÉJÀ regardé le renomme : c'est la seule voie au
// renommage qui ne demande pas de connaître `F2`, et l'utilisateur pilote
// à la souris.
TEST(terminal_renames_the_active_tab_when_it_is_clicked_again) {
  Terminal t;
  t.on_resize(Size{30, 4});

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});

  CHECK(t.mode_for_tests() == Terminal::Mode::Renaming);
}

// Une saisie sans curseur a l'air d'une application figée : pendant le
// renommage, il est DANS LA BARRE, au bout de ce qu'on tape.
TEST(terminal_shows_the_cursor_in_the_bar_while_renaming) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "abc");

  // « espace 1 deux-points a b c espace » : le caret est sur le blanc de
  // queue, et la case s'élargit à chaque frappe.
  Pos p{};
  REQUIRE(t.wants_cursor(p));
  CHECK_EQ(p.y, 0);
  CHECK_EQ(p.x, 6);
  type(t, "d");
  REQUIRE(t.wants_cursor(p));
  CHECK_EQ(p.x, 7);
}

// `Alt+w` sur le DERNIER onglet ferme la fenêtre, et il passe par le [×]
// habituel : un `make` en cours doit faire poser la question, ici comme
// ailleurs. L'onglet reste debout tant que la réponse n'est pas venue.
TEST(terminal_asks_the_window_to_close_with_its_last_tab) {
  FakeHost host;
  Terminal t;
  t.on_resize(Size{30, 5});
  t.attach(host);

  t.on_key(KeyEvent{Key::Char, U'w', mod::Alt});

  CHECK_EQ(host.close_requests, 1);
  CHECK_EQ(t.tab_count_for_tests(), size_t{1});
}

// LES OCTETS VONT À L'ONGLET DONT ILS VIENNENT, pas à celui qu'on regarde.
// Deux parseurs appellent le même puits : sans le jeton pour les
// départager, un `ls` lancé dans le second onglet repeindrait le premier.
TEST(terminal_feeds_the_tab_the_bytes_came_from) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "printf coucou; read ignore"});
  t.on_resize(Size{40, 6});
  t.attach(host);
  const uint64_t first = host.next_token - 1;

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{2});
  const uint64_t second = host.next_token - 1;
  REQUIRE(second != first);

  // On revient sur le PREMIER onglet, et on ne draine que le maître du
  // second : ce qu'il écrit ne doit pas se voir ici.
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{0});
  for (int waited = 0; waited < 3000; waited += 10) {
    t.on_io(second, 0);
    nap_ms(10);
    if (t.tab_label_for_tests(1) != "2") break;
  }

  CHECK(painted(t, 40, 5).find("coucou") == std::string::npos);
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 5, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});
  CHECK(painted(t, 40, 5).find("coucou") != std::string::npos);
}

// UN ONGLET FERMÉ NE LAISSE PAS DE ZOMBIE. Son enfant meurt de la
// fermeture du maître, mais sa dépouille attend un waitpid() -- et
// l'onglet qui la portait n'est plus là pour le faire.
TEST(terminal_reaps_the_child_of_a_closed_tab) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 6});
  t.attach(host);

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  const pid_t doomed = t.pid_for_tests();
  REQUIRE(doomed > 0);

  t.on_key(KeyEvent{Key::Char, U'w', mod::Alt});
  REQUIRE_EQ(t.tab_count_for_tests(), size_t{1});

  // Le démon récolte sur SIGCHLD : le test reproduit cet ordre.
  for (int waited = 0; waited < 3000; waited += 10) {
    t.on_child_exit(0);
    if (::kill(doomed, 0) != 0) break;
    nap_ms(10);
  }
  CHECK(::kill(doomed, 0) != 0);
}

// --------------------------------- les onglets, seconde passe (mutations)

// UNE RÉPONSE APPARTIENT À L'ONGLET QUI A POSÉ LA QUESTION. `CSI 6 n`
// arrive pendant qu'on parse le flux d'un onglet de fond : envoyer sa
// réponse sur le maître de l'onglet regardé ferait apparaître « ;1R » au
// milieu de l'invite d'à côté.
TEST(terminal_answers_the_tab_that_asked_not_the_one_on_screen) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{0});

  t.feed_tab_for_tests(1, "\033[6n");

  CHECK_EQ(t.take_written_for_tests(size_t{0}), std::string());
  CHECK_EQ(t.take_written_for_tests(size_t{1}), std::string("\033[1;1R"));
}

// LE JALON EST REPRIS après la lecture : sans cela, la frappe suivante --
// qui n'appartient à aucun flux -- partirait au dernier onglet lu au lieu
// de celui qu'on regarde.
TEST(terminal_types_into_the_tab_on_screen_after_reading_another) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  t.feed_tab_for_tests(1, "rien");

  t.on_key(KeyEvent{Key::Char, U'x', 0});

  CHECK_EQ(t.take_written_for_tests(size_t{0}), std::string("x"));
  CHECK_EQ(t.take_written_for_tests(size_t{1}), std::string());
}

// CHAQUE ONGLET A SA CROIX. Sans l'avance d'une colonne après l'avoir
// posée, la case suivante la recouvre : la barre n'en montre plus qu'une,
// et le premier onglet devient infermable.
TEST(terminal_gives_every_tab_its_own_cross) {
  Terminal t;
  t.on_resize(Size{40, 5});
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});

  const std::vector<int> crosses = t.cross_columns_for_tests();
  REQUIRE_EQ(crosses.size(), size_t{2});
  Surface s(40, 5);
  t.render(View(s, Rect{0, 0, 40, 5}));
  for (int x : crosses) CHECK_EQ(s.at(x, 0).ch, U'×');
}

// LES CASES SE PARTAGENT LA LARGEUR. Sans ce partage, le premier onglet
// prend tout ce qu'il veut et les suivants tombent hors de la barre : on
// ne peut plus ni les voir ni les cliquer.
TEST(terminal_shares_the_bar_between_all_its_tabs) {
  Terminal t;
  t.on_resize(Size{40, 5});
  t.feed_for_tests("\033]2;un-titre-tres-long-de-shell\033\\");
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.feed_for_tests("\033]2;un-autre-titre-tres-long\033\\");
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.feed_for_tests("\033]2;un-troisieme-titre-tres-long\033\\");

  const std::string row = bar(t, 40);
  CHECK(row.find("1:") != std::string::npos);
  CHECK(row.find("2:") != std::string::npos);
  CHECK(row.find("3:") != std::string::npos);
  CHECK(sshos::text_cells(row) <= 40);
}

// L'ONGLET FERMÉ SORT DE L'EPOLL. Son descripteur part avec lui : le
// laisser surveillé ferait réveiller le démon sur un maître mort, en
// boucle et pour rien.
TEST(terminal_unwatches_the_master_of_a_closed_tab) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 6});
  t.attach(host);
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  const uint64_t second = host.next_token - 1;
  REQUIRE(host.unwatched.empty());

  t.on_key(KeyEvent{Key::Char, U'w', mod::Alt});

  REQUIRE_EQ(host.unwatched.size(), size_t{1});
  CHECK_EQ(host.unwatched[0], second);
}

// SEUL L'ONGLET REGARDÉ NOMME LA FENÊTRE : un `make` qui pose son titre
// dans un onglet de fond n'a pas à renommer ce qu'on a sous les yeux.
TEST(terminal_lets_no_background_tab_retitle_the_window) {
  FakeHost host;
  Terminal t;
  t.on_resize(Size{30, 5});
  t.attach(host);
  t.feed_for_tests("\033]2;devant\033\\");
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  REQUIRE_EQ(host.title, std::string("devant"));

  t.feed_tab_for_tests(1, "\033]2;derriere\033\\");

  CHECK_EQ(host.title, std::string("devant"));
  // Le nom l'attend quand même dans la barre.
  CHECK_EQ(t.tab_label_for_tests(1), std::string("derriere"));
}

// Le retour arrière retire un CARACTÈRE, pas un octet : couper une
// séquence UTF-8 en deux laisserait un demi-caractère dans le nom, et la
// grille rendrait un U+FFFD à sa place.
TEST(terminal_backspaces_a_whole_character_in_a_name) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::F2, 0, 0});
  t.on_key(KeyEvent{Key::Char, U'e', 0});
  t.on_key(KeyEvent{Key::Char, U'é', 0});
  t.on_key(KeyEvent{Key::Backspace, 0, 0});
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  CHECK_EQ(t.tab_label_for_tests(0), std::string("e"));
}

// Un geste d'onglet ne laisse RIEN passer, dans aucun onglet : `Alt+t`
// transpose deux mots dans readline, et le laisser filer en plus d'ouvrir
// un onglet ferait les deux.
TEST(terminal_leaks_no_tab_gesture_into_any_tab) {
  Terminal t;
  t.on_resize(Size{30, 5});

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  t.on_key(KeyEvent{Key::Left, 0, mod::Alt});
  t.on_key(KeyEvent{Key::Right, 0, mod::Alt});

  CHECK_EQ(t.take_written_for_tests(size_t{0}), std::string());
  CHECK_EQ(t.take_written_for_tests(size_t{1}), std::string());
}

// N'IMPORTE QUEL onglet vivant retient la fenêtre. Ne regarder que celui
// qu'on voit tuerait un `make` en cours dans un onglet de fond sans jamais
// poser la question.
TEST(terminal_asks_before_closing_a_live_tab_left_behind) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read ignore"});
  t.on_resize(Size{40, 6});
  t.attach(host);

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});
  const pid_t front = t.pid_for_tests();
  REQUIRE(front > 0);

  // Ctrl+D ferme l'entrée du `read` : l'onglet REGARDÉ meurt, celui de
  // derrière reste bien vivant.
  t.on_key(KeyEvent{Key::Char, U'd', mod::Ctrl});
  REQUIRE(wait_until_zombie(front, 3000));
  t.on_child_exit(0);

  const sshos::CloseCheck c = t.can_close();
  CHECK(!c.allowed);
}

// LE JALON DE LECTURE EST REPRIS. `on_io` pose l'onglet nourri avant de
// parser ; s'il ne le reprenait pas, la frappe suivante -- qui n'appartient
// à aucun flux -- partirait au dernier onglet LU au lieu de celui qu'on
// regarde, et l'invite ne répondrait plus à personne.
TEST(terminal_types_into_the_tab_on_screen_after_a_real_read) {
  FakeHost host;
  Terminal t({"/bin/sh", "-c", "read x; printf 'REPONSE-%s' \"$x\"; read y"});
  t.on_resize(Size{40, 6});
  t.attach(host);
  const uint64_t first = host.next_token - 1;

  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  const uint64_t second = host.next_token - 1;
  REQUIRE(second != first);

  // On draine le maître du SECOND onglet : c'est cette lecture qui pose le
  // jalon.
  for (int i = 0; i < 30; ++i) {
    t.on_io(second, 0);
    nap_ms(10);
  }

  // Puis on revient sur le premier et on tape : la réponse doit venir de LUI.
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{0});
  t.on_key(KeyEvent{Key::Char, U'z', 0});
  t.on_key(KeyEvent{Key::Enter, 0, 0});

  bool seen = false;
  for (int waited = 0; waited < 3000 && !seen; waited += 10) {
    t.on_io(first, 0);
    nap_ms(10);
    seen = painted(t, 40, 5).find("REPONSE-z") != std::string::npos;
  }
  CHECK(seen);
}

// CLIQUER AILLEURS VALIDE LE RENOMMAGE. Sans cela, le mode restait ouvert
// et le texte en cours -- qui se dessine sur l'onglet ACTIF -- suivait la
// sélection : le nom atterrissait sur l'onglet d'à côté, et celui qu'on
// venait de renommer revenait à son titre d'invité.
TEST(terminal_commits_a_rename_when_the_user_clicks_another_tab) {
  Terminal t;
  t.on_resize(Size{30, 5});
  t.on_key(KeyEvent{Key::Char, U't', mod::Alt});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});

  // Recliquer l'onglet regardé ouvre son renommage.
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 5, 0, 0});
  REQUIRE(t.mode_for_tests() == Terminal::Mode::Renaming);
  type(t, "ESSAI");

  // Puis on part sur l'autre onglet, SANS valider.
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 1, 0, 0});

  CHECK(t.mode_for_tests() == Terminal::Mode::Normal);
  CHECK_EQ(t.active_tab_for_tests(), size_t{0});
  CHECK_EQ(t.tab_label_for_tests(1), std::string("ESSAI"));
  CHECK_EQ(t.tab_label_for_tests(0), std::string("1"));

  // Et le nom TIENT quand on revient dessus puis qu'on clique ailleurs.
  // Une validation qui s'appliquerait HORS du mode reposerait une saisie
  // vide sur l'onglet regardé, et l'effacerait à chaque clic.
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 6, 0, 0});
  REQUIRE_EQ(t.active_tab_for_tests(), size_t{1});
  t.on_mouse(MouseEvent{MouseAction::Press, 0, 6, 2, 0});
  CHECK_EQ(t.tab_label_for_tests(1), std::string("ESSAI"));
}

// Un clic DANS LA GRILLE vaut aussi validation : on est passé à autre
// chose, et le nom qu'on venait de taper ne doit pas rester en suspens.
TEST(terminal_commits_a_rename_when_the_user_clicks_into_the_grid) {
  FakeHost host;
  Terminal t;
  t.on_resize(Size{30, 5});
  t.attach(host);
  t.on_key(KeyEvent{Key::F2, 0, 0});
  type(t, "build");

  t.on_mouse(MouseEvent{MouseAction::Press, 0, 4, 3, 0});

  CHECK(t.mode_for_tests() == Terminal::Mode::Normal);
  CHECK_EQ(t.tab_label_for_tests(0), std::string("build"));
  // ET LE CADRE SUIT : c'est lui qui disait encore le titre du shell
  // pendant toute la saisie.
  CHECK_EQ(host.title, std::string("build"));
}
