#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fake_apps.hpp"
#include "client/client.hpp"
#include "common/fd.hpp"
#include "common/net.hpp"
#include "common/platform.hpp"
#include "common/proto.hpp"
#include "daemon/daemon.hpp"
#include "daemon/session.hpp"
#include "harness.hpp"
#include "input/events.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"

using sshos::Session;
using sshos::Surface;

namespace {

// Registrar nul partagé par tous les cas unitaires : ils n'ont pas d'epoll
// et n'en veulent pas. Une seule instance suffit parce que l'objet est sans
// état -- il ne fait rien, c'est tout son rôle. Les cas bout-en-bout, eux,
// passent par le vrai registrar du démon.
sshos::NullFdRegistrar g_fds;

// Horloge figée : sans elle le harnais n'est pas déterministe par
// construction, et un test d'affichage d'heure est ininspectable.
struct FakePlatform : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    // 2026-08-10 14:05:00 UTC
    return std::chrono::system_clock::time_point(std::chrono::seconds(1786370700));
  }
  // Horloge monotone réglable : le chien de garde du glissement se teste en
  // avançant le temps d'un cran, pas en dormant deux secondes.
  std::chrono::steady_clock::time_point steady_now() const override {
    return steady;
  }
  void advance_steady(std::chrono::milliseconds d) { steady += d; }
  std::string read_file(std::string_view) const override { return {}; }

  std::chrono::steady_clock::time_point steady{};
};

// Horloge murale QUI AVANCE, contrairement aux deux doubles figés
// ci-dessous : la seule façon de faire tourner une minute sans dormir
// soixante secondes.
struct MovingPlatform : sshos::Platform {
  explicit MovingPlatform(std::int64_t epoch_seconds) : t_(epoch_seconds) {}
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(t_));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point{};
  }
  std::string read_file(std::string_view) const override { return {}; }
  void advance(std::int64_t seconds) { t_ += seconds; }

 private:
  std::int64_t t_;
};

// Même rôle que FakePlatform ci-dessus, mais avec un instant configurable au
// lieu d'un seul figé en dur -- nécessaire pour l'item 4 (round horloge, voir
// clock-round-brief.md), qui doit comparer le rendu à DEUX instants distincts
// sous le même fuseau.
struct FakePlatformAt : sshos::Platform {
  explicit FakePlatformAt(std::int64_t epoch_seconds) : t_(epoch_seconds) {}
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(t_));
  }
  // Point fixe : aucun test de ce double ne touche au glissement.
  std::chrono::steady_clock::time_point steady_now() const override {
    return {};
  }
  std::string read_file(std::string_view) const override { return {}; }

 private:
  std::int64_t t_;
};

// Fixe TZ pour la durée du test puis restaure exactement l'état antérieur --
// même raison d'être que UnlinkGuard/FifoReleaseGuard dans
// tests/test_daemonize.cpp : la suite entière s'exécute dans un seul
// processus ouvrier (voir tests/main.cpp, un seul fork() pour tout le lot),
// donc TZ est un état GLOBAL partagé avec tous les cas restants -- un garde
// qui ne restaure pas correctement contaminerait silencieusement toute la
// suite après lui.
//
// setenv("TZ", "", 1) ne reproduit PAS l'absence de TZ : glibc distingue "TZ
// vide" (UTC) de "TZ absent" (consultation de /etc/localtime). Le cas "non
// défini au départ" est donc mémorisé à part et restauré par unsetenv(),
// jamais par un setenv() avec une chaîne vide.
//
// tzset() après chaque changement de TZ (construction ET destruction) n'est
// pas cosmétique : la glibc ne relit TZ qu'à la première utilisation de
// localtime_r/mktime (vérifié empiriquement -- voir rapport de tâche), donc
// sans ce tzset() explicite ici, ce garde changerait TZ dans l'environnement
// sans que ::localtime_r() ne le voie jamais -- un résultat faux mais
// crédible, exactement le piège documenté dans clock-round-brief.md.
class TzGuard {
 public:
  explicit TzGuard(const char* zone) {
    const char* prev = std::getenv("TZ");
    had_tz_ = prev != nullptr;
    if (had_tz_) prev_value_ = prev;
    ::setenv("TZ", zone, 1);
    ::tzset();
  }

  ~TzGuard() {
    if (had_tz_) {
      ::setenv("TZ", prev_value_.c_str(), 1);
    } else {
      ::unsetenv("TZ");
    }
    ::tzset();
  }

  TzGuard(const TzGuard&) = delete;
  TzGuard& operator=(const TzGuard&) = delete;

 private:
  bool had_tz_;
  std::string prev_value_;
};

}  // namespace

TEST(surface_text_row_reads_back_what_was_written) {
  Surface s(6, 1);
  s.root().text(0, 0, "\xe6\x97\xa5" "ab", sshos::Style{});  // 日ab
  CHECK_EQ(s.text_row(0), std::string("\xe6\x97\xa5" "ab  "));
}

// Round horloge (clock-round-brief.md), items 2 et 3. Ce test affirmait
// autrefois panel.find("14:05") -- le rendu UTC de l'instant figé par
// FakePlatform, stable uniquement parce que le code appelait ::gmtime_r et
// que cette machine est réglée sur Etc/UTC. Dès que le code passe à
// ::localtime_r, une assertion qui dépend du fuseau de la MACHINE est un
// défaut de portabilité latent : elle continuerait de passer ici et
// échouerait ailleurs. TzGuard fixe donc explicitement le fuseau au lieu de
// dépendre de celui de la machine, avec restauration garantie en sortie.
//
// C'est aussi, sans modification supplémentaire, le test discriminant exigé
// par l'item 3 : avec TZ=America/Toronto, "10:05" (rendu local, EDT en
// août) ne peut sortir que de ::localtime_r ; la seconde assertion exclut
// explicitement "14:05" (le rendu qu'aurait produit l'ancien ::gmtime_r),
// donc ce test échoue contre le code d'avant ce round et passe contre le
// code corrigé. Voir le rapport de tâche pour la sortie d'échec exacte
// obtenue en remettant ::gmtime_r en place.
TEST(session_draws_a_panel_on_the_last_row) {
  TzGuard tz("America/Toronto");

  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);
  const std::string panel = s.text_row(11);
  // 1786370700 = 2026-08-10 14:05:00 UTC = 10:05:00 EDT à Toronto (août,
  // heure d'été active).
  CHECK(panel.find("10:05") != std::string::npos);
  CHECK(panel.find("14:05") == std::string::npos);
  CHECK(panel.find("ssh_os") != std::string::npos);
}

// Round horloge (clock-round-brief.md), item 4. Sous UN SEUL fuseau
// (America/Toronto), un instant d'hiver (EST, UTC-5) et un instant d'été
// (EDT, UTC-4) doivent produire des écarts différents entre heure rendue et
// heure UTC. C'est précisément ce qui distingue un vrai appel à la base de
// fuseaux (tzdata, via ::localtime_r) d'un décalage constant codé en dur --
// un offset fixe serait juste faux six mois par an, exactement la confusion
// exprimée par l'utilisateur à l'origine de ce round.
//
// Les deux instants ci-dessous partagent volontairement la même heure UTC
// (12:00:00) : seule la date diffère, ce qui isole strictement l'effet de la
// bascule DST sur l'écart mesuré plus bas. Choisis "franchement à
// l'intérieur" de chaque période -- loin des deux bascules de Toronto en
// 2026 (dimanche 8 mars, dimanche 1er novembre) -- et vérifiés
// indépendamment (voir rapport de tâche) :
//   TZ=America/Toronto date -d @1768478400  -> Thu Jan 15 07:00:00 EST 2026
//   TZ=America/Toronto date -d @1786795200  -> Sat Aug 15 08:00:00 EDT 2026
TEST(session_clock_follows_daylight_saving_under_a_single_timezone) {
  TzGuard tz("America/Toronto");

  constexpr std::int64_t kJanuaryEpoch = 1768478400;  // 2026-01-15 12:00:00 UTC
  constexpr std::int64_t kAugustEpoch = 1786795200;   // 2026-08-15 12:00:00 UTC

  FakePlatformAt jan(kJanuaryEpoch);
  Session sess_jan(jan, g_fds, 40, 12);
  Surface s_jan(40, 12);
  sess_jan.render(s_jan);
  const std::string panel_jan = s_jan.text_row(11);
  const auto pos_jan = panel_jan.find("07:00");  // EST : UTC-5
  CHECK(pos_jan != std::string::npos);

  FakePlatformAt aug(kAugustEpoch);
  Session sess_aug(aug, g_fds, 40, 12);
  Surface s_aug(40, 12);
  sess_aug.render(s_aug);
  const std::string panel_aug = s_aug.text_row(11);
  const auto pos_aug = panel_aug.find("08:00");  // EDT : UTC-4
  CHECK(pos_aug != std::string::npos);

  // REQUIRE, pas seulement les deux CHECK ci-dessus : sans les deux positions
  // trouvées, le substr() qui suit déréférencerait au-delà de la chaîne --
  // exactement le cas que la convention du harnais réserve à REQUIRE (voir
  // harness.hpp), pas à un CHECK laissé sans garde.
  REQUIRE(pos_jan != std::string::npos);
  REQUIRE(pos_aug != std::string::npos);

  // Preuve centrale de cet item : l'écart heure-UTC / heure-rendue -- dérivé
  // de l'heure RÉELLEMENT rendue (pas d'une valeur supposée) et de
  // ::gmtime_r, indépendamment du code sous test -- N'EST PAS le même aux
  // deux dates, bien que le fuseau soit identique.
  const int rendered_hour_jan = std::stoi(panel_jan.substr(pos_jan, 2));
  const int rendered_hour_aug = std::stoi(panel_aug.substr(pos_aug, 2));

  std::time_t t_jan = static_cast<std::time_t>(kJanuaryEpoch);
  std::time_t t_aug = static_cast<std::time_t>(kAugustEpoch);
  std::tm utc_jan{};
  std::tm utc_aug{};
  ::gmtime_r(&t_jan, &utc_jan);
  ::gmtime_r(&t_aug, &utc_aug);
  const int offset_jan = utc_jan.tm_hour - rendered_hour_jan;
  const int offset_aug = utc_aug.tm_hour - rendered_hour_aug;
  CHECK(offset_jan != offset_aug);
  CHECK_EQ(offset_jan, 5);
  CHECK_EQ(offset_aug, 4);
}

TEST(session_draws_a_decorated_window_with_its_title) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);

  bool found_title = false;
  for (int y = 0; y < 11; ++y) {
    if (s.text_row(y).find("Bloc") != std::string::npos) found_title = true;
  }
  CHECK(found_title);
}

// A1 : sans le correctif, ce test échouait — le message complet, tronqué à
// 12 colonnes par View::text (qui clippe à la largeur de la vue plutôt que
// de couper au dernier mot entier), produit "terminal tro" : la sous-chaîne
// "petit" (qui commence au 15e caractère du message complet) n'y figure
// jamais. C'est exactement la propriété que le rapport de tâche doit
// prouver par discrimination — voir aussi le test dédié ci-dessous, qui
// isole la même propriété sans dépendre d'un comptage de caractères précis.
TEST(session_survives_a_terminal_smaller_than_the_minimum) {
  FakePlatform plat;
  Session sess(plat, g_fds, 12, 3);
  Surface s(12, 3);
  sess.render(s);  // ne doit ni planter ni écrire hors surface
  CHECK(s.text_row(0).find("petit") != std::string::npos);
}

// Le geste que la main fait pour « quitter » doit DÉTACHER. S'il détruisait
// la session, revenir donnerait un bureau vide -- soit exactement l'inverse
// de ce que ce programme promet. Les deux drapeaux sont donc vérifiés :
// detach_ levé ET quit_ intact, faute de quoi un futur raccourci qui lèverait
// les deux passerait ici sans être vu.
TEST(session_detaches_on_ctrl_q_and_keeps_the_session_alive) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'q', sshos::mod::Ctrl}});
  CHECK(!sess.wants_quit());
  CHECK(sess.take_detach());
  // Consommé une seule fois : un démon qui relit le drapeau au tour suivant
  // ne doit pas congédier une seconde fois le client qui vient d'arriver.
  CHECK(!sess.take_detach());
}

TEST(session_detaches_on_the_leader_chord) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'd', 0}});
  CHECK(!sess.wants_quit());
  CHECK(sess.take_detach());
}

// Le pendant du test précédent : détruire la session pour de bon reste
// possible, mais il faut le demander par son nom.
TEST(session_quits_only_when_the_menu_says_so) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  for (char c : std::string("fermer")) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{
        sshos::Key::Char, static_cast<char32_t>(c), 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  // ON DEMANDE D'ABORD. Fermer la session détruit le travail de toutes les
  // fenêtres à la fois, et c'était jusqu'ici à un clic de distance -- au
  // milieu du menu, juste sous une entrée qui ne détruit rien.
  CHECK(!sess.wants_quit());

  // La modale s'ouvre sur « annuler » : Entrée seule ne détruit rien. Il
  // faut ALLER CHERCHER la confirmation, ce qui est le point.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  CHECK(!sess.wants_quit());

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  for (char c : std::string("fermer")) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{
        sshos::Key::Char, static_cast<char32_t>(c), 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Tab, 0, 0}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  CHECK(sess.wants_quit());
  CHECK(!sess.take_detach());
}

// L'AUTRE SORTIE : elle rend la main SANS rien détruire. Les deux étaient
// nommées « Quitter » et « Quitter la session », donc indistinguables --
// et c'est la destructive qu'on choisissait par défaut.
TEST(session_detaches_without_destroying_anything_from_the_menu) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  for (char c : std::string("quitter")) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{
        sshos::Key::Char, static_cast<char32_t>(c), 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});

  CHECK(!sess.wants_quit());
  CHECK(sess.take_detach());
}

// A1, second embranchement : quand la largeur suffit à accueillir le message
// complet (40 >= sa longueur), c'est bien LUI qui doit s'afficher, pas la
// forme courte — même si la hauteur, elle, reste insuffisante. Discriminé
// par mutation (voir le rapport de tâche) plutôt que contre du code
// antérieur : le code d'avant A1 affichait déjà le message complet dans ce
// cas précis (il l'affichait TOUJOURS, sans branche), donc ce test seul ne
// distinguerait pas les deux versions — c'est le test suivant qui joue ce
// rôle pour l'autre embranchement.
TEST(session_shows_full_warning_when_width_permits_but_height_does_not) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 3);
  Surface s(40, 3);
  sess.render(s);
  CHECK(s.text_row(0).find("terminal trop petit - 40x12 minimum") !=
        std::string::npos);
}

// A1, l'embranchement qui a motivé le correctif : en dessous de la longueur
// du message complet, c'est la forme courte qui doit s'afficher — pas le
// message complet tronqué par le clip de View::text (qui produirait
// "terminal tro", sans jamais "trop petit" ni "40x12 minimum" en clair).
// Discrimine directement contre le code d'avant A1 : celui-là échoue au
// premier CHECK ci-dessous (il n'écrit jamais "trop petit" tel quel, ce
// segment est coupé par le clip avant d'être atteint).
TEST(session_shows_short_warning_when_width_is_too_small_for_the_full_message) {
  FakePlatform plat;
  Session sess(plat, g_fds, 12, 3);
  Surface s(12, 3);
  sess.render(s);
  const std::string row = s.text_row(0);
  CHECK(row.find("trop petit") != std::string::npos);
  CHECK(row.find("40x12 minimum") == std::string::npos);
}

// A3 : Session ne mémorise plus cols/rows depuis le constructeur (dead state
// supprimé, cf. rapport de tâche) — render() doit tirer TOUTE sa géométrie
// de la Surface qu'on lui passe, à chaque appel, jamais d'un état interne
// figé à la construction. Construire avec des dimensions qui ne
// correspondent à AUCUNE des deux Surfaces rendues ensuite est la façon la
// plus directe de le prouver : si un futur maintainer réintroduisait un
// cache de géométrie (cols_/rows_ pris à la construction, ou mémorisés au
// premier render()), le panneau et la boîte resteraient figés sur ces
// valeurs-là au lieu de suivre chaque Surface — voir le rapport de tâche
// pour la preuve par mutation (le code actuel, comme celui du plan avant
// A3, ne lit déjà jamais cols_/rows_ dans render() : ce test ne discrimine
// donc pas contre une version antérieure réelle, seulement contre une
// régression hypothétique, démontrée par mutation).
TEST(session_geometry_follows_the_surface_not_the_constructor_arguments) {
  FakePlatform plat;
  Session sess(plat, g_fds, 999, 999);

  Surface a(40, 12);
  sess.render(a);
  CHECK(a.text_row(11).find("ssh_os") != std::string::npos);
  CHECK(a.text_row(10).find("ssh_os") == std::string::npos);
  bool found_title_a = false;
  for (int y = 0; y < 11; ++y) {
    if (a.text_row(y).find("Bloc") != std::string::npos) found_title_a = true;
  }
  CHECK(found_title_a);

  Surface b(60, 20);
  sess.render(b);
  CHECK(b.text_row(19).find("ssh_os") != std::string::npos);
  CHECK(b.text_row(18).find("ssh_os") == std::string::npos);
  bool found_title_b = false;
  for (int y = 0; y < 19; ++y) {
    if (b.text_row(y).find("Bloc") != std::string::npos) found_title_b = true;
  }
  CHECK(found_title_b);
}

TEST(session_forwards_a_click_inside_the_client_area_to_the_app) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);  // crée la fenêtre

  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.x = 9;
  m.y = 4;
  sess.on_input(sshos::InputEvent{m});

  Surface again(80, 24);
  sess.render(again);
  bool found = false;
  for (int y = 0; y < 23; ++y) {
    if (again.text_row(y).find("clics: 1") != std::string::npos) found = true;
  }
  CHECK(found);
}

// Le pendant du test précédent : un clic sur la décoration ou sur le
// bureau ne doit PAS être livré à l'application. Sans cette garde, une
// application recevrait des coordonnées négatives ou hors de sa surface.
TEST(session_does_not_forward_a_click_outside_the_client_area) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.x = 2;  // colonne gauche du cadre : la bordure, pas la zone cliente
  m.y = 5;
  sess.on_input(sshos::InputEvent{m});

  Surface again(80, 24);
  sess.render(again);
  bool zero = false;
  for (int y = 0; y < 23; ++y) {
    if (again.text_row(y).find("clics: 0") != std::string::npos) zero = true;
  }
  CHECK(zero);
}

// Une frame rendue deux fois de suite à taille constante ne doit annoncer
// qu'UN redimensionnement. C'est le relevé qui servira de preuve au geste
// complet, à la tâche 5.
TEST(session_announces_a_size_change_only_when_it_changes) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  sess.render(s);
  sess.render(s);

  bool once = false;
  for (int y = 0; y < 23; ++y) {
    if (s.text_row(y).find("resize: 1") != std::string::npos) once = true;
  }
  CHECK(once);
}

TEST(session_uses_ascii_borders_until_the_client_announces_utf8) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);

  Surface ascii(80, 24);
  sess.render(ascii);
  CHECK_EQ(ascii.at(2, 14).ch, U'+');  // coin bas-gauche du cadre {2,1,44,14}

  sshos::OutputProfile p;
  p.depth = sshos::ColorDepth::TrueColor;
  p.utf8 = true;
  sess.set_output(p);

  Surface uni(80, 24);
  sess.render(uni);
  CHECK_EQ(uni.at(2, 14).ch, U'└');
}

// La zone de travail s'arrête au-dessus du panneau : une fenêtre ne peut
// pas recouvrir la barre des tâches, quelle que soit sa géométrie voulue.
TEST(session_keeps_every_window_above_the_panel) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  CHECK(s.text_row(23).find("ssh_os") != std::string::npos);
}

namespace {

// Presse, bouge, relâche. Le bouton 0 est le gauche (voir parser.cpp, où
// le bouton est décodé par `cb & 3`).
void press_at(Session& s, int x, int y) {
  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.button = 0;
  m.x = x;
  m.y = y;
  s.on_input(sshos::InputEvent{m});
}

void motion_to(Session& s, int x, int y, std::uint8_t button = 0) {
  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Motion;
  m.button = button;
  m.x = x;
  m.y = y;
  s.on_input(sshos::InputEvent{m});
}

void release_at(Session& s, int x, int y) {
  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Release;
  m.button = 0;
  m.x = x;
  m.y = y;
  s.on_input(sshos::InputEvent{m});
}

// Le bouton DROIT. Un clic droit envoie deux évènements comme les autres :
// s'en tenir à l'appui dans un test laisserait passer un menu qui se
// rouvre à chaque relâchement.
void right_press(Session& s, int x, int y) {
  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.button = 2;
  m.x = x;
  m.y = y;
  s.on_input(sshos::InputEvent{m});
}

void right_release(Session& s, int x, int y) {
  sshos::MouseEvent m;
  m.action = sshos::MouseAction::Release;
  m.button = 2;
  m.x = x;
  m.y = y;
  s.on_input(sshos::InputEvent{m});
}

// La géométrie du cadre, relevée sur la surface : la première ligne qui
// porte le titre.
bool surface_contains(const Surface& s, const std::string& needle) {
  for (int y = 0; y < s.h(); ++y) {
    if (s.text_row(y).find(needle) != std::string::npos) return true;
  }
  return false;
}

// La colonne la plus à droite qui appartienne encore à une fenêtre, sur la
// ligne donnée. Mesure la géométrie par le hit-test plutôt que par les
// glyphes : c'est ce que l'utilisateur peut réellement attraper.
int left_edge_of(Session& s, int y) {
  for (int x = 0; x < 80; ++x) {
    if (s.hit_window_at(x, y).what != sshos::WinHit::None) return x;
  }
  return -1;
}

int right_edge_of(Session& s, int y) {
  for (int x = 79; x >= 0; --x) {
    if (s.hit_window_at(x, y).what != sshos::WinHit::None) return x;
  }
  return -1;
}

// Combien de fenêtres distinctes le hit-test voit-il ? Compter les lignes
// de texte ne suffit pas : deux fenêtres identiques se recouvrent, et la
// seconde efface le libellé de la première.
size_t count_windows(Session& s, int cols, int rows) {
  std::vector<sshos::WindowId> ids;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const sshos::WinHitResult h = s.hit_window_at(x, y);
      if (h.what == sshos::WinHit::None) continue;
      if (std::find(ids.begin(), ids.end(), h.win) == ids.end()) {
        ids.push_back(h.win);
      }
    }
  }
  return ids.size();
}

// La barre de titre d'une fenetre, quelle que soit l'application dedans :
// la fenetre amorcee porte le double factice, celles qu'on ouvre portent
// une vraie application.
int title_row_of(const Surface& s, int rows) {
  for (int y = 0; y < rows; ++y) {
    const std::string row = s.text_row(y);
    if (row.find("Bloc") != std::string::npos ||
        row.find("Editeur") != std::string::npos ||
        row.find("Fichiers") != std::string::npos) {
      return y;
    }
  }
  return -1;
}

}  // namespace

TEST(session_moves_a_window_dragged_by_its_title_bar) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 24), 1);

  press_at(sess, 5, 1);
  motion_to(sess, 8, 6);
  release_at(sess, 8, 6);

  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 24), 6);
}

// Le contrat central du glissement de redimensionnement : pendant le
// geste, seul un contour bouge. L'application n'apprend sa nouvelle taille
// qu'au relâchement -- UNE fois, quel que soit le nombre de mouvements.
//
// La composition intercalée entre chaque mouvement n'est pas décorative :
// c'est elle qui donne des dents au test. Sans elle, une implémentation qui
// redimensionnerait la fenêtre à chaque mouvement passerait aussi, puisque
// on_resize() n'est émis que depuis render() -- le compteur ne verrait
// jamais que l'état initial et l'état final. Avec elle, une telle
// implémentation annonce une douzaine de tailles au lieu d'une.
TEST(session_tells_the_app_its_new_size_exactly_once_per_resize_gesture) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);  // resize: 1

  press_at(sess, 45, 14);  // coin bas-droit du cadre {2,1,44,14}
  for (int i = 0; i < 10; ++i) {
    motion_to(sess, 45 + i, 14 + (i % 3));
    Surface mid(80, 24);
    sess.render(mid);
  }
  release_at(sess, 54, 16);

  Surface after(80, 24);
  sess.render(after);
  bool twice = false;
  for (int y = 0; y < 23; ++y) {
    if (after.text_row(y).find("resize: 2") != std::string::npos) twice = true;
  }
  CHECK(twice);
}

// Les sept chemins d'annulation. Ils asseyent TOUS la même conclusion : la
// fenêtre est revenue là où le geste l'avait prise. Un seul chemin oublié
// laisserait un glissement fantôme capable de déplacer une fenêtre au
// prochain mouvement de souris, longtemps après que l'utilisateur a lâché
// le bouton.
TEST(session_cancels_a_drag_on_every_one_of_the_seven_paths) {
  const int kPaths = 7;
  for (int path = 0; path < kPaths; ++path) {
    FakePlatform plat;
    Session sess(plat, g_fds, 80, 24);
    Surface s(80, 24);
    sess.render(s);
    REQUIRE_EQ(title_row_of(s, 24), 1);

    press_at(sess, 5, 1);
    motion_to(sess, 8, 6);  // le glissement est engagé, la fenêtre a suivi

    switch (path) {
      case 0:  // Échap
        sess.on_input(
            sshos::InputEvent{sshos::KeyEvent{sshos::Key::Escape, 0, 0}});
        break;
      case 1:  // n'importe quelle autre frappe
        sess.on_input(
            sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'z', 0}});
        break;
      case 2:  // perte de focus du terminal
        sess.on_input(sshos::InputEvent{sshos::FocusEvent{false}});
        break;
      case 3:  // mouvement sans bouton : le relâchement s'est perdu
        motion_to(sess, 20, 15, 3);
        break;
      case 4:  // détachement du client, puis attache du suivant : DEUX sites
               // d'appel dans le démon, une seule méthode ici. Le câblage
               // des deux sites est couvert par le test bout-en-bout
               // plus bas, pas par ce cas.
        sess.cancel_drag();
        break;
      case 5:  // second appui pendant le glissement (ici sur le bouton de
               // fermeture, qui ne doit pas non plus fermer quoi que ce soit)
        press_at(sess, 44, 1);
        break;
      case 6: {  // chien de garde : plus de deux secondes sans nouvelle
        plat.advance_steady(std::chrono::milliseconds(2100));
        Surface tick(80, 24);
        sess.render(tick);
        break;
      }
      default:
        break;
    }

    // Après annulation, un mouvement de souris ne doit plus rien traîner.
    motion_to(sess, 40, 20);
    release_at(sess, 40, 20);

    Surface after(80, 24);
    sess.render(after);
    CHECK_EQ(title_row_of(after, 24), 1);
  }
}

// Le chien de garde est relu à DEUX endroits, à l'entrée et à la
// composition, et le test des sept chemins ne discrimine que le premier :
// son mouvement de contrôle repasse par on_input(), qui aurait balayé le
// geste de toute façon. Ce test-ci isole le second.
//
// Le scénario est celui qui rend la relecture à la composition nécessaire :
// un redimensionnement dont le relâchement s'est perdu, et plus une seule
// entrée derrière. Le contour élastique resterait peint indéfiniment, sans
// rien pour venir l'effacer -- une trace à l'écran qu'aucun geste de
// l'utilisateur ne peut plus enlever.
TEST(session_sweeps_a_stale_resize_outline_without_any_further_input) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  press_at(sess, 45, 14);   // coin bas-droit du cadre {2,1,44,14}
  motion_to(sess, 60, 20);  // contour étiré jusqu'à {2,1,59,20}

  Surface during(80, 24);
  sess.render(during);
  // Coin bas-droit du contour, hors du cadre de la fenêtre : il n'y a que le
  // contour pour poser un glyphe là. Bordures ASCII, faute de set_output().
  REQUIRE_EQ(during.at(60, 20).ch, U'+');

  // Plus aucune entrée, seulement du temps qui passe et des compositions.
  plat.advance_steady(std::chrono::milliseconds(2100));
  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(after.at(60, 20).ch, U' ');
}

TEST(session_draws_the_focused_window_on_top) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  sess.open_from_catalog("editeur");
  Surface two(80, 24);
  sess.render(two);

  int titles = 0;
  for (int y = 0; y < 23; ++y) {
    const std::string row = two.text_row(y);
    if (row.find("Bloc") != std::string::npos ||
        row.find("Editeur") != std::string::npos) {
      ++titles;
    }
  }
  CHECK_EQ(titles, 2);  // deux fenêtres, deux barres de titre visibles
}

// Une seule fenêtre porte les couleurs du focus. Le test ne rejoue pas le
// calcul du thème : il vérifie que les deux barres de titre diffèrent, PUIS
// qu'elles s'échangent quand le focus change -- ni « tout focalisé » ni
// « tout terne » ne peut imiter cet échange.
TEST(session_dims_every_window_that_does_not_have_the_focus) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE(sess.open_from_catalog("editeur") != 0u);
  Surface two(80, 24);
  sess.render(two);

  // Barre de titre du fond en ligne 1 (cadre {2,1,44,14}), celle du dessus
  // en ligne 2 ({4,2,44,14}).
  const sshos::Color back = two.at(20, 1).bg;
  const sshos::Color front = two.at(20, 2).bg;
  CHECK(!(back == front));

  press_at(sess, 2, 5);  // bord gauche du fond : il passe devant
  release_at(sess, 2, 5);
  Surface swapped(80, 24);
  sess.render(swapped);
  CHECK(swapped.at(20, 1).bg == front);
}

// hit_window_at() est l'entrée publique du test de collision. Avec une seule
// fenêtre, parcourir la pile à l'endroit ou à l'envers donne le même
// résultat : il faut deux fenêtres qui se recouvrent pour que l'ordre
// compte.
TEST(session_hit_test_answers_for_the_window_on_top) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  const sshos::WindowId top = sess.open_from_catalog("editeur");
  REQUIRE(top != 0u);
  Surface two(80, 24);
  sess.render(two);

  // (20, 5) tombe dans les DEUX cadres, {2,1,44,14} et {4,2,44,14}.
  const sshos::WinHitResult over = sess.hit_window_at(20, 5);
  CHECK_EQ(over.win, top);
  CHECK(over.what == sshos::WinHit::Client);

  // Colonne 2 : seul le cadre du dessous l'atteint.
  const sshos::WinHitResult below = sess.hit_window_at(2, 5);
  CHECK(below.win != top);
  CHECK(below.what == sshos::WinHit::Frame);

  // Et le bureau nu ne répond rien.
  CHECK(sess.hit_window_at(70, 20).what == sshos::WinHit::None);
}

// Sous 40x12, la session affiche un avertissement -- et ne touche à RIEN.
// Réagrandir doit rendre le bureau tel qu'il était, pas un bureau neuf.
//
// La comparaison porte sur les 24 lignes, pas sur un décompte de titres :
// une disposition peut se perdre sans qu'aucune fenêtre disparaisse, et
// c'est même le mode de défaillance le plus probable -- des user_rect
// écrasés par la géométrie contrainte du petit terminal.
TEST(session_preserves_the_desktop_across_a_terminal_too_small_to_draw_it) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface big(80, 24);
  sess.render(big);
  REQUIRE(sess.open_from_catalog("editeur") != 0u);
  REQUIRE(sess.open_from_catalog("editeur") != 0u);
  sess.render(big);

  std::vector<std::string> before;
  for (int y = 0; y < 24; ++y) before.push_back(big.text_row(y));
  int titles = 0;
  for (int y = 0; y < 23; ++y) {
    const std::string& row = before[static_cast<size_t>(y)];
    if (row.find("Bloc") != std::string::npos ||
        row.find("Editeur") != std::string::npos) {
      ++titles;
    }
  }
  REQUIRE_EQ(titles, 3);

  Surface tiny(20, 5);
  sess.render(tiny);
  CHECK(tiny.text_row(0).find("petit") != std::string::npos);

  Surface again(80, 24);
  sess.render(again);
  for (int y = 0; y < 24; ++y) {
    CHECK_EQ(again.text_row(y), before[static_cast<size_t>(y)]);
  }
}

// LE canal que la tâche 9 ouvre. Sans lui, l'horloge du panneau resterait
// figée jusqu'à la prochaine frappe : le démon ne compose que sur une frame
// sale, et render() -- qui n'est appelée QUE sur une frame déjà sale -- ne
// peut donc pas être ce qui découvre qu'une minute a tourné.
TEST(session_asks_for_a_repaint_when_the_clock_changes_minute) {
  MovingPlatform plat(1786370700);  // 2026-08-10 14:05:00 UTC
  Session sess(plat, g_fds, 80, 24);
  Surface out(80, 24);
  sess.render(out);
  sess.take_dirty();  // la première composition amorce l'horloge

  CHECK(!sess.take_dirty());
  plat.advance(30);
  CHECK(!sess.take_dirty());  // même minute : rien à repeindre
  plat.advance(30);
  CHECK(sess.take_dirty());   // la minute a tourné
  CHECK(!sess.take_dirty());  // et le drapeau se consomme
}

// Le bureau exécute vraiment l'action, pas seulement le dispatcheur.
TEST(session_moves_the_focused_window_with_the_leader_table) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  const int before = title_row_of(s, 24);
  REQUIRE(before >= 0);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'j', 0}});

  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 24), before + 1);
}

// Les quatre redimensionnements clavier travaillent sur la géométrie
// VOULUE, donc rétrécir d'autant qu'on a agrandi rend exactement la taille
// de départ.
TEST(session_resizes_the_focused_window_with_the_leader_table) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  const int before = right_edge_of(sess, 1);
  REQUIRE(before > 0);

  for (int i = 0; i < 2; ++i) {
    sess.on_input(sshos::InputEvent{
        sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'L', 0}});
  }
  sess.render(s);
  CHECK_EQ(right_edge_of(sess, 1), before + 2);

  for (int i = 0; i < 2; ++i) {
    sess.on_input(sshos::InputEvent{
        sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'H', 0}});
  }
  sess.render(s);
  CHECK_EQ(right_edge_of(sess, 1), before);
}

// Un pas clavier vaut exactement une cellule -- ni plus, ni moins, et
// surtout pas zéro : aimanter un pas d'une cellule avec une tolérance d'une
// cellule collerait la fenêtre au bord pour toujours. Et pousser au-delà du
// bord ne l'emmène pas hors de l'écran, où il faudrait autant de frappes
// pour la ramener.
TEST(session_bounds_a_window_moved_with_the_keyboard) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(left_edge_of(sess, 1), 2);

  // Un seul pas vers la gauche déplace d'exactement une cellule.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'h', 0}});
  sess.render(s);
  CHECK_EQ(left_edge_of(sess, 1), 1);

  // Soixante pas à droite : la fenêtre bute contre le bord droit au lieu de
  // partir au loin. Soixante pas en retour la ramènent donc au bord gauche,
  // pas à son point de départ.
  for (int i = 0; i < 60; ++i) {
    sess.on_input(sshos::InputEvent{
        sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'l', 0}});
  }
  sess.render(s);
  CHECK_EQ(right_edge_of(sess, 1), 79);

  for (int i = 0; i < 60; ++i) {
    sess.on_input(sshos::InputEvent{
        sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'h', 0}});
  }
  sess.render(s);
  CHECK_EQ(left_edge_of(sess, 1), 0);
}

// Les raccourcis ne doivent PAS fuir vers l'application, et une frappe
// ordinaire ne doit PAS être avalée par le bureau.
TEST(session_never_leaks_a_leader_chord_to_the_application) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  // Bloc déplace son curseur avec les flèches et marque son titre sur 'e'.
  // Un accord complet ne doit rien lui faire.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'e', 0}});
  Surface after(80, 24);
  sess.render(after);
  CHECK(after.text_row(1).find("Bloc *") == std::string::npos);

  // Alors que la même touche seule lui parvient.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'e', 0}});
  Surface typed(80, 24);
  sess.render(typed);
  CHECK(typed.text_row(1).find("Bloc *") != std::string::npos);
}

// ToggleMouse n'a pas de message de protocole : la bascule voyage dans le
// flux de trames, que le client recopie verbatim.
TEST(session_emits_the_mouse_toggle_out_of_band) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  CHECK(sess.take_out_of_band().empty());

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'm', 0}});

  const std::string oob = sess.take_out_of_band();
  CHECK(oob.find("\033[?1002l") != std::string::npos);
  CHECK(oob.find("\033[?1006l") != std::string::npos);
  CHECK(sess.take_out_of_band().empty());  // consommé une seule fois

  // Et la bascule bascule : le second accord remet la souris.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'm', 0}});
  const std::string back = sess.take_out_of_band();
  CHECK(back.find("\033[?1002h") != std::string::npos);
  CHECK(back.find("\033[?1006h") != std::string::npos);
}

TEST(session_asks_for_a_full_repaint_on_demand) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  CHECK(!sess.take_repaint());
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'r', 0}});
  CHECK(sess.take_repaint());
  CHECK(!sess.take_repaint());
}

// Le menu s'ouvre au clavier, capture les frappes, et lance ce qu'on a
// choisi. C'est le seul chemin qui ouvre une application depuis le bureau.
TEST(session_opens_an_application_through_the_menu) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  Surface open(80, 24);
  sess.render(open);
  // Le menu est là : il porte ses entrées, dont celle qui ne lance rien.
  CHECK(surface_contains(open, "Fermer la session"));

  // Filtrer sur « moniteur » puis valider.
  for (const char32_t c : {U'f', U'i', U'c', U'h'}) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, c, 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});

  Surface after(80, 24);
  sess.render(after);
  bool found = false;
  for (int y = 0; y < 23; ++y) {
    if (after.text_row(y).find("Fichiers") != std::string::npos) found = true;
  }
  CHECK(found);

  // Relancer la même entrée rappelle la fenêtre au lieu d'en empiler une
  // seconde : c'est ce que fait une épinglée dans toute barre des tâches.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  for (const char32_t c : {U'b', U'a', U't', U't'}) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, c, 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  Surface twice(80, 24);
  sess.render(twice);
  (void)twice;
  // Deux fenêtres en tout -- le Bloc du démarrage et le Battement --, pas
  // trois. Compté au hit-test : deux Battement en cascade se recouvrent et
  // le second effacerait le texte du premier, ce qui rendrait un décompte
  // de lignes complaisant.
  CHECK_EQ(count_windows(sess, 80, 23), static_cast<size_t>(2));
}

// Échap referme le menu sans rien lancer, et rend le clavier à
// l'application.
TEST(session_closes_the_menu_on_escape_without_running_anything) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Escape, 0, 0}});

  Surface after(80, 24);
  sess.render(after);
  // Une seule fenêtre, et plus de ligne de saisie.
  int titles = 0;
  for (int y = 0; y < 23; ++y) {
    const std::string row = after.text_row(y);
    if (row.find("Bloc") != std::string::npos ||
        row.find("Editeur") != std::string::npos) {
      ++titles;
    }
  }
  CHECK_EQ(titles, 1);

  // Le clavier est revenu à l'application.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'e', 0}});
  Surface typed(80, 24);
  sess.render(typed);
  CHECK(typed.text_row(1).find("Bloc *") != std::string::npos);
}

// Le bouton de menu du panneau ouvre le menu, et l'entrée d'une fenêtre la
// réduit puis la rappelle.
TEST(session_answers_clicks_on_the_panel) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  press_at(sess, 2, 23);  // « ☰ ssh_os » / « ssh_os »
  Surface menu(80, 24);
  sess.render(menu);
  CHECK(surface_contains(menu, "Fermer la session"));

  press_at(sess, 2, 23);  // le même clic referme (hors du menu)
  Surface closed(80, 24);
  sess.render(closed);

  // L'entrée de la fenêtre active la réduit.
  int task_x = -1;
  for (int x = 0; x < 80; ++x) {
    if (closed.at(x, 23).ch == U'*' || closed.at(x, 23).ch == U'●') task_x = x;
  }
  REQUIRE(task_x >= 0);
  press_at(sess, task_x, 23);
  Surface hidden(80, 24);
  sess.render(hidden);
  CHECK(title_row_of(hidden, 23) < 0);

  press_at(sess, task_x, 23);
  Surface back(80, 24);
  sess.render(back);
  CHECK(title_row_of(back, 23) >= 0);
}

// Plein écran : le panneau disparaît. C'est toute la différence avec
// maximisé, qui s'arrête à la zone de travail pour le laisser visible.
TEST(session_hides_the_panel_under_a_fullscreen_window) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE(s.text_row(23).find("ssh_os") != std::string::npos);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'f', 0}});
  Surface full(80, 24);
  sess.render(full);
  CHECK(full.text_row(23).find("ssh_os") == std::string::npos);
  // Et le repeint complet est réclamé : aucun delta ne dit « le panneau a
  // disparu » de façon fiable.
  CHECK(sess.take_repaint());

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'f', 0}});
  Surface again(80, 24);
  sess.render(again);
  CHECK(again.text_row(23).find("ssh_os") != std::string::npos);
}

// Bloc refuse de se fermer une fois modifiée : la session doit poser la
// question, pas fermer, et pas non plus l'ignorer.
TEST(session_asks_before_closing_a_modified_window_and_honours_the_answer) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'z', 0}});

  // Ctrl+A w : demander la fermeture.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'w', 0}});

  Surface asked(80, 24);
  sess.render(asked);
  bool question = false;
  for (int y = 0; y < 23; ++y) {
    if (asked.text_row(y).find("modifications") != std::string::npos) question = true;
  }
  CHECK(question);

  // Entrée valide le bouton par défaut, qui est Annuler : la fenêtre reste.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  Surface kept(80, 24);
  sess.render(kept);
  // L'étoile du titre dit que c'est bien LA fenêtre modifiée qui est
  // restée : le bureau rouvre aussitôt une fenêtre neuve quand la pile se
  // vide (ensure_window), donc compter les titres ne distinguerait pas
  // « restée » de « fermée puis remplacée ».
  CHECK(surface_contains(kept, "Bloc *"));
  // Et le dialogue a bien disparu.
  CHECK(!surface_contains(kept, "modifications"));

  // Cette fois on confirme : Tab pour atteindre Confirmer, puis Entrée.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'w', 0}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Tab, 0, 0}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});

  Surface gone(80, 24);
  sess.render(gone);
  CHECK(!surface_contains(gone, "Bloc *"));
  // Le bureau n'est jamais vide : une fenêtre neuve a pris la place.
  CHECK(title_row_of(gone, 24) >= 0);
}

// Tant que la modale est là, ni l'application ni les raccourcis ne voient
// quoi que ce soit. C'est tout ce que « modal » veut dire.
TEST(session_lets_nothing_through_while_the_modal_is_up) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'z', 0}});
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'w', 0}});
  Surface asked(80, 24);
  sess.render(asked);
  REQUIRE(surface_contains(asked, "modifications"));

  // Le raccourci du menu ne l'ouvre pas.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  Surface still(80, 24);
  sess.render(still);
  CHECK(!surface_contains(still, "Fermer la session"));
  CHECK(surface_contains(still, "modifications"));

  // Un clic sur la barre de titre de la fenêtre en dessous n'engage aucun
  // déplacement : la fenêtre ne bouge pas d'une cellule.
  const int before = title_row_of(still, 24);
  press_at(sess, 10, before);
  motion_to(sess, 20, before + 5, 1);
  release_at(sess, 20, before + 5);
  Surface unmoved(80, 24);
  sess.render(unmoved);
  CHECK_EQ(title_row_of(unmoved, 24), before);

  // Échap referme sans fermer la fenêtre.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Escape, 0, 0}});
  Surface back(80, 24);
  sess.render(back);
  CHECK(!surface_contains(back, "modifications"));
  CHECK(title_row_of(back, 24) >= 0);
}

// Un accord entamé pendant un glissement annule le geste au lieu de
// l'exécuter -- la règle « toute frappe annule le glissement » de la
// tâche 5 passe AVANT le dispatcheur. La fenêtre revient donc là où le
// geste l'avait prise, et le 'w' qui suit n'est pas une fermeture : il
// tombe dans l'application, qui le prend pour une saisie.
TEST(session_cancels_a_drag_instead_of_closing_when_a_chord_starts_mid_gesture) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  const int before = title_row_of(s, 24);
  REQUIRE(before >= 0);

  press_at(sess, 5, before);
  motion_to(sess, 8, before + 5, 1);
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'w', 0}});

  motion_to(sess, 40, 20, 1);
  release_at(sess, 40, 20);
  Surface after(80, 24);
  sess.render(after);
  // Le geste a été défait, pas poursuivi : la fenêtre n'a pas bougé.
  CHECK_EQ(title_row_of(after, 24), before);
  // Et le 'w' est bien allé à l'application, qui se déclare modifiée.
  CHECK(surface_contains(after, "Bloc *"));
}

TEST(session_refuses_an_unknown_catalog_entry) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  CHECK_EQ(sess.open_from_catalog("il-n-existe-pas"), 0u);
}

// Un clic sur une fenêtre d'arrière-plan la ramène au premier plan, et un
// clic sur sa case [×] la ferme -- ELLE, pas celle qui avait le focus.
TEST(session_focuses_and_closes_the_window_under_the_pointer) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE(sess.open_from_catalog("editeur") != 0u);
  Surface two(80, 24);
  sess.render(two);
  // Deuxième fenêtre en cascade : cadre {4, 2, 44, 14}, titre en ligne 2.
  REQUIRE_EQ(two.text_row(1).find("Bloc"), std::string::size_type(4));
  REQUIRE_EQ(two.text_row(2).find("Editeur"), std::string::size_type(6));

  // Bord gauche de la fenêtre d'ARRIÈRE-plan, à gauche du cadre de celle du
  // dessus : elle prend le focus, donc le dessus. Le bord et non la barre de
  // titre, pour ne pas engager de déplacement au passage.
  press_at(sess, 2, 5);
  release_at(sess, 2, 5);
  Surface raised(80, 24);
  sess.render(raised);
  // Elle repasse devant : sa ligne cliente recouvre désormais la barre de
  // titre de l'autre, qui disparaît de la ligne 2.
  CHECK_EQ(raised.text_row(2).find("Bloc"), std::string::npos);

  // [×] de la fenêtre du dessus : cadre {2,1,44,14}, dernier bouton collé au
  // bord droit intérieur, colonnes 42..44.
  press_at(sess, 44, 1);
  release_at(sess, 44, 1);
  Surface closed(80, 24);
  sess.render(closed);
  CHECK_EQ(closed.text_row(1).find("Bloc"), std::string::npos);
  CHECK_EQ(closed.text_row(2).find("Editeur"), std::string::size_type(6));
}

// [_] réduit : la fenêtre sort de la composition sans rien perdre. [□]
// bascule maximisée puis rétablit EXACTEMENT la géométrie d'avant.
TEST(session_minimizes_and_maximizes_from_the_title_bar_buttons) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(s.text_row(1).find("Bloc"), std::string::size_type(4));

  // [□] au milieu : cadre {2,1,44,14}, boutons en colonnes 36..44.
  press_at(sess, 41, 1);
  release_at(sess, 41, 1);
  Surface maxed(80, 24);
  sess.render(maxed);
  // Maximisée sur la zone de travail {0,0,80,23} : titre en ligne 0.
  CHECK_EQ(maxed.text_row(0).find("Bloc"), std::string::size_type(2));

  press_at(sess, 74, 0);  // [□] du cadre maximisé : boutons en 70..78
  release_at(sess, 74, 0);
  Surface restored(80, 24);
  sess.render(restored);
  CHECK_EQ(restored.text_row(1).find("Bloc"), std::string::size_type(4));

  // [_] : colonnes 36..38 du cadre rétabli.
  press_at(sess, 37, 1);
  release_at(sess, 37, 1);
  Surface gone(80, 24);
  sess.render(gone);
  int titles = 0;
  for (int y = 0; y < 23; ++y) {
    const std::string row = gone.text_row(y);
    if (row.find("Bloc") != std::string::npos ||
        row.find("Editeur") != std::string::npos) {
      ++titles;
    }
  }
  CHECK_EQ(titles, 0);
}

// Un clic sur la barre de titre qui ne bouge pas la souris ne doit RIEN
// déplacer. Sans garde, l'aimantation du relâchement décale la fenêtre d'une
// cellule au premier clic venu : la première marche de la cascade tombe
// justement à une cellule du bord haut de la zone de travail.
TEST(session_does_not_move_a_window_merely_clicked_on_its_title_bar) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 24), 1);
  REQUIRE_EQ(s.text_row(1).find("Bloc"), std::string::size_type(4));

  press_at(sess, 5, 1);
  release_at(sess, 5, 1);
  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 24), 1);
  CHECK_EQ(after.text_row(1).find("Bloc"), std::string::size_type(4));
}

// L'aimantation s'applique au relâchement d'un DÉPLACEMENT : un bord lâché
// à une cellule d'un bord de la zone s'y colle, un bord lâché à trois ne
// bouge pas. Le titre commence deux colonnes après le cadre, ce qui donne
// l'abscisse du cadre à la lecture.
TEST(session_snaps_a_dragged_window_onto_a_nearby_edge) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(s.text_row(1).find("Bloc"), std::string::size_type(4));

  press_at(sess, 5, 1);  // prise à trois colonnes du bord gauche du cadre
  motion_to(sess, 4, 6);
  release_at(sess, 4, 6);  // poserait le cadre en x = 1
  Surface snapped(80, 24);
  sess.render(snapped);
  CHECK_EQ(snapped.text_row(6).find("Bloc"), std::string::size_type(2));

  press_at(sess, 2, 6);
  motion_to(sess, 5, 6);
  release_at(sess, 5, 6);  // pose le cadre en x = 3, trop loin pour aimanter
  Surface free_(80, 24);
  sess.render(free_);
  CHECK_EQ(free_.text_row(6).find("Bloc"), std::string::size_type(5));
}

// ---------------------------------------------------------------------
// Infrastructure bout-en-bout : un vrai démon (fork() + run_daemon() dans
// le fils, sans passer par --daemon) et un vrai client sur socket abstrait.
// ---------------------------------------------------------------------

namespace {

// Même précédent que unique_name() dans tests/test_net.cpp et
// unique_marker() dans tests/test_daemonize.cpp : un aléa frais tiré d'une
// source d'entropie du noyau à chaque appel, pas seulement le pid, pour
// rester correct sous deux espaces de noms pid distincts partageant un même
// espace de noms réseau.
std::string unique_name() {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "sshos-test-session/" << ::getpid() << '-' << std::hex << dist(rng);
  return os.str();
}

// Fait tourner un vrai démon dans un processus fils distinct, sans passer
// par `sshos --daemon` : /proc/self/exe depuis le binaire sshos_tests
// résout vers sshos_tests lui-même, dont main() (tests/main.cpp) traite
// argv[1] comme un filtre de nom de test, pas comme un mode démon — cette
// route est donc inutilisable ici. run_daemon() est lié statiquement via
// sshos_core (voir CMakeLists.txt) : l'appeler directement dans le fils,
// puis _exit() sans jamais revenir à main(), obtient le même résultat
// (un vrai process, un vrai socket, un vrai epoll) sans dupliquer
// l'aiguillage de main.cpp. Choix explicitement offert par le brief de
// tâche, en alternative à l'exécutable `sshos` réel.
//
// become_daemon() (double fork, setsid, redirection vers /dev/null) n'est
// délibérément PAS appelé : ce n'est pas ce que ce test vérifie (daemonize.*
// est couvert par tests/test_daemonize.cpp, propriété d'un autre chantier de
// cette tâche), et s'en passer garde stdout/stderr du fils utiles au
// diagnostic si run_daemon() échoue de façon inattendue.
class DaemonHandle {
 public:
  explicit DaemonHandle(std::string socket_name) {
    pid_ = ::fork();
    if (pid_ == 0) {
      _exit(sshos::run_daemon(socket_name));
    }
  }

  ~DaemonHandle() {
    if (pid_ > 0) {
      // SIGKILL est notre filet de sécurité, pas le chemin nominal : un
      // test qui veut vérifier un arrêt propre envoie lui-même SIGTERM et
      // attend la sortie AVANT que ce destructeur ne s'exécute (voir
      // end_to_end_attach_render_detach_kill plus bas). Envoyer SIGKILL à
      // un pid déjà sorti et récolté échoue silencieusement (ESRCH) ; un
      // second waitpid() sur un pid déjà récolté échoue tout aussi
      // silencieusement (ECHILD) — aucun des deux cas ne mérite de
      // diagnostic ici. SIGKILL ne peut ni être bloqué ni être ignoré :
      // l'attente qui suit n'a pas besoin d'être bornée séparément.
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }

  DaemonHandle(const DaemonHandle&) = delete;
  DaemonHandle& operator=(const DaemonHandle&) = delete;

  bool valid() const { return pid_ > 0; }
  pid_t pid() const { return pid_; }

 private:
  pid_t pid_ = -1;
};

// connect_abstract() lève tant que le fils n'a pas encore atteint
// bind_abstract() — la fenêtre entre fork() et ce point n'est pas bornée
// par construction (ordonnancement du noyau). Même schéma que
// start_daemon_and_connect() dans src/main.cpp : tentatives bornées avec un
// petit délai, pas d'attente active ni de pari sur un délai fixe unique.
constexpr int kConnectAttempts = 100;
constexpr int kConnectDelayUs = 20 * 1000;  // 100 x 20 ms = 2 s au pire

sshos::Fd connect_retry(const std::string& name) {
  for (int i = 0; i < kConnectAttempts; ++i) {
    try {
      return sshos::connect_abstract(name);
    } catch (const std::exception&) {
      ::usleep(kConnectDelayUs);
    }
  }
  return sshos::Fd();
}

// send() plutôt que write() : write() lève SIGPIPE sur une connexion déjà
// fermée par le pair, qui tue tout le binaire de test hors tty (même piège
// que celui documenté dans tests/test_daemonize.cpp pour les tubes
// anonymes). MSG_NOSIGNAL le désarme sans toucher la disposition globale du
// signal — pas question de faire fuiter un changement de disposition vers
// les autres test_*.cpp liés dans le même processus.
bool send_all(int fd, const std::string& bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
    const ssize_t n =
        ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

// Boucle de réception bornée dans le temps (poll() + recv() non bloquant en
// esprit, bien que le socket lui-même reste bloquant comme connect_abstract()
// le rend — poll() avant chaque recv() évite néanmoins tout blocage
// indéfini) : rend le premier message que Decoder parvient à assembler, ou
// rien si le délai expire ou si le pair ferme avant.
std::optional<sshos::Msg> recv_one(int fd, sshos::Decoder& dec, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[65536];
  for (;;) {
    if (dec.failed()) return std::nullopt;
    if (auto m = dec.next()) return m;

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::nullopt;
    const int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());

    pollfd pfd{fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, std::max(1, remaining));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (pr == 0) continue;  // la vérification de deadline ci-dessus tranche

    const ssize_t got = ::recv(fd, buf, sizeof buf, 0);
    if (got > 0) {
      dec.feed(std::string_view(buf, static_cast<size_t>(got)));
      continue;
    }
    if (got == 0) return std::nullopt;  // le pair a fermé avant le message attendu
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
    return std::nullopt;
  }
}

// Draine les messages reçus jusqu'à trouver une FrameMsg dont l'ansi
// contient les deux motifs demandés, ou jusqu'à expiration du délai global.
bool wait_for_frame_containing(int fd, sshos::Decoder& dec,
                               std::string_view needle_a,
                               std::string_view needle_b, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    auto m = recv_one(fd, dec, remaining);
    if (!m) return false;
    if (const auto* f = std::get_if<sshos::FrameMsg>(&*m)) {
      if (f->ansi.find(needle_a) != std::string::npos &&
          f->ansi.find(needle_b) != std::string::npos) {
        return true;
      }
    }
  }
}

// Détecte la fermeture côté pair SANS jamais lire le moindre octet, borné
// dans le temps. C'est délibéré, pas une paresse : pour le test Dirty
// ci-dessous, le principe même du scénario est de laisser une grosse
// quantité de données non lues s'accumuler côté noyau pour empêcher
// off_ de retomber à zéro ; un recv() ici, même un seul, libère de la place
// dans le tampon de réception du noyau, ce qui réarme EPOLLOUT côté démon
// et lui permet de vider davantage de la première trame — vidant
// précisément l'état (off_ > 0) que ce test cherche à observer. Une mesure
// directe (voir rapport de tâche) a d'ailleurs surpris : plus de 200 Kio
// passent avant le premier EAGAIN malgré un SO_RCVBUF réduit à sa valeur
// plancher, largement plus qu'une estimation initiale de quelques Kio — un
// premier essai de ce test avec un wait_for_peer_close qui lisait
// (recherchant l'EOF par vidange) échouait pour cette raison précise : il
// vidait lui-même la trame coincée avant que la seconde ne puisse
// déborder. POLLHUP est signalé par le noyau dès que le pair a fermé,
// indépendamment du fait qu'il reste ou non des octets non lus en attente
// (vérifié empiriquement, voir rapport de tâche) : c'est un signal
// suffisant et non intrusif.
bool wait_for_peer_close(int fd, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    pollfd pfd{fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, std::max(1, remaining));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0) continue;
    if ((pfd.revents & (POLLHUP | POLLERR)) != 0) return true;
    // POLLIN seul (données en attente, pas encore de fermeture visible) :
    // reboucle sans lire, la deadline globale tranche au pire.
  }
}

// Attend, par sondage FIONREAD SANS JAMAIS LIRE, que le nombre d'octets en
// attente côté noyau atteigne au moins `min_bytes`. Sert de barrière entre
// deux rendus déclenchés en rafale par ce test (voir le commentaire du test
// Dirty ci-dessous pour la raison précise) : sans elle, deux messages Resize
// envoyés trop rapprochés risqueraient d'être tous les deux déjà présents
// dans le tampon de réception du démon au moment où celui-ci reprend la
// main après le premier rendu — la boucle `while (dec.next())` de
// daemon.cpp les déclencherait alors TOUS LES DEUX avant la moindre
// composition, les fusionnant en un seul rendu au lieu de deux. FIONREAD
// (ioctl) rapporte le nombre d'octets actuellement en file sans en
// consommer aucun, contrairement à recv() même en MSG_PEEK suivi d'un rejet
// — même précaution de non-lecture que wait_for_peer_close ci-dessus, pour
// la même raison (ne pas perturber l'état de la file de sortie du démon,
// dont ce test dépend).
bool wait_for_avail_at_least(int fd, int min_bytes, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    int avail = 0;
    if (::ioctl(fd, FIONREAD, &avail) != 0) return false;
    if (avail >= min_bytes) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    ::usleep(20 * 1000);
  }
}

sshos::Hello make_hello(uint16_t cols, uint16_t rows) {
  sshos::Hello h;
  h.cols = cols;
  h.rows = rows;
  // term/colorterm volontairement vides : force le profil Mono16
  // (OutputProfile::detect), le moins coûteux en octets par transition SGR
  // — nécessaire aux calculs de gabarit des deux tests A7 ci-dessous.
  h.utf8 = false;
  return h;
}

// ---------------------------------------------------------------------
// Infrastructure additionnelle pour le round correctif Critique
// (integration-fix-brief.md, items 1 et 2). Les helpers ci-dessus
// (DaemonHandle, connect_retry, send_all, recv_one,
// wait_for_frame_containing) sont réutilisés tels quels ; ce qui suit est
// propre à la reproduction de ces deux défauts précis.
// ---------------------------------------------------------------------

// Fabrique un message dont l'enveloppe (tag connu + longueur déclarée)
// ment sciemment sur ce que le lecteur consomme réellement -- exactement
// ce qu'un pair hostile ou bogué ferait, sans jamais passer par
// l'encodeur légitime (encode(), proto.cpp, ne sait produire que des
// messages cohérents). Reproduit ici en dur le tag Resize = 6 (enum Tag
// anonyme de proto.cpp, non exporté) : son lecteur (proto.cpp,
// case Tag::Resize) ne consomme que 4 octets fixes (cols u16 + rows u16,
// pas de préfixe de longueur interne). En déclarant une longueur de corps
// de 5 dans l'enveloppe -- un octet de plus que ce que Resize consomme --
// r.i (4) reste différent de len (5) une fois le corps lu : exactement la
// condition documentée dans proto.cpp qui fait basculer le décodeur en
// échec permanent (Decoder::fail()), voir Decoder::next() ~ligne 258.
// C'est la reproduction directe suggérée par le rapport de tâche pour
// l'item 1 : « une Resize dont la longueur déclarée dépasse ce que le
// lecteur consomme ».
std::string malformed_known_tag_message() {
  std::string out;
  out += static_cast<char>(6);  // Tag::Resize
  // longueur déclarée du corps : 5, big-endian sur 4 octets.
  out += static_cast<char>(0);
  out += static_cast<char>(0);
  out += static_cast<char>(0);
  out += static_cast<char>(5);
  // corps : cols=80, rows=24 (les 4 octets que Resize consomme), suivis
  // d'un cinquième octet que personne ne consommera jamais.
  out += static_cast<char>(0x00);
  out += static_cast<char>(0x50);
  out += static_cast<char>(0x00);
  out += static_cast<char>(0x18);
  out += static_cast<char>(0x00);  // l'octet de trop, jamais consommé -> fail()
  return out;
}

// accept() bloquant, mais borné par un poll() préalable -- même principe
// que connect_retry() plus haut, côté acceptation plutôt que connexion.
// N'utilise délibérément PAS accept_peer() (net.hpp) : ce test-ci n'a rien
// à vérifier côté SO_PEERCRED, un accept() nu suffit à jouer le rôle d'un
// faux démon pour le test client ci-dessous.
sshos::Fd accept_with_timeout(int listen_fd, int timeout_ms) {
  pollfd pfd{listen_fd, POLLIN, 0};
  const int pr = ::poll(&pfd, 1, timeout_ms);
  if (pr <= 0) return sshos::Fd();
  const int raw = ::accept(listen_fd, nullptr, nullptr);
  if (raw < 0) return sshos::Fd();
  return sshos::Fd(raw);
}

// Fait tourner un vrai client (fork() + appel direct à run_client() dans
// le fils, sans passer par l'exécutable `sshos` -- même schéma et même
// raison d'être que DaemonHandle plus haut) avec STDIN_FILENO redirigé
// vers l'extrémité lecture d'un tube dont l'écriture reste ouverte côté
// appelant pendant toute la durée du test (jamais fermée, jamais écrite) :
// sans donnée ni EOF sur ce tube, le poll(STDIN_FILENO, sock) de
// run_client() (client.cpp) ne peut se réveiller que sur `sock` -- exactement
// ce qu'il faut pour isoler le chemin testé ci-dessous. TtyGuard::TtyGuard
// (tty_guard.cpp) échoue silencieusement son tcgetattr() sur un tube
// (ENOTTY) et reste désarmé (armed_ == false) : ni mode brut appliqué, ni
// séquence d'échappement écrite vers ce tube, ni restauration à dérouler à
// la sortie -- vérifié en lisant tty_guard.cpp (TtyGuard::TtyGuard,
// ~ligne 129 : le `return` immédiat après un tcgetattr() en échec saute
// tout le reste du constructeur, y compris write_all(fd_, tty_setup_sequence())).
class ClientHandle {
 public:
  ClientHandle(std::string socket_name, int stdin_read_fd) {
    pid_ = ::fork();
    if (pid_ == 0) {
      ::dup2(stdin_read_fd, STDIN_FILENO);
      _exit(sshos::run_client(socket_name));
    }
  }

  ~ClientHandle() {
    if (pid_ > 0) {
      // Même filet de sécurité que DaemonHandle ci-dessus, pour la même
      // raison : un test qui échoue (code d'avant le correctif de l'item 1,
      // le client gelant indéfiniment) ne doit pas laisser un processus
      // orphelin derrière lui.
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }

  ClientHandle(const ClientHandle&) = delete;
  ClientHandle& operator=(const ClientHandle&) = delete;

  bool valid() const { return pid_ > 0; }
  pid_t pid() const { return pid_; }

 private:
  pid_t pid_ = -1;
};

}  // namespace

// A7, cas Clean : un tout premier repaint qui dépasse à lui seul le
// plafond de la file de sortie (1 Mio, voir kBackpressureCeiling dans
// daemon.cpp) est rejeté avant qu'aucun octet n'ait pu partir sur le fil
// (aucun flush() n'a encore eu lieu pour cette connexion à cet instant) :
// c'est structurellement un rejet Clean, quelle que soit la taille exacte.
// Un terminal 1150x1000 force cette situation dès le message Hello — voir le
// commentaire de make_hello ci-dessus pour le choix du profil Mono16, qui
// rend le gabarit prévisible (fond bleu uniforme, un seul SGR par ligne tant
// qu'aucune écriture partielle n'a encore eu lieu). Mesuré directement avec
// le binaire réel (voir rapport de tâche) : 1 158 044 octets, ~10,4%
// au-dessus du plafond — marge délibérément modeste : sous ASan+UBSan,
// rendre et diffuser cette grille est le poste de temps dominant de ce
// test (voir rapport de tâche), un premier essai à 2800x450 (~1,26 Mio,
// ~1,26 million de cellules) le rendait de 5 à 6 fois plus lent sans gain
// de fiabilité.
//
// Le fix A7 doit laisser la connexion ouverte (branche Clean : invalidate()
// + mark_dirty(), sans drop_client()) — jamais essayé de repaint, la file de
// sortie qui vient d'être vidée par le rejet est renvoyée telle quelle. La
// preuve retenue : après avoir laissé plusieurs cycles de rejet Clean
// s'écouler sans que la connexion ne se ferme (poll() sans HUP), rétrécir le
// terminal à une taille normale doit produire une trame reçue avec succès —
// la connexion n'est donc pas seulement "pas encore fermée", elle continue
// réellement de fonctionner.
// Budget de temps : mesuré directement (voir rapport de tâche), un seul
// Session::render + Differ::frame sur cette grille prend environ 5,3 s sous
// ASan+UBSan — c'est le calcul dominant de ce test, pas un artefact du
// test lui-même. La boucle du démon étant mono-thread et synchrone, ce
// premier repaint bloque tout traitement ultérieur (y compris la lecture
// du message Resize envoyé plus bas, qui patiente sans risque dans le
// tampon de réception du démon) jusqu'à son terme.
TEST(daemon_clean_overflow_keeps_the_connection_alive) {
  const std::string name = unique_name() + "-clean";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{make_hello(1150, 1000)})));

  // Vérification de forme, pas la preuve principale : à cet instant le
  // tout premier repaint (voir ci-dessus, ~5,3 s) n'a très probablement pas
  // encore atteint son terme, donc aucun rejet Clean n'a encore eu lieu —
  // on vérifie simplement qu'il ne s'est rien passé d'anormal (pas de HUP
  // prématuré). La preuve qui compte est l'assertion finale ci-dessous :
  // recevoir une trame complète après le rétrécissement démontre que la
  // connexion n'a jamais été fermée ET qu'elle fonctionne encore.
  ::usleep(50 * 1000);

  pollfd pfd{client.get(), POLLIN, 0};
  const int pr = ::poll(&pfd, 1, 50);
  REQUIRE(pr >= 0);
  if (pr > 0) {
    CHECK((pfd.revents & (POLLHUP | POLLERR)) == 0);
  }

  // Rétrécir à une taille normale : le prochain repaint tient largement
  // sous le plafond et doit atteindre le client intact.
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{80, 24}})));

  sshos::Decoder dec;
  CHECK(wait_for_frame_containing(client.get(), dec, "ssh_os", "ssh_os", 12000));
}

// A7, cas Dirty : obtenir un rejet Dirty exige plus qu'un simple gros
// premier repaint suivi d'un second — un piège rencontré et corrigé en
// construisant ce test (voir rapport de tâche pour le détail complet du
// raisonnement, résumé ici) : OutQueue::compact() (outqueue.cpp), appelé à
// la fin de CHAQUE flush(), efface le préfixe déjà envoyé et remet off_ à
// zéro dès que ce préfixe dépasse 65536 octets (kReleaseCapacityThreshold
// n'entre pas en jeu ici, c'est le seuil `off_ > (1<<16)` de compact() qui
// commande). Or, sur cette machine, une connexion fraîche laisse passer
// environ 219 264 octets avant le premier EAGAIN — mesuré directement avec
// le vrai démon (voir rapport de tâche), bien au-delà de ce seuil de 64 Kio.
// Un scénario à DEUX repaints (le premier assez gros pour déborder à lui
// seul le tampon de réception du noyau) laisse donc TOUJOURS off_ retomber à
// zéro entre les deux : le second rejet est alors Clean, jamais Dirty,
// quelle que soit la taille choisie pour les deux trames. C'est exactement
// ce qui s'est produit lors d'un premier essai de ce test (Hello 1600x400
// suivi d'un seul Resize 1600x400) : aucune fermeture n'était jamais
// observée, même après 15 s d'attente — le rejet avait bien lieu, mais
// classé Clean (connexion gardée ouverte, repaint retenté indéfiniment),
// pas Dirty.
//
// La séquence qui marche a besoin de TROIS rendus :
//  1. Un premier repaint MODESTE (425x400, mesuré à 173 243 octets — voir
//     rapport de tâche) qui tient largement sous les ~219 264 octets que le
//     noyau accepte avant EAGAIN : il part donc ENTIÈREMENT dans son propre
//     flush() (off_ == buf_.size(), branche release_buffer() de compact(),
//     qui vide buf_ ET remet off_ à zéro) — mais laisse volontairement de la
//     place inutilisée côté noyau (~219 264 - 173 243 ≈ 46 021 octets).
//  2. Un second repaint, plus gros (1600x400, 643 243 octets), déclenché par
//     un premier Resize : son propre flush() ne peut alors envoyer QUE ce
//     reliquat de ~46 Kio avant d'essuyer EAGAIN à son tour — off_ s'arrête
//     donc à une petite valeur non nulle (≈46 Kio, sous le seuil de 64 Kio
//     de compact()), que compact() cette fois NE TOUCHE PAS (aucune de ses
//     trois branches ne s'applique : off_ != 0, off_ != buf_.size(),
//     off_ <= 65536). off_ reste ainsi collé à une valeur > 0 — c'est cet
//     état précis, capturé nulle part ailleurs, que le troisième rendu doit
//     trouver intact pour être classé Dirty.
//  3. Un troisième repaint (1500x400, 603 243 octets), déclenché par un
//     second Resize, dont le push() voit encore ~643 243 - 46 021 ≈ 597 222
//     octets non envoyés du second repaint dans buf_ : 603 243 dépasse la
//     place qu'il reste sous le plafond de 1 Mio (marge d'environ 152 000
//     octets au-delà du seuil strict — délibérément confortable plutôt que
//     tendue, pour absorber une variation raisonnable de la capacité noyau
//     mesurée). off_ étant encore > 0 à cet instant précis (capturé par
//     push() AVANT que release_buffer() ne le remette à zéro — voir le
//     commentaire de take_overflow(), outqueue.hpp), ce rejet est Dirty.
//
// Entre l'étape 2 et l'étape 3, une simple pause fixe ne suffit pas : si le
// second Resize (étape 3) arrivait alors que le démon n'a pas encore fini de
// composer/pousser le second repaint (étape 2), les DEUX messages Resize
// seraient décodés dans la MÊME passe de la boucle `while (dec.next())` de
// daemon.cpp — un seul rendu final serait composé (celui du dernier Resize
// lu), fusionnant les étapes 2 et 3 et cassant tout le raisonnement
// ci-dessus. wait_for_avail_at_least() (définie plus haut) sert de barrière
// non intrusive : elle attend, par FIONREAD, que le nombre d'octets en
// attente dépasse 173 243 (le total de l'étape 1 seule) avant d'envoyer le
// second Resize — signe fiable que le flush() de l'étape 2 a eu lieu,
// puisque FIONREAD ne peut croître qu'après que le démon a réellement
// écrit des octets supplémentaires sur le fil.
//
// Le fix A7 doit fermer la connexion dans ce cas (drop_client(nullptr)) :
// c'est la seule preuve retenue ici, par détection de fermeture (voir
// wait_for_peer_close ci-dessus pour la raison de ne jamais lire).
//
// Budget de temps : la boucle du démon est mono-thread et synchrone — les
// trois rendus sont sérialisés, bloquants, et leur coût est dominé par le
// nombre de cellules (mesuré directement, voir rapport de tâche : environ
// 0,8 s + 2,9 s + 2,7 s sous ASan+UBSan, soit ~6,4 s au total avant la
// fermeture). Les délais ci-dessous gardent une marge confortable plutôt que
// de viser au plus juste.
TEST(daemon_dirty_overflow_closes_the_connection) {
  const std::string name = unique_name() + "-dirty";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  // Réduit AVANT le moindre octet applicatif : la file de réception du
  // noyau pour cette connexion doit déjà être à sa taille plancher quand le
  // démon commencera à pousser le premier repaint.
  int rcvbuf = 1024;
  REQUIRE(::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                       sizeof rcvbuf) == 0);

  // Étape 1 : petit repaint (173 243 octets), part entièrement — voir le
  // commentaire ci-dessus pour la raison de ce choix précis de gabarit.
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{make_hello(425, 400)})));
  // N'a pas besoin de couvrir la durée du premier repaint : le démon
  // sérialise son traitement (un seul thread, une seule boucle epoll), donc
  // le message Resize envoyé ci-dessous patiente simplement dans le tampon
  // de réception du démon jusqu'à ce que le premier repaint soit terminé —
  // aucune perte ni réordonnancement possible. Cette petite pause n'est
  // qu'un espacement de forme entre les deux envois.
  ::usleep(50 * 1000);

  // Étape 2 : gros repaint (643 243 octets) qui ne peut partir qu'à
  // hauteur du reliquat de place noyau laissé par l'étape 1, y laissant
  // off_ collé à une petite valeur non nulle.
  REQUIRE(
      send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{1600, 400}})));

  // Barrière : attendre que l'étape 2 ait réellement fini d'être poussée
  // avant d'envoyer le Resize de l'étape 3 — voir le commentaire ci-dessus.
  // Le seuil doit être STRICTEMENT supérieur aux 173 243 octets de l'étape 1
  // seule : wait_for_avail_at_least() teste `avail >= min_bytes`, et avail
  // vaut déjà exactement 173 243 dès la fin de l'étape 1, avant même l'envoi
  // du Resize ci-dessus — un seuil de 173243 serait donc trivialement déjà
  // atteint, ne prouvant rien sur l'étape 2 (bogue effectivement rencontré :
  // sous la build Release, sans le ralentissement d'ASan/UBSan, ce seuil
  // trivial laissait passer le Resize de l'étape 3 quasi immédiatement,
  // avant que le moindre octet de l'étape 2 n'ait été poussé — les deux
  // Resize se retrouvaient alors décodés dans la même passe de
  // `dec.next()`, fusionnant les étapes 2 et 3 en un seul rendu et cassant
  // le calcul d'octets ci-dessus ; voir rapport de tâche). 174243 (173 243 +
  // 1000 de marge) est le seuil validé par la maquette autonome
  // (repro_dirty2.cpp, voir rapport de tâche) : tout dépassement de ce seuil
  // ne peut venir que de l'étape 2.
  // Le seuil est un ORDRE DE GRANDEUR, pas une mesure : il doit seulement
  // prouver que l'etape 2 a commence a partir, donc depasser franchement
  // ce que l'etape 1 seule laisse dans le tampon. Il etait ecrit a l'octet
  // pres (174 243), ce qui le rendait solidaire du CONTENU du bureau --
  // ajouter des cadres au fond d'ecran suffisait a le faire tomber.
  REQUIRE(wait_for_avail_at_least(client.get(), 200000, 12000));

  // Étape 3 : ce repaint (603 243 octets) doit déborder le plafond compte
  // tenu du reliquat non vidé de l'étape 2, avec off_ encore > 0 — Dirty.
  REQUIRE(
      send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{1500, 400}})));

  CHECK(wait_for_peer_close(client.get(), 12000));
}

// Le test bout-en-bout exigé par la tâche : un vrai démon, un vrai client
// sur socket abstrait, un aller Hello/Welcome, une trame reçue contenant le
// titre de la fenêtre ("Bloc") et le texte du panneau ("ssh_os"), puis
// un arrêt propre (SIGTERM, comme `sshos --kill`) vérifié par le code de
// sortie ET par la disparition du socket abstrait. Les étapes 12 et 14 du
// plan (lancement interactif, Ctrl+Q sous un vrai tty) ne sont pas
// exécutables sans tty et ne sont PAS couvertes ici — voir le rapport de
// tâche. L'étape 13 (`setsid --fork`) est exécutée séparément, à la main,
// hors de ce binaire de test.
TEST(end_to_end_attach_render_detach_kill) {
  const std::string name = unique_name() + "-e2e";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  CHECK(std::holds_alternative<sshos::Welcome>(*welcome));

  CHECK(wait_for_frame_containing(client.get(), dec, "Bloc", "ssh_os", 2000));

  // Détachement côté client : fermer notre bout ne doit rien perturber côté
  // démon (le bouchon M1 n'a qu'un seul client à la fois, sans notion de
  // session détachée séparément du processus démon — voir Session).
  client.reset();

  // Arrêt comme le ferait `sshos --kill` : SIGTERM, pas SIGKILL.
  REQUIRE(::kill(daemon.pid(), SIGTERM) == 0);

  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(daemon.pid(), &status, WNOHANG);
    if (r == daemon.pid()) {
      exited = true;
      break;
    }
    ::usleep(10 * 1000);
  }
  REQUIRE(exited);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);

  // Le socket abstrait doit avoir disparu avec le démon : s'y connecter
  // après coup doit échouer, pas trouver un écouteur fantôme.
  bool threw = false;
  try {
    sshos::Fd probe = sshos::connect_abstract(name);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

// ---------------------------------------------------------------------
// Rond 1 du correctif Critique : le démon ne doit plus jamais scruter
// activement (voir daemon.cpp ~ligne 125-140 et FrameClock::reset() dans
// frameclock.hpp pour le mécanisme retenu, et le rapport de tâche pour le
// diagnostic complet). Les deux tests qui suivent couvrent respectivement
// le symptôme régressé lui-même (boucle active mesurée en temps CPU réel)
// et le chemin de câblage qui n'était couvert par aucun test existant
// (Input reçu sur le fil jusqu'à Session::on_input()).
// ---------------------------------------------------------------------

namespace {

// Lit utime+stime (champs 14 et 15, indices 1-based du format proc(5)) du
// processus pid, en jiffies noyau -- même idiome que extract_field() dans
// tests/test_daemonize.cpp pour lire /proc/*, adapté ici au format
// espace-séparé de /proc/*/stat plutôt qu'au format « label: valeur » de
// /proc/*/status. Le champ comm (2e champ, entre parenthèses) peut contenir
// des espaces ou des parenthèses : on repère la DERNIÈRE ')' de la ligne
// plutôt que de découper naïvement depuis le début -- même précaution que
// documentée dans le rapport de tâche.
long read_cpu_ticks(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  const std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  const auto paren = content.rfind(')');
  if (paren == std::string::npos || paren + 2 > content.size()) return -1;
  std::istringstream rest(content.substr(paren + 2));
  std::vector<std::string> fields;
  std::string field;
  while (rest >> field) fields.push_back(field);
  if (fields.size() < 13) return -1;
  return std::stol(fields[11]) + std::stol(fields[12]);
}

}  // namespace

// Test discriminant exigé par la tâche : observe directement le symptôme
// régressé (consommation CPU réelle du VRAI processus démon), pas
// seulement l'absence de plantage. Reproduit le scénario exact du rapport
// de tâche : un client envoie un seul message Resize, SANS jamais envoyer
// Hello, puis se déconnecte. Contre le code d'avant ce correctif (avant le
// rond 1), ce test échoue -- mesuré directement en repartant du commit de
// base de cette tâche (voir rapport de tâche pour les chiffres complets,
// de l'ordre de 100 % d'un cœur) ; contre le code corrigé, il passe (0
// jiffie mesuré sur la même machine).
//
// Laisser le démon se réveiller et lire les 9 octets AVANT de fermer le
// socket client est délibéré, pas accessoire : un close() immédiat ferait
// courir la fermeture contre epoll_wait() du démon, et le noyau peut alors
// signaler EPOLLIN|EPOLLHUP en une seule fois. daemon.cpp traite EPOLLHUP
// avant EPOLLIN (continue anticipé, ~ligne 217) : les octets ne seraient
// alors jamais lus ni décodés, donc mark_dirty() jamais appelé, donc le
// bogue jamais exercé -- piège rencontré et documenté dans le rapport de
// tâche lors de la reproduction manuelle de ce scénario.
//
// Le seuil est délibérément lâche plutôt que serré : sysconf(_SC_CLK_TCK)
// convertit la fenêtre de mesure en un budget de jiffies, et le seuil ne
// retient qu'un quart de ce que consommerait un cœur occupé en continu sur
// cette même fenêtre -- alors qu'une boucle active en consomme ~100 %.
// Cette marge reste confortable sous ASan+UBSan (Debug) comme en Release,
// y compris sur une machine chargée, sans jamais supposer une fréquence
// d'horloge noyau particulière (délibérément pas de 100 Hz codé en dur).
TEST(daemon_does_not_busy_loop_after_resize_without_hello) {
  const std::string name = unique_name() + "-busyloop";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  const long ticks_per_sec = ::sysconf(_SC_CLK_TCK);
  REQUIRE(ticks_per_sec > 0);

  // Fenêtre de mesure et seuil : voir commentaire ci-dessus.
  constexpr int kSampleMs = 600;
  const long budget_if_pegged = ticks_per_sec * kSampleMs / 1000;
  const long threshold = std::max<long>(1, budget_if_pegged / 4);

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  REQUIRE(
      send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{80, 24}})));
  // Voir le commentaire au-dessus du test pour la raison de ce délai avant
  // fermeture.
  ::usleep(100 * 1000);

  // Phase 1 : le client sonde reste connecté (fd toujours ouvert côté
  // test), comme dans la mesure « après Resize seul, sonde déconnectée »
  // du rapport de tâche -- nommée ainsi côté démon parce que rien n'a
  // encore été lu de plus sur cette connexion, pas parce que le socket est
  // fermé à cet instant précis.
  const long before1 = read_cpu_ticks(daemon.pid());
  REQUIRE(before1 >= 0);
  ::usleep(kSampleMs * 1000);
  const long after1 = read_cpu_ticks(daemon.pid());
  REQUIRE(after1 >= 0);
  CHECK(after1 - before1 < threshold);

  // Phase 2 : le client se déconnecte réellement -- le cas que le rapport
  // de tâche isole explicitement (dirty_ resté vrai côté démon, plus aucun
  // client pour jamais le consommer). Le correctif doit couvrir aussi bien
  // la connexion encore ouverte que sa disparition ; voir
  // FrameClock::reset() et son appel dans drop_client() (daemon.cpp).
  client.reset();
  ::usleep(50 * 1000);

  const long before2 = read_cpu_ticks(daemon.pid());
  REQUIRE(before2 >= 0);
  ::usleep(kSampleMs * 1000);
  const long after2 = read_cpu_ticks(daemon.pid());
  REQUIRE(after2 >= 0);
  CHECK(after2 - before2 < threshold);
}

// Second test exigé par la tâche : jusqu'ici, aucun test ne passait par le
// chemin réel Input sur le fil -> dec.next() -> client->input.feed() ->
// InputParser::next() -> session.on_input() (daemon.cpp ~ligne 263-266) --
// le seul test existant pour Ctrl+Q (session_quits_on_ctrl_q, plus haut)
// appelle Session::on_input() directement, court-circuitant exactement la
// zone qui portait le bogue Critique du rond 1 (mark_dirty() y est appelé
// sans garde, tout comme dans la branche Resize). Celui-ci envoie un VRAI
// message Input encodé sur un VRAI socket vers un VRAI démon, et observe un
// effet côté démon impossible à obtenir autrement qu'en traversant tout ce
// chemin : l'arrêt spontané du démon (wants_quit() -> running = false ->
// sortie propre de run_daemon()), SANS le moindre SIGTERM envoyé par ce
// test -- seul le Ctrl+Q décodé depuis l'octet brut 0x11 peut produire cet
// effet (InputParser::step(), input/parser.cpp : tout octet < 0x20 devient
// Ctrl+lettre, ici 'a' + 0x11 - 1 = 'q').
//
// Nature discriminante vérifiée directement (voir rapport de tâche) : en
// commentant temporairement l'appel à session.on_input() dans la boucle
// `while (auto e = client->input.next())` de daemon.cpp (tout en laissant
// InputParser::feed()/next() intacts, pour isoler précisément le dernier
// maillon du chemin), ce test échoue (délai de 2 s épuisé, le démon ne
// sort jamais de lui-même) alors que session_quits_on_ctrl_q continue de
// passer sans changement -- preuve que les deux tests ne couvrent pas la
// même zone, et que celui-ci couvre bien le maillon jusque-là non testé.
TEST(daemon_quits_when_the_menu_asks_for_it_over_the_wire) {
  const std::string name = unique_name() + "-wire-input";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  CHECK(std::holds_alternative<sshos::Welcome>(*welcome));

  // « Fermer la session » du menu, tapée sur le fil : c'est le seul chemin
  // qui arrête le démon (Ctrl+Q détache). Elle DEMANDE désormais, d'où la
  // tabulation qui va chercher la confirmation puis l'Entrée qui la donne
  // -- une Entrée seule tombe sur « annuler », et c'est le point.
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{sshos::Input{
                                     "\x01 fermer\r\t\r"}})));

  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(daemon.pid(), &status, WNOHANG);
    if (r == daemon.pid()) {
      exited = true;
      break;
    }
    ::usleep(10 * 1000);
  }
  REQUIRE(exited);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);
}

// ---------------------------------------------------------------------
// A2 (couverture au meilleur effort, cf. rapport de tâche) : le garde-fou
// anti-réutilisation de descripteur au sein d'un même lot epoll
// (closed_this_batch, daemon.cpp ~lignes 103/112/117/208-231) n'avait
// jusqu'ici aucune couverture -- remplacer sa condition par `if (false)`
// laissait passer toute la suite.
//
// Un test a été tenté pour forcer le scénario que le garde-fou est censé
// prévenir : un client A enregistré auprès d'epoll sous un descripteur X,
// fermé PENDANT que le démon est gelé (SIGSTOP), en même temps qu'un
// client B se connecte (l'écouteur devient prêt) -- de façon à ce qu'un
// seul epoll_wait() rende les deux évènements dans le MÊME lot. Une
// strace du démon (voir rapport de tâche) confirme que ce montage
// fonctionne et produit bien une VRAIE réutilisation de X : quand
// l'évènement de A (HUP) est traité avant celui de l'écouteur, drop_client
// ferme X, puis accept4() -- appelé juste après, dans le traitement de
// l'évènement de l'écouteur -- se voit effectivement redonner X par le
// noyau (plus petit numéro libre).
//
// Mais ce même strace montre aussi pourquoi ce test ne peut pas
// discriminer le garde-fou avec la boucle telle qu'elle existe
// aujourd'hui : epoll_wait() ne rend jamais deux entrées pour le même
// numéro de descripteur au sein d'un seul appel (chaque descripteur prêt
// n'apparaît qu'une fois par lot), et la boucle du démon n'accepte qu'UNE
// connexion par tour de la table des évènements (un seul client à la
// fois : `client` est un pointeur unique, pas une collection). L'entrée
// périmée que closed_this_batch est censé filtrer -- une DEUXIÈME
// référence à X, postérieure à sa réutilisation, dans le MÊME evs[] --
// n'a donc structurellement aucune occasion d'exister : dès que
// l'évènement de A a été consommé (une seule fois), il ne reste plus rien
// à filtrer pour ce numéro dans ce lot. Ceci a été vérifié dans les DEUX
// ordres d'arrivée possibles (écouteur avant X, et X avant écouteur) --
// aucun des deux ne produit de doublon. Remplacer la condition du
// garde-fou par `if (false)` ne fait donc échouer ni la suite existante
// ni le test ci-dessous, qui pourtant force la coexistence des deux
// évènements dans un seul lot ET la réutilisation réelle du descripteur :
// ce n'est pas un défaut du test, la fonctionnalité qui rendrait le
// garde-fou atteignable (vidage de plusieurs connexions en attente par
// tour, ou plusieurs clients simultanés) n'existe simplement pas encore
// dans ce jalon. Voir le rapport de tâche pour le détail des deux
// montages strace qui ont mené à cette conclusion.
//
// Le test est conservé malgré cette absence de discrimination : il
// couvre un scénario réel et non trivial (relais vers un nouveau client
// dont le descriptor réutilise EXACTEMENT le numéro de l'ancien, y
// compris le epoll_ctl DEL/ADD et le protocole applicatif qui suit), et
// sa valeur de non-régression pour ce chemin de relais reste entière même
// si le garde-fou n'est, pour l'instant, prouvé nécessaire par aucun test
// boîte noire.
// Le hors-bande et le repeint force ne traversent le demon que par le flux
// de trames : ni l'un ni l'autre n'a de message de protocole. Ces deux cas
// sont les seuls qui exercent ce cablage de bout en bout.
TEST(daemon_carries_the_mouse_toggle_ahead_of_the_frame) {
  const std::string name = unique_name() + "-oob";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());
  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());

  // Ctrl+A puis 'm' : la bascule souris (spec §7.4). \x01 est Ctrl+A sur
  // le fil.
  REQUIRE(send_all(client.get(),
                   sshos::encode(sshos::Msg{sshos::Input{"\x01m"}})));
  CHECK(wait_for_frame_containing(client.get(), dec, "\033[?1002l",
                                  "\033[?1006l", 3000));
}

TEST(daemon_repaints_everything_when_the_desktop_asks_for_it) {
  const std::string name = unique_name() + "-repaint";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());
  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  REQUIRE(wait_for_frame_containing(client.get(), dec, "Bloc", "ssh_os", 3000));

  // Ctrl+A puis 'r'. RIEN n'a change a l'ecran : sans invalidate() le delta
  // serait vide, et seule une trame complete peut porter a la fois le titre
  // de la fenetre et le texte du panneau.
  REQUIRE(send_all(client.get(),
                   sshos::encode(sshos::Msg{sshos::Input{"\x01r"}})));
  CHECK(wait_for_frame_containing(client.get(), dec, "Bloc", "ssh_os", 3000));
}

TEST(daemon_handles_client_takeover_with_reused_fd_in_one_epoll_batch) {
  const std::string name = unique_name() + "-fdreuse";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client_a = connect_retry(name);
  REQUIRE(client_a.valid());

  // Laisse le temps au démon d'accepter A et de l'enregistrer auprès
  // d'epoll (accept4() + epoll_ctl(ADD), synchrone et rapide dans sa
  // boucle) avant de le geler ci-dessous -- sans quoi SIGSTOP pourrait le
  // figer avant même que A ne soit un client epoll à part entière.
  ::usleep(100 * 1000);

  REQUIRE(::kill(daemon.pid(), SIGSTOP) == 0);

  // Pendant que le démon est gelé (aucune progression possible sur aucun
  // appel système, quel qu'il soit -- c'est ce qui rend déterministe la
  // coexistence des deux évènements dans un seul lot, malgré l'ordre
  // normalement non spécifié d'epoll_wait()) : A ferme d'abord son
  // descripteur X (devient prêt en HUP), PUIS B se connecte (l'écouteur
  // devient prêt à son tour). Cet ordre précis est celui qui, vérifié par
  // strace, fait effectivement réutiliser X par accept4() pour B --
  // l'ordre inverse (écouteur avant X) laisse le noyau attribuer un
  // numéro neuf à B et n'exerce donc rien d'intéressant.
  client_a.reset();
  sshos::Fd client_b = connect_retry(name);
  REQUIRE(client_b.valid());

  REQUIRE(::kill(daemon.pid(), SIGCONT) == 0);

  // Preuve retenue : B doit survivre intact et rester pleinement
  // fonctionnel après ce relais à descripteur réutilisé -- pas seulement
  // « pas immédiatement fermé ». Un aller Hello/Welcome complet le
  // démontre.
  REQUIRE(send_all(client_b.get(),
                    sshos::encode(sshos::Msg{make_hello(80, 24)})));
  sshos::Decoder dec;
  auto welcome = recv_one(client_b.get(), dec, 3000);
  REQUIRE(welcome.has_value());
  CHECK(std::holds_alternative<sshos::Welcome>(*welcome));
}

// ---------------------------------------------------------------------
// Round correctif Critique (jalon 1, intégration -- voir
// .superpowers/sdd/2026-08-10-ssh-os-m1-noyau/integration-fix-brief.md).
// Deux défauts, chacun couvert ci-dessous par un test qui échoue contre le
// code d'avant son propre correctif -- voir le rapport de tâche pour la
// sortie exacte obtenue en retirant temporairement chaque correctif.
// ---------------------------------------------------------------------

// Item 1, volet démon : Decoder::failed() (proto.hpp) documentait une
// obligation -- « à charge pour l'appelant de la fermer » -- que personne
// ne respectait. Une fois failed_ vrai, feed() n'accumule plus rien et
// next() ne renvoie plus jamais rien (proto.cpp) : plus aucun signal ne
// distingue alors « rien à lire pour l'instant » de « ce pair ne parlera
// plus jamais correctement » -- la connexion gelait en silence, pour
// toujours, sans jamais être fermée. Ce test force ce scénario côté
// démon : un Hello/Welcome normal, puis un message à tag connu (Resize)
// dont le corps ment sur sa propre longueur (voir
// malformed_known_tag_message() ci-dessus).
TEST(daemon_closes_connection_on_malformed_message_instead_of_freezing) {
  const std::string name = unique_name() + "-malformed";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  CHECK(std::holds_alternative<sshos::Welcome>(*welcome));

  REQUIRE(send_all(client.get(), malformed_known_tag_message()));

  // Preuve retenue : la connexion doit se FERMER, pas geler indéfiniment.
  // Contre le code d'avant le correctif de l'item 1, ce CHECK échoue par
  // expiration du délai (le décodeur du démon reste en échec permanent,
  // mais rien n'appelle jamais drop_client() pour autant).
  CHECK(wait_for_peer_close(client.get(), 3000));
}

// Item 1, volet client : le même défaut existait aussi dans client.cpp
// (voir son commentaire « Item 1 », juste après la boucle
// `while (auto m = dec.next())` de run_client()). run_client() est un vrai
// processus (fork() + appel direct, même schéma que DaemonHandle),
// connecté à un faux démon (ce test lui-même, via un accept() nu -- voir
// accept_with_timeout() ci-dessus) qui envoie un Hello/Welcome légitime
// PUIS le même message corrompu que le test précédent. Sans le correctif,
// le client gèlerait indéfiniment sur un décodeur en échec permanent au
// lieu de sortir avec rc=1.
TEST(client_exits_on_malformed_message_instead_of_freezing) {
  const std::string name = unique_name() + "-client-malformed";

  sshos::Fd listener = sshos::bind_abstract(name);
  REQUIRE(listener.valid());

  int pipefd[2];
  REQUIRE(::pipe(pipefd) == 0);
  // stdin_write n'est délibérément ni fermé ni écrit avant la fin de ce
  // test -- voir le commentaire de ClientHandle ci-dessus.
  sshos::Fd stdin_write(pipefd[1]);
  sshos::Fd stdin_read(pipefd[0]);

  ClientHandle client(name, stdin_read.get());
  REQUIRE(client.valid());

  sshos::Fd peer = accept_with_timeout(listener.get(), 3000);
  REQUIRE(peer.valid());

  sshos::Decoder server_dec;
  auto hello_msg = recv_one(peer.get(), server_dec, 2000);
  REQUIRE(hello_msg.has_value());
  CHECK(std::holds_alternative<sshos::Hello>(*hello_msg));

  REQUIRE(send_all(peer.get(), sshos::encode(sshos::Msg{sshos::Welcome{}})));
  REQUIRE(send_all(peer.get(), malformed_known_tag_message()));

  // Preuve retenue : le PROCESSUS client doit sortir (rc=1), pas geler.
  // Contre le code d'avant le correctif, ce REQUIRE échoue par expiration
  // du délai (le processus ne sort jamais de lui-même).
  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(client.pid(), &status, WNOHANG);
    if (r == client.pid()) {
      exited = true;
      break;
    }
    ::usleep(10 * 1000);
  }
  REQUIRE(exited);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 1);
}

// Item 2 : reproduction directe du symptôme du rapport de tâche -- une
// simple sonde (--status, --kill, la sonde d'attache initiale de
// main.cpp) qui ouvre une connexion puis la referme SANS JAMAIS RIEN
// ÉCRIRE évinçait le client attaché avant ce correctif (case
// AcceptOutcome::Accepted de daemon.cpp appelait drop_client() sans
// condition, avant même que la nouvelle connexion n'ait décliné une
// quelconque intention). Ce test attache un vrai client (Hello/Welcome
// complet, une première trame reçue), ouvre puis referme un second socket
// sans y écrire le moindre octet, et vérifie que le premier reste attaché
// (pas de HUP) ET reste pleinement fonctionnel (un Resize qu'il envoie
// ensuite doit encore produire une trame reçue).
TEST(daemon_mute_probe_does_not_evict_the_attached_client) {
  const std::string name = unique_name() + "-muteprobe";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  CHECK(std::holds_alternative<sshos::Welcome>(*welcome));

  CHECK(wait_for_frame_containing(client.get(), dec, "Bloc", "ssh_os", 2000));

  // La sonde muette : ouvre puis referme, sans écrire un seul octet --
  // exactement --status/--kill/la sonde d'attache initiale de main.cpp.
  {
    sshos::Fd probe = connect_retry(name);
    REQUIRE(probe.valid());
  }  // fermeture ici, sans avoir rien envoyé

  // Laisse au démon le temps de traiter l'accept puis le HUP de la sonde
  // (deux tours d'epoll au plus, synchrones et rapides) avant de vérifier
  // que le premier client n'a rien vu passer.
  ::usleep(100 * 1000);

  // Preuve directe : le premier client n'a jamais reçu de HUP. Contre le
  // code d'avant ce correctif, la sonde évince immédiatement le client
  // (drop_client() inconditionnel à l'accept) : ce CHECK échoue.
  pollfd pfd{client.get(), POLLIN, 0};
  const int pr = ::poll(&pfd, 1, 200);
  REQUIRE(pr >= 0);
  if (pr > 0) {
    CHECK((pfd.revents & (POLLHUP | POLLERR)) == 0);
  }

  // Preuve fonctionnelle : il reste pleinement attaché, pas seulement
  // « pas encore fermé ». Contre le code d'avant ce correctif, ce CHECK
  // échoue aussi (la connexion a déjà été fermée par la sonde précédente,
  // wait_for_frame_containing rend faux dès le premier recv_one en échec).
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{100, 30}})));
  CHECK(wait_for_frame_containing(client.get(), dec, "ssh_os", "ssh_os", 3000));
}

namespace {

// kill(SIGSTOP) rend la main AVANT que la cible ne soit effectivement
// arrêtée : l'arrêt est asynchrone. Écrire dans la foulée laisse donc au
// démon une fenêtre courte mais bien réelle pour drainer l'EPOLLIN de
// lui-même avant de se figer — auquel cas les octets sont traités, et le test
// passe alors même que le code est défectueux. Ce n'est pas une crainte
// théorique : mesuré sur le test des clics ci-dessous, un simple kill() suivi
// d'écritures immédiates ne faisait échouer le code d'avant que 5 fois sur
// 10. Attendre l'état « arrêté » rend la coalescence déterministe et porte
// les trois cas de ce round à 20 échecs sur 20 contre le code d'avant.
//
// L'état vit dans /proc/<pid>/stat, champ 3 : 'T' = arrêté. Le processus est
// désigné par son pid, jamais par correspondance de nom — `ps` et `pgrep -f`
// matchent leur propre ligne de commande (piège rencontré trois fois sur ce
// projet, dont un « défaut reproduit » entièrement faux). Le champ comm
// pouvant contenir espaces et parenthèses, on repart de la DERNIÈRE ')'.
bool wait_until_stopped(pid_t pid, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (f && std::getline(f, line)) {
      const size_t rp = line.rfind(')');
      // ") T ..." : l'état est le premier caractère non blanc après ')'.
      if (rp != std::string::npos && rp + 2 < line.size() && line[rp + 2] == 'T') {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    ::usleep(2 * 1000);
  }
}

}  // namespace

// ---------------------------------------------------------------------
// Round EPOLLHUP / drainage (voir docs/hup-drain-brief.md).
//
// Symptôme utilisateur : cliquer puis fermer la fenêtre du terminal dans
// la même fraction de seconde perd les tout derniers messages. Cause : le
// répartiteur du démon honorait EPOLLHUP|EPOLLERR et faisait `continue`
// AVANT d'avoir drainé EPOLLIN. Or epoll_wait() coalesce couramment les
// deux bits dans un SEUL évènement quand le pair écrit puis ferme aussitôt
// derrière : à cet instant les octets sont déjà dans le tampon de
// réception du socket, et les jeter sans les lire est précisément la perte
// observée — alors même que la session du démon, elle, survit au
// détachement (c'est tout l'intérêt du démon).
//
// Ce test n'observe que du comportement de bout en bout : un Ctrl+Q envoyé
// juste avant la fermeture doit encore arrêter le démon. Il ne dit rien de
// l'ordre interne des blocs du répartiteur, afin de rester valable quelle
// que soit la façon dont le correctif s'y prend.
//
// SIGSTOP rend la coalescence déterministe au lieu de seulement probable :
// gelé, le démon ne peut pas se réveiller sur l'EPOLLIN seul avant que la
// fermeture ne survienne, donc les deux bits sont forcément posés ensemble
// quand il reprend la main. Même montage que
// daemon_handles_client_takeover_with_reused_fd_in_one_epoll_batch
// ci-dessus, dont une strace avait déjà confirmé le principe. SIGCONT peut
// faire rendre EINTR à epoll_wait() : la boucle du démon le traite déjà par
// un `continue` (daemon.cpp ~ligne 220) et repasse en attente sans perdre
// les bits déjà posés.
TEST(daemon_processes_input_sent_just_before_the_client_closes) {
  const std::string name = unique_name() + "-hup-drain";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{hello})));

  // Point de synchronisation, pas un délai au hasard : Welcome n'est poussé
  // qu'APRÈS `client = std::move(pending)` (daemon.cpp), donc le recevoir
  // prouve que cette connexion est bien le `client` attaché. C'est sa
  // branche du répartiteur que ce test vise, pas celle de `pending`.
  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 2000);
  REQUIRE(welcome.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome));

  REQUIRE(::kill(daemon.pid(), SIGSTOP) == 0);

  // Aucun REQUIRE entre le gel et la reprise : son `return` nu laisserait le
  // démon figé. On relève donc les résultats dans des booléens et on ne
  // tranche qu'après SIGCONT.
  const bool stopped = wait_until_stopped(daemon.pid(), 2000);
  const bool sent = send_all(  // « Quitter la session » au menu
      client.get(),
      sshos::encode(sshos::Msg{sshos::Input{"\x01 fermer\r\t\r"}}));
  // Fermeture immédiate : le FIN suit les octets sur le même socket, sans
  // laisser au démon la moindre occasion de se réveiller entre les deux.
  client.reset();
  const bool resumed = ::kill(daemon.pid(), SIGCONT) == 0;

  REQUIRE(stopped);
  REQUIRE(sent);
  REQUIRE(resumed);

  // Preuve retenue : le démon s'arrête, donc le Ctrl+Q a bien traversé le
  // décodeur et la session malgré la fermeture simultanée. Contre le code
  // d'avant le correctif, l'octet est jeté avec le HUP : le démon reste
  // vivant et cette attente expire.
  int status = 0;
  bool exited = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(daemon.pid(), &status, WNOHANG);
    if (r == daemon.pid()) {
      exited = true;
      break;
    }
    ::usleep(10 * 1000);
  }
  // REQUIRE et non CHECK : sans sortie constatée, `status` ne veut rien dire
  // et WIFEXITED(0) passerait tout seul, maquillant l'échec en succès.
  REQUIRE(exited);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);
}

// Même round, chemin d'observation exactement celui que réclamait le brief :
// le symptôme tel que l'utilisateur le vit. Un client clique, ferme la fenêtre
// du terminal aussitôt, se rattache — et doit retrouver ses clics. Le test
// ci-dessus prouve la même garde par la sortie du démon (observable binaire,
// insensible au rendu) ; celui-ci prouve qu'il en reste quelque chose dans
// l'ÉTAT DE SESSION, ce qui est la promesse même du démon.
//
// Le second client est indispensable, pas décoratif : le protocole est
// différentiel, un client déjà attaché ne reçoit que les cellules modifiées.
// Seule une connexion neuve reçoit un repeint complet, donc une trame où
// « clics: 3 » figure en toutes lettres.
TEST(daemon_keeps_clicks_sent_just_before_the_client_closes) {
  const std::string name = unique_name() + "-hup-clicks";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd a = connect_retry(name);
  REQUIRE(a.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec_a;
  auto welcome = recv_one(a.get(), dec_a, 2000);
  REQUIRE(welcome.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome));

  // Référence avant tout clic, et point de synchronisation : attendre une
  // trame effectivement composée garantit que le démon est retombé en attente
  // dans epoll_wait() avant qu'on ne le gèle.
  REQUIRE(wait_for_frame_containing(a.get(), dec_a, "clics: 0", "ssh_os", 3000));

  REQUIRE(::kill(daemon.pid(), SIGSTOP) == 0);

  // Trois pressions SGR (\033[<0;C;LM), puis fermeture immédiate — sans jamais
  // laisser au démon l'occasion de se réveiller entre les octets et le FIN.
  // Aucun REQUIRE dans cette fenêtre : voir le test précédent.
  const bool stopped = wait_until_stopped(daemon.pid(), 2000);
  bool sent = true;
  for (int i = 0; i < 3; ++i) {
    sent = sent && send_all(a.get(), sshos::encode(sshos::Msg{sshos::Input{
                                "\033[<0;10;5M"}}));
  }
  a.reset();
  const bool resumed = ::kill(daemon.pid(), SIGCONT) == 0;

  REQUIRE(stopped);
  REQUIRE(sent);
  REQUIRE(resumed);

  // Rattache : client neuf, donc repeint complet.
  sshos::Fd b = connect_retry(name);
  REQUIRE(b.valid());
  REQUIRE(send_all(b.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec_b;
  auto welcome_b = recv_one(b.get(), dec_b, 3000);
  REQUIRE(welcome_b.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome_b));

  // Contre le code d'avant le correctif, les trois pressions sont parties à la
  // poubelle avec le HUP : la session affiche « clics: 0 » et cette attente
  // expire.
  CHECK(wait_for_frame_containing(b.get(), dec_b, "clics: 3", "ssh_os", 3000));
}

// Le scénario tel que l'utilisateur le vit, et la seule chose que ce
// programme promet vraiment : je pars, je reviens, tout est là. Rien ne le
// vérifiait de bout en bout -- les tests de rattachement existants portaient
// sur un client TUÉ, jamais sur un départ volontaire, et c'est précisément
// par là que le défaut est passé : Ctrl+Q, unique geste de sortie offert,
// détruisait la session au lieu de congédier le client.
//
// Trois observables, dans l'ordre où elles se produisent :
//   1. le client partant reçoit Detached -- le démon le congédie, il ne meurt
//      pas avec lui ;
//   2. le démon est toujours vivant après ;
//   3. le rattachement retrouve la fenêtre Battement ouverte avant le départ.
// Contre le code d'avant le correctif, (1) échoue déjà : le démon s'arrête,
// le socket se ferme sans annonce.
TEST(daemon_keeps_the_desktop_across_a_voluntary_detach) {
  const std::string name = unique_name() + "-detach";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd a = connect_retry(name);
  REQUIRE(a.valid());
  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec_a;
  auto welcome = recv_one(a.get(), dec_a, 2000);
  REQUIRE(welcome.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome));

  // Ouvre Battement par le menu, et attend de le VOIR : sans ce point de
  // synchronisation, le Ctrl+Q pourrait doubler l'ouverture.
  REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{sshos::Input{
                                "\x01 fich\r"}})));
  // Puis un repeint complet forcé (<leader>r) AVANT de relever quoi que ce
  // soit. Sans lui la trame est un delta, et un delta ne réémet que les
  // cellules changées : la première tentative de ce test cherchait un motif
  // dont une lettre se trouvait déjà là, héritée de la fenêtre dessous, et
  // le différentiel n'envoyait que le reste. Un motif ne survit à un delta
  // que par chance ; le repeint retire la chance de l'équation.
  REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{sshos::Input{"\x01r"}})));
  REQUIRE(wait_for_frame_containing(a.get(), dec_a, "Fichiers", "ssh_os", 3000));

  REQUIRE(send_all(a.get(),
                   sshos::encode(sshos::Msg{sshos::Input{"\x11"}})));  // Ctrl+Q

  bool detached = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!detached) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    const int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    auto m = recv_one(a.get(), dec_a, std::max(1, left));
    if (!m) break;
    if (std::holds_alternative<sshos::Detached>(*m)) detached = true;
  }
  CHECK(detached);
  a.reset();

  int status = 0;
  CHECK_EQ(::waitpid(daemon.pid(), &status, WNOHANG), 0);  // toujours vivant

  sshos::Fd b = connect_retry(name);
  REQUIRE(b.valid());
  REQUIRE(send_all(b.get(), sshos::encode(sshos::Msg{hello})));
  sshos::Decoder dec_b;
  auto welcome_b = recv_one(b.get(), dec_b, 3000);
  REQUIRE(welcome_b.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome_b));

  CHECK(wait_for_frame_containing(b.get(), dec_b, "Fichiers", "ssh_os", 3000));
}

// Même round, second emplacement du même motif : la branche `pending`
// (connexion acceptée qui n'a pas encore décliné son intention). Le brief la
// met explicitement dans le périmètre, et rien ne la discriminait jusqu'ici.
//
// Un Hello valide arrivé dans le même évènement que la fermeture de son
// expéditeur doit être honoré, exactement comme il l'aurait été s'il était
// arrivé une milliseconde plus tôt. C'est le point : avant le correctif, le
// fait qu'un Hello compte ou non dépendait de la coalescence décidée par le
// noyau — une course. Après, c'est déterministe.
//
// Observable retenue : le client déjà attaché reçoit Detached(« un autre
// client a pris la main »), que drop_client() pousse ET vide sur le fil avant
// de fermer. C'est la preuve directe que le Hello a bien été lu malgré le HUP
// simultané.
TEST(daemon_honours_a_hello_coalesced_with_its_senders_closure) {
  const std::string name = unique_name() + "-hup-pending";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd a = connect_retry(name);
  REQUIRE(a.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.term = "xterm";
  hello.utf8 = true;
  REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{hello})));

  sshos::Decoder dec_a;
  auto welcome = recv_one(a.get(), dec_a, 2000);
  REQUIRE(welcome.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome));
  REQUIRE(wait_for_frame_containing(a.get(), dec_a, "clics: 0", "ssh_os", 3000));

  REQUIRE(::kill(daemon.pid(), SIGSTOP) == 0);

  // B se connecte pendant le gel : le noyau met la connexion en file d'attente
  // de l'écouteur, le démon ne l'accepte qu'à la reprise. Ses octets et son
  // FIN sont donc tous deux déjà présents quand son descripteur entre enfin
  // dans epoll — la coalescence est acquise sans dépendre d'un ordonnancement
  // heureux.
  const bool stopped = wait_until_stopped(daemon.pid(), 2000);
  bool sent_b = false;
  {
    sshos::Fd b = connect_retry(name);
    if (b.valid()) {
      sent_b = send_all(b.get(), sshos::encode(sshos::Msg{hello}));
    }
  }  // fermeture immédiate de B, juste derrière son Hello

  const bool resumed = ::kill(daemon.pid(), SIGCONT) == 0;

  REQUIRE(stopped);
  REQUIRE(sent_b);
  REQUIRE(resumed);

  // Contre le code d'avant le correctif, le HUP de B est honoré avant le
  // drainage : son Hello n'est jamais lu, A n'est jamais évincé, et cette
  // attente n'apporte aucun Detached.
  bool saw_detached = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count());
    auto m = recv_one(a.get(), dec_a, std::max(1, remaining));
    if (!m) break;
    if (std::holds_alternative<sshos::Detached>(*m)) {
      saw_detached = true;
      break;
    }
  }
  CHECK(saw_detached);
}

// Le cas 4 de session_cancels_a_drag_on_every_one_of_the_seven_paths appelle
// cancel_drag() en direct : il prouve la méthode, pas les DEUX sites d'appel
// du démon. C'est ce test-ci qui les couvre -- un client qui s'en va en plein
// glissement, et le suivant qui hérite d'un bureau propre plutôt que d'un
// geste à moitié fait.
TEST(daemon_forgets_a_drag_left_behind_by_a_departed_client) {
  const std::string name = unique_name() + "-drag-orphan";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Hello hello = make_hello(80, 24);
  hello.utf8 = true;

  {
    sshos::Fd a = connect_retry(name);
    REQUIRE(a.valid());
    REQUIRE(send_all(a.get(), sshos::encode(sshos::Msg{hello})));
    sshos::Decoder dec_a;
    REQUIRE(wait_for_frame_containing(a.get(), dec_a, "Bloc", "ssh_os", 3000));

    // Presser sur la barre de titre (ligne 1, colonne 6 en base 0 -> 7;2 en
    // SGR, qui compte à partir de 1), bouger bouton enfoncé (cb = 32), et
    // disparaître sans jamais relâcher.
    REQUIRE(send_all(a.get(),
                     sshos::encode(sshos::Msg{sshos::Input{"\033[<0;7;2M"}})));
    REQUIRE(send_all(
        a.get(), sshos::encode(sshos::Msg{sshos::Input{"\033[<32;20;10M"}})));
  }  // le client se ferme ici, en plein geste

  sshos::Fd b = connect_retry(name);
  REQUIRE(b.valid());
  REQUIRE(send_all(b.get(), sshos::encode(sshos::Msg{hello})));
  sshos::Decoder dec_b;
  REQUIRE(wait_for_frame_containing(b.get(), dec_b, "Bloc", "ssh_os", 3000));

  // Un mouvement bouton enfoncé : si le glissement avait survécu au
  // détachement, la fenêtre suivrait le curseur jusqu'en bas de l'écran.
  REQUIRE(send_all(
      b.get(), sshos::encode(sshos::Msg{sshos::Input{"\033[<32;60;18M"}})));

  // Puis un clic à l'intérieur de la zone cliente d'ORIGINE. Il n'atteint
  // Bloc que si la fenêtre n'a pas bougé ; sinon il tombe sur le bureau et le
  // compteur reste à zéro, ce que l'attente traduit en échec.
  REQUIRE(send_all(b.get(),
                   sshos::encode(sshos::Msg{sshos::Input{"\033[<0;10;5M"}})));

  // Un Resize immédiatement derrière, non pour redimensionner quoi que ce
  // soit -- les dimensions sont identiques -- mais pour son effet de bord :
  // le démon y appelle Differ::invalidate(), donc la trame suivante est un
  // repeint COMPLET. Sans lui, l'incrément du compteur ne produit qu'un
  // delta d'une seule cellule (le chiffre), où la chaîne « clics: 1 » ne
  // figure jamais, et aucune aiguille textuelle n'aurait de quoi mordre.
  // Le socket garantit l'ordre : le clic est traité avant le Resize.
  REQUIRE(send_all(b.get(), sshos::encode(sshos::Msg{sshos::Resize{80, 24}})));
  CHECK(wait_for_frame_containing(b.get(), dec_b, "clics: 1", "ssh_os", 3000));
}

// ---------------------------------------------------------------------------
// L'aide de découvrabilité (§16 : « la touche leader est peu découvrable
// pour qui vient d'un vrai bureau »). Elle ne se teste pas sur son contenu
// -- c'est le rôle du golden -- mais sur les trois chemins qui l'ouvrent et
// sur celui qui la referme.
// ---------------------------------------------------------------------------

namespace {

// Arme l'accord et laisse-le en l'air, comme une main qui hésite.
void arm_leader(Session& s) {
  s.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
}

}  // namespace

TEST(session_opens_the_help_when_the_leader_chord_is_left_hanging) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  // Au repos, rien n'attend : le démon ne doit pas se réveiller pour rien.
  CHECK_EQ(sess.help_delay_ms(), -1);

  // L'horloge avance AVANT l'armement, et c'est essentiel : l'estampille
  // par défaut et l'origine de l'horloge du double valent toutes deux
  // l'époque. Sans ce décalage, le test passerait à l'identique même si
  // l'armement n'estampillait rien -- une mutation l'a montré.
  plat.advance_steady(std::chrono::seconds(30));

  arm_leader(sess);
  const int delay = sess.help_delay_ms();
  CHECK(delay > 0);
  CHECK(delay <= 500);

  // Un accord tapé d'un trait ne doit JAMAIS faire clignoter l'aide : à
  // 400 ms elle n'est toujours pas due.
  plat.advance_steady(std::chrono::milliseconds(400));
  CHECK(sess.help_delay_ms() > 0);
  sess.take_dirty();
  Surface early(80, 24);
  sess.render(early);
  CHECK(!surface_contains(early, "Ctrl+A puis :"));

  plat.advance_steady(std::chrono::milliseconds(150));
  CHECK_EQ(sess.help_delay_ms(), 0);
  // C'est take_dirty() qui la découvre : render() n'est appelée que sur une
  // trame déjà sale et ne pourrait donc jamais se salir elle-même.
  CHECK(sess.take_dirty());
  Surface shown(80, 24);
  sess.render(shown);
  CHECK(surface_contains(shown, "Ctrl+A puis :"));
  CHECK(surface_contains(shown, "Maximiser (bascule)"));

  // Ouverte, elle cesse de se réclamer : sans ça le démon tournerait à
  // 0 ms de délai tant qu'elle est à l'écran.
  CHECK_EQ(sess.help_delay_ms(), -1);
}

// La touche qui referme l'aide GARDE son effet. L'aide s'est ouverte parce
// que l'accord traînait, pas pour installer un mode dont il faudrait
// ressortir : la faire avaler une frappe punirait l'hésitation.
TEST(session_closes_the_help_on_the_next_key_without_eating_it) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  // La fenêtre part de la première marche de la cascade : sa barre de
  // titre est en ligne 1, elle passera en ligne 0 une fois maximisée.
  REQUIRE_EQ(title_row_of(s, 24), 1);

  arm_leader(sess);
  plat.advance_steady(std::chrono::milliseconds(600));
  sess.take_dirty();
  Surface shown(80, 24);
  sess.render(shown);
  REQUIRE(surface_contains(shown, "Ctrl+A puis :"));

  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'z', 0}});
  Surface after(80, 24);
  sess.render(after);
  CHECK(!surface_contains(after, "Ctrl+A puis :"));
  CHECK_EQ(title_row_of(after, 24), 0);  // et 'z' a bien maximisé
}

TEST(session_opens_the_help_on_the_question_mark_chord) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  arm_leader(sess);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'?', 0}});
  Surface shown(80, 24);
  sess.render(shown);
  CHECK(surface_contains(shown, "Ctrl+A puis :"));

  // Un clic, n'importe où, la retire -- et ne fait rien d'autre : ce n'est
  // pas un dialogue, il n'y a rien à y répondre.
  press_at(sess, 40, 12);
  Surface gone(80, 24);
  sess.render(gone);
  CHECK(!surface_contains(gone, "Ctrl+A puis :"));
}

// Le rappel du panneau et l'aide sont les deux moitiés de la même parade :
// le premier dit quelle touche, la seconde dit quoi en faire. Le cliquer
// doit donc ouvrir la seconde -- qui ne connaît pas encore le clavier du
// bureau a toutes les raisons d'essayer la souris.
TEST(session_opens_the_help_when_the_panel_reminder_is_clicked) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  const std::string row = s.text_row(23);
  const size_t at = row.find("^A = aide");
  REQUIRE(at != std::string::npos);

  press_at(sess, static_cast<int>(at) + 1, 23);
  Surface shown(80, 24);
  sess.render(shown);
  CHECK(surface_contains(shown, "Ctrl+A puis :"));
}

// Le leader est configurable (spec §7.4) : ni l'en-tête de l'aide ni le
// rappel du panneau ne doivent nommer Ctrl+A en dur. Rien ne permet encore
// d'en changer -- le fichier de configuration arrive plus tard -- donc ce
// test vérifie ce qui est vérifiable aujourd'hui : que les deux textes
// s'accordent sur la MÊME touche, celle du dispatcheur.
TEST(session_shows_the_same_leader_in_the_panel_and_in_the_help) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  CHECK(surface_contains(s, "^A = aide"));

  arm_leader(sess);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'?', 0}});
  Surface shown(80, 24);
  sess.render(shown);
  CHECK(surface_contains(shown, "Ctrl+A puis :"));
}

// ---------------------------------------------------------------------------
// Les accords qui s'enchaînent, vus de la session : c'est elle qui tient le
// délai entre deux gestes d'une même série.
// ---------------------------------------------------------------------------

TEST(session_chains_repeatable_gestures_without_retyping_the_leader) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 24), 1);

  arm_leader(sess);
  for (int i = 0; i < 3; ++i) {
    // Une seule frappe par pas, sans reprendre Ctrl+A -- et le temps passe
    // entre les gestes, comme sous une vraie main.
    plat.advance_steady(std::chrono::milliseconds(300));
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Down, 0, 0}});
  }
  Surface moved(80, 24);
  sess.render(moved);
  CHECK_EQ(title_row_of(moved, 24), 4);  // trois cellules plus bas
}

// La série expire. Passé le délai, la touche appartient de nouveau à
// l'application : Bloc la reçoit, se déclare modifiée, et la fenêtre n'a
// pas bougé.
TEST(session_ends_the_series_after_its_window_and_gives_the_key_back) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  arm_leader(sess);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Down, 0, 0}});
  Surface once(80, 24);
  sess.render(once);
  REQUIRE_EQ(title_row_of(once, 24), 2);

  plat.advance_steady(std::chrono::seconds(5));
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'j', 0}});
  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 24), 2);         // rien n'a bougé
  CHECK(surface_contains(after, "Bloc *"));      // et Bloc a reçu la touche
}

// Ce qui rend la fenêtre de répétition sans danger : en série, une touche
// qui ne s'enchaîne pas n'est ni exécutée ni avalée. Sans cette règle, un
// « w » tapé dans un document une seconde après un déplacement fermerait la
// fenêtre sous les doigts.
TEST(session_never_lets_a_series_swallow_a_destructive_key) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE(title_row_of(s, 24) >= 0);

  arm_leader(sess);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Right, 0, 0}});

  // Aussitôt après, dans la fenêtre de répétition : « w » fermerait la
  // fenêtre s'il était capté.
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'w', 0}});
  Surface after(80, 24);
  sess.render(after);
  CHECK(title_row_of(after, 24) >= 0);       // la fenêtre est toujours là
  CHECK(surface_contains(after, "Bloc *"));  // et c'est Bloc qui a eu le « w »
}

// L'aide ne s'invite pas au milieu d'une série : elle répond à l'hésitation
// de qui vient de taper le leader, pas à celle de qui pousse une fenêtre.
TEST(session_does_not_pop_the_help_in_the_middle_of_a_series) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  plat.advance_steady(std::chrono::seconds(30));
  arm_leader(sess);
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Left, 0, 0}});

  CHECK_EQ(sess.help_delay_ms(), -1);
  plat.advance_steady(std::chrono::seconds(2));
  sess.take_dirty();
  Surface quiet(80, 24);
  sess.render(quiet);
  CHECK(!surface_contains(quiet, "Ctrl+A puis :"));
}

// ---------------------------------------------------------------------------
// Le bureau vide. Signalé à l'usage : « si il reste une fenêtre ouverte, je
// ne peux pas la fermer ». La session rouvrait une application à CHAQUE
// trame dès que la pile était vide, si bien que le [×] de la dernière
// fenêtre semblait ne rien faire.
// ---------------------------------------------------------------------------

TEST(session_lets_the_desktop_be_empty_once_the_last_window_is_closed) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 24), 1);  // une fenêtre au démarrage

  // [×] de la seule fenêtre : cadre {2,1,44,14}, dernier bouton en 42..44.
  press_at(sess, 44, 1);
  release_at(sess, 44, 1);

  // Et le bureau RESTE vide, trame après trame : c'est le rendu qui
  // rouvrait, donc une seule vérification ne prouverait rien.
  for (int i = 0; i < 5; ++i) {
    Surface after(80, 24);
    sess.render(after);
    // 23 et non 24 : la ligne du panneau porte l'entrée ÉPINGLÉE « Bloc »,
    // qui n'est pas une fenêtre et ne disparaît jamais.
    if (title_row_of(after, 23) >= 0) {
      th::fail(__FILE__, __LINE__,
               "une fenetre est revenue toute seule au tour " +
                   std::to_string(i));
    }
  }
}

// Une fenêtre au démarrage, oui -- s'attacher sur un écran vide sans savoir
// quoi faire est le pire premier contact possible. Mais une seule fois : la
// suite appartient à l'utilisateur.
TEST(session_seeds_one_window_at_the_start_and_never_again) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE(title_row_of(s, 24) >= 0);

  press_at(sess, 44, 1);
  release_at(sess, 44, 1);
  Surface empty(80, 24);
  sess.render(empty);
  REQUIRE_EQ(title_row_of(empty, 23), -1);

  // Un redimensionnement du bureau ne réamorce pas non plus.
  sess.resize(100, 30);
  Surface bigger(100, 30);
  sess.render(bigger);
  CHECK_EQ(title_row_of(bigger, 29), -1);
}

// Un bureau vide doit dire quoi faire, et le dire pour la SOURIS : c'est
// avec elle qu'on arrive, et le menu est à un clic.
TEST(session_says_what_to_do_when_the_desktop_is_empty) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  press_at(sess, 44, 1);
  release_at(sess, 44, 1);

  Surface empty(80, 24);
  sess.render(empty);
  REQUIRE_EQ(title_row_of(empty, 23), -1);
  CHECK(surface_contains(empty, "menu"));

  // Et l'invite disparaît dès qu'une fenêtre existe. On vise « le menu »,
  // pas « pour ouvrir » : l'invite est peinte AVANT les fenêtres, et le
  // cadre {2,1,44,14} s'arrête pile sur la colonne où « pour ouvrir »
  // commence. Le test ne passait que par accident de géométrie -- il
  // laissait vivre une invite dessinée en permanence.
  REQUIRE(sess.open_from_catalog("editeur") != 0u);
  Surface busy(80, 24);
  sess.render(busy);
  CHECK(!surface_contains(busy, "le menu"));
}

// Clic DROIT sur le bureau : le menu s'ouvre là où on a cliqué. C'est le
// geste qu'on essaie en premier quand on vient d'un vrai bureau, et c'est
// la sortie d'un écran vide sans toucher au clavier.
TEST(session_opens_the_menu_on_a_right_click_on_the_desktop) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  // Loin de toute fenêtre : la fenêtre du démarrage occupe {2,1,44,14}.
  sess.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Press, 2, 60, 18, 0}});
  Surface opened(80, 24);
  sess.render(opened);
  // « Panneau : haut » n'existe QUE dans le menu -- « Bloc » se lirait
  // aussi bien dans la barre des tâches.
  REQUIRE(surface_contains(opened, "Panneau"));

  // Et il s'ouvre AU CURSEUR, pas sur son bouton. Ancre en 60, largeur du
  // menu 34, écran 80 : ramené en 46, donc les entrées vers la colonne 47.
  // Ancré au bouton il serait en 1, et `open_at` ne servirait à rien.
  int row = -1;
  for (int y = 0; y < 24; ++y) {
    if (opened.text_row(y).find("Panneau") != std::string::npos) {
      row = y;
      break;
    }
  }
  REQUIRE(row >= 0);
  CHECK(opened.text_row(row).find("Panneau") > std::string::size_type(40));
}

// Le clic GAUCHE sur le vide ne fait toujours rien : il sert à défocaliser
// ou à ne rien faire, et ouvrir un menu dessus serait insupportable.
TEST(session_leaves_a_left_click_on_the_desktop_alone) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  press_at(sess, 60, 18);
  release_at(sess, 60, 18);
  Surface after(80, 24);
  sess.render(after);
  CHECK(!surface_contains(after, "Panneau"));
}

// Double-clic sur la barre de titre : maximise, puis rétablit. C'est le
// geste de tous les bureaux, et il n'existait qu'au clavier et au bouton.
TEST(session_maximizes_on_a_double_click_on_the_title_bar) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 24), 1);

  // Deux appuis rapprochés sur la barre de titre, loin des boutons.
  press_at(sess, 10, 1);
  release_at(sess, 10, 1);
  plat.advance_steady(std::chrono::milliseconds(120));
  press_at(sess, 10, 1);
  release_at(sess, 10, 1);
  Surface big(80, 24);
  sess.render(big);
  // Maximisée : le cadre occupe toute la zone, ses boutons filent à droite.
  CHECK(big.text_row(0).find("[") != std::string::npos);
  CHECK(big.text_row(0).find("Bloc") != std::string::npos);

  // Et le même geste rétablit.
  plat.advance_steady(std::chrono::seconds(2));
  press_at(sess, 10, 0);
  release_at(sess, 10, 0);
  plat.advance_steady(std::chrono::milliseconds(120));
  press_at(sess, 10, 0);
  release_at(sess, 10, 0);
  Surface back(80, 24);
  sess.render(back);
  CHECK_EQ(title_row_of(back, 24), 1);
}

// Deux clics ESPACÉS ne font pas un double-clic : sinon toute paire de
// prises de focus sur la barre de titre finirait par maximiser.
TEST(session_does_not_take_two_slow_clicks_for_a_double_click) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  press_at(sess, 10, 1);
  release_at(sess, 10, 1);
  plat.advance_steady(std::chrono::seconds(2));
  press_at(sess, 10, 1);
  release_at(sess, 10, 1);
  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 24), 1);  // toujours à sa place
}

// La position de l'invite n'est pas cosmétique : au bord ou tout en bas de
// la zone, elle se confond avec le panneau et cesse d'être ce qu'on regarde
// en premier sur un écran par ailleurs nu.
TEST(session_centres_the_empty_desktop_hint) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  press_at(sess, 44, 1);
  release_at(sess, 44, 1);

  Surface empty(80, 24);
  sess.render(empty);

  int row = -1;
  for (int y = 0; y < 23; ++y) {
    if (empty.text_row(y).find("Bureau vide") != std::string::npos) {
      row = y;
      break;
    }
  }
  REQUIRE(row >= 0);
  // Zone de travail {0, 0, 80, 23}, deux lignes centrées : le haut en 10.
  CHECK_EQ(row, (23 - 2) / 2);
  // « Bureau vide » fait 11 cellules sur 80.
  CHECK_EQ(empty.text_row(row).find("Bureau vide"),
           std::string::size_type((80 - 11) / 2));
}

// Un clic droit envoie un appui ET un relâchement. Si les deux ouvraient,
// le clic qui referme le menu le rouvrirait aussitôt : plus moyen d'en
// sortir à la souris, ce qui est le défaut d'origine sous une autre forme.
TEST(session_does_not_reopen_the_menu_on_the_release_of_the_click_that_closed_it) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  right_press(sess, 60, 18);
  right_release(sess, 60, 18);
  Surface opened(80, 24);
  sess.render(opened);
  REQUIRE(surface_contains(opened, "Panneau"));

  // Un second clic droit, hors du menu : il le referme, et son relâchement
  // ne doit surtout pas le rouvrir.
  right_press(sess, 2, 20);
  right_release(sess, 2, 20);
  Surface closed(80, 24);
  sess.render(closed);
  CHECK(!surface_contains(closed, "Panneau"));
}

// Un double-clic se mesure d'APPUI à APPUI. Compter depuis le relâchement
// ferait d'un bouton tenu longtemps puis recliqué un double-clic, ce qu'il
// n'est pas.
TEST(session_measures_a_double_click_from_press_to_press) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  REQUIRE_EQ(title_row_of(s, 23), 1);

  press_at(sess, 10, 1);
  plat.advance_steady(std::chrono::milliseconds(500));  // bouton tenu
  release_at(sess, 10, 1);
  plat.advance_steady(std::chrono::milliseconds(200));
  press_at(sess, 10, 1);
  release_at(sess, 10, 1);

  // 700 ms d'appui à appui : au-delà de la fenêtre, donc pas un double.
  // Depuis le relâchement il n'y aurait que 200 ms, et la fenêtre serait
  // maximisée à tort.
  Surface after(80, 24);
  sess.render(after);
  CHECK_EQ(title_row_of(after, 23), 1);
}

// Une coordonnée négative n'est pas théorique : le parseur SGR fait
// `param - 1`, donc `CSI <2;0;0M` livre (-1, -1). Le menu doit rester à
// l'écran plutôt que d'être dessiné hors champ.
TEST(session_keeps_the_menu_on_screen_after_a_right_click_at_the_origin) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Press, 2, -1, -1, 0}});
  Surface opened(80, 24);
  sess.render(opened);
  CHECK(surface_contains(opened, "Panneau"));
}

// L'invite nomme le bouton du panneau par son glyphe. Sur un client qui
// n'annonce pas UTF-8, un ☰ brut se lirait en mojibake de trois cellules
// et désignerait un bouton qui, lui, s'est bien replié en « = ».
TEST(session_falls_back_to_ascii_in_the_empty_desktop_hint) {
  FakePlatform plat;
  Session sess(plat, g_fds, 80, 24);
  Surface s(80, 24);
  sess.render(s);
  press_at(sess, 44, 1);
  release_at(sess, 44, 1);

  Surface ascii(80, 24);
  sess.render(ascii);
  REQUIRE(surface_contains(ascii, "Bureau vide"));
  CHECK(surface_contains(ascii, "ou = en bas"));

  sshos::OutputProfile p;
  p.depth = sshos::ColorDepth::TrueColor;
  p.utf8 = true;
  sess.set_output(p);

  Surface uni(80, 24);
  sess.render(uni);
  CHECK(surface_contains(uni, "ou \xe2\x98\xb0 en bas"));
}

// UN ÉCHAPPEMENT SEUL DOIT FINIR PAR ARRIVER.
//
// `ESC` est indécidable tant qu'aucun octet ne suit : c'est peut-être la
// touche, c'est peut-être le début d'une séquence. `InputParser` le retient
// et attend qu'on lui dise que le délai a expiré -- et personne ne le lui
// disait. La méthode `timeout()` existait depuis le jalon 1 SANS AUCUN
// APPELANT, exactement comme `Decoder::failed()` avant elle.
//
// La conséquence ne se voyait qu'avec un vrai invité : dans `vim`,
// l'échappement ne quittait JAMAIS le mode insertion -- il restait en
// attente jusqu'à la frappe suivante et se relisait alors comme un accord
// `Alt` avec elle. Trouvé en faisant tourner un vrai `vim`, pas par un test.
//
// Le menu donne l'observable qu'il fallait : `ESC` le referme, et rien
// d'autre n'est envoyé après lui. Sans le repli du délai dans la boucle du
// démon, le menu reste ouvert pour toujours.
TEST(daemon_delivers_a_lone_escape_after_the_ambiguity_delay) {
  const std::string name = unique_name() + "-esc";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{make_hello(80, 24)})));

  sshos::Decoder dec;
  REQUIRE(wait_for_frame_containing(client.get(), dec, "ssh_os", "ssh_os", 5000));

  // Ctrl+A, Espace, puis un filtre : le menu s'ouvre sur les commandes de
  // panneau, dont AUCUNE n'existe ailleurs à l'écran.
  REQUIRE(send_all(client.get(),
                   sshos::encode(sshos::Msg{sshos::Input{"\001 pa"}})));
  REQUIRE(wait_for_frame_containing(client.get(), dec, "Panneau", "Panneau", 3000));

  // UN SEUL octet, et plus rien après. C'est tout le piège : le parseur
  // n'aura aucune frappe suivante pour lever son ambiguïté, et seul le
  // repli du délai dans la boucle du démon peut la lever à sa place.
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{sshos::Input{"\033"}})));
  ::usleep(400 * 1000);

  // Un Resize force un repeint COMPLET sans passer par une frappe -- une
  // frappe de plus lèverait elle-même l'ambiguïté et le test ne mesurerait
  // plus rien. Le protocole étant différentiel, c'est le seul moyen de
  // relire l'écran entier.
  REQUIRE(send_all(client.get(),
                   sshos::encode(sshos::Msg{sshos::Resize{80, 25}})));

  bool saw_full_frame = false;
  bool menu_gone = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (std::chrono::steady_clock::now() < deadline) {
    auto m = recv_one(client.get(), dec, 250);
    if (!m) continue;
    const auto* f = std::get_if<sshos::FrameMsg>(&*m);
    if (f == nullptr) continue;
    // Le repeint complet se reconnaît à la barre des tâches, qu'il porte
    // toujours en entier.
    if (f->ansi.find("ssh_os") == std::string::npos) continue;
    saw_full_frame = true;
    menu_gone = f->ansi.find("Panneau") == std::string::npos;
    break;
  }
  REQUIRE(saw_full_frame);
  CHECK(menu_gone);
}

// LE RAFRAICHISSEMENT PERIODIQUE DU FOND n'a PAS de cas ici, et c'est
// delibere. Un cas a ete ecrit, puis retire : il attendait une trame
// spontanee a travers le vrai demon, et n'en voyait aucune de facon
// fiable -- sur une machine au repos deux echantillons donnent le meme
// ecran, donc aucun delta, donc aucune trame. Le faire passer aurait
// demande de charger la machine dans la suite, ce qui rend le resultat
// dependant de ce que fait le reste du monde.
//
// Ce qui le couvre a la place : `test_sysinfo.cpp` pour le contenu et la
// troncature, les references de rendu pour sa presence sur le bureau, et
// une sonde bout-en-bout (`scratchpad/term_probe.py`) qui l'a montre
// vivant a travers le vrai demon -- memoire, debits reseau et liste de
// processus qui bougent.

TEST(session_tiles_its_windows_side_by_side_from_the_menu) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);  // amorce la première fenêtre

  const sshos::WindowId second = sess.open_from_catalog("editeur");
  REQUIRE(second != 0);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  for (char c : std::string("ranger")) {
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{
        sshos::Key::Char, static_cast<char32_t>(c), 0}});
  }
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
  sess.render(s);

  std::vector<sshos::Rect> seen;
  for (const auto& w : sess.windows_for_tests()) {
    if (w->mode != sshos::WinMode::Minimized) seen.push_back(w->display_rect);
  }
  REQUIRE_EQ(seen.size(), size_t{2});
  // Deux colonnes, pleine hauteur, qui se touchent sans se chevaucher.
  CHECK_EQ(seen[0].h, seen[1].h);
  CHECK_EQ(seen[0].w, seen[1].w);
  CHECK(seen[0].x != seen[1].x);
  CHECK_EQ(std::min(seen[0].x, seen[1].x) + seen[0].w,
           std::max(seen[0].x, seen[1].x));
}

// ------------------------------------------- ouvrir DEUX fois la meme app

// LE MENU EST UN LANCEUR, PAS UN RAPPEL. Il rappelait la fenêtre existante
// quand il y en avait une, ce qui rendait impossible d'ouvrir deux
// terminaux -- le geste le plus courant du bureau. Rappeler est le travail
// de la barre des tâches, qui existe pour ça.
TEST(session_opens_a_second_instance_from_the_menu) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);

  const size_t before = sess.windows_for_tests().size();
  for (int i = 0; i < 2; ++i) {
    sess.on_input(sshos::InputEvent{
        sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
    for (char c : std::string("edit")) {
      sess.on_input(sshos::InputEvent{sshos::KeyEvent{
          sshos::Key::Char, static_cast<char32_t>(c), 0}});
    }
    sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Enter, 0, 0}});
    sess.render(s);
  }

  CHECK_EQ(sess.windows_for_tests().size(), before + 2);
}

// LA BARRE DES TÂCHES, ELLE, RAPPELLE au clic gauche : c'est sa raison
// d'être, et empiler une fenêtre de plus à chaque clic serait le contraire
// de ce qu'on demande en visant une entrée qui existe déjà.
TEST(panel_recalls_instead_of_opening_on_a_left_click) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);
  const sshos::WindowId opened = sess.open_from_catalog("editeur");
  REQUIRE(opened != 0);
  sess.render(s);
  const size_t before = sess.windows_for_tests().size();

  // On cherche la cellule que le hit-test donne à cette fenêtre.
  int x = -1;
  for (int i = 0; i < 60 && x < 0; ++i) {
    const sshos::PanelHitResult h = sess.panel_hit_for_tests(i, 19);
    if (h.what == sshos::PanelHit::Task && h.win == opened) x = i;
  }
  REQUIRE(x >= 0);

  sess.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Press, 0, x, 19, 0}});
  CHECK_EQ(sess.windows_for_tests().size(), before);
}

// ET LE CLIC DROIT EN OUVRE UNE DE PLUS. Les deux gestes répondent à deux
// questions différentes -- « retourne à celle-là » et « donne m'en une
// autre » -- et les confondre enfermait l'utilisateur dans une seule
// fenêtre par application.
TEST(panel_opens_a_new_instance_on_a_right_click) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);
  const sshos::WindowId opened = sess.open_from_catalog("editeur");
  REQUIRE(opened != 0);
  sess.render(s);
  const size_t before = sess.windows_for_tests().size();

  int x = -1;
  for (int i = 0; i < 60 && x < 0; ++i) {
    const sshos::PanelHitResult h = sess.panel_hit_for_tests(i, 19);
    if (h.what == sshos::PanelHit::Task && h.win == opened) x = i;
  }
  REQUIRE(x >= 0);

  sess.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Press, 2, x, 19, 0}});
  sess.render(s);
  CHECK_EQ(sess.windows_for_tests().size(), before + 1);
}

// ------------------------------------------------------------- l'ancrage

// `Ctrl+A` puis `Ctrl+flèche` : la fenêtre prend la MOITIÉ de l'écran du
// côté de la flèche, pleine hauteur. La touche « Tux » des bureaux
// graphiques n'existe pas dans un terminal -- aucun n'en rapporte l'état
// -- d'où le leader.
TEST(session_snaps_the_focused_window_to_a_half) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);

  // SANS ACCORD : `Ctrl+fleche` suffit.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Left, 0, sshos::mod::Ctrl}});
  sess.render(s);

  const sshos::Window* w = sess.window_for_tests(sess.focused_for_tests());
  REQUIRE(w != nullptr);
  CHECK_EQ(w->display_rect.x, 0);
  CHECK_EQ(w->display_rect.w, 30);
  CHECK_EQ(w->display_rect.h, 19);  // toute la hauteur, panneau exclu
}

TEST(session_snaps_to_the_right_half_too) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Right, 0, sshos::mod::Ctrl}});
  sess.render(s);

  const sshos::Window* w = sess.window_for_tests(sess.focused_for_tests());
  REQUIRE(w != nullptr);
  CHECK_EQ(w->display_rect.x, 30);
  CHECK_EQ(w->display_rect.w, 30);
}

// Ancrer vers le HAUT donne une demi-hauteur PLEINE LARGEUR : c'est
// l'autre moitié du geste, et l'oublier laisse deux flèches sans effet.
TEST(session_snaps_to_a_half_height_on_the_vertical_arrows) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Up, 0, sshos::mod::Ctrl}});
  sess.render(s);

  const sshos::Window* w = sess.window_for_tests(sess.focused_for_tests());
  REQUIRE(w != nullptr);
  CHECK_EQ(w->display_rect.w, 60);
  CHECK_EQ(w->display_rect.y, 0);
  CHECK_EQ(w->display_rect.h, 10);
}

// La flèche NUE déplace toujours, et la flèche MAJ redimensionne toujours :
// l'ancrage ne leur a pas volé leur geste.
TEST(session_leaves_the_plain_and_shifted_arrows_alone) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);
  const sshos::Window* w = sess.window_for_tests(sess.focused_for_tests());
  REQUIRE(w != nullptr);
  const sshos::Rect before = w->display_rect;

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Right, 0, 0}});
  sess.render(s);

  CHECK_EQ(w->display_rect.w, before.w);  // deplacee, pas ancree
  CHECK(w->display_rect.x != before.x);
}

// ------------------------------------------------------------- le curseur

// LE CURSEUR DE L'APPLICATION ARRIVE JUSQU'À L'ÉCRAN. Les quatre
// applications répondent à `wants_cursor()` depuis toujours, `Differ::frame`
// sait le poser -- et personne, entre les deux, ne le demandait : le démon
// passait `std::nullopt` à chaque trame. Mesuré à la sonde : six `?25l`
// envoyés, zéro `?25h`. Aucun champ de saisie du bureau n'avait de caret.
TEST(session_hands_the_focused_app_cursor_to_the_screen) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);

  const auto& wins = sess.windows_for_tests();
  REQUIRE_EQ(wins.size(), size_t{1});
  const sshos::Rect cr = sshos::client_rect(wins[0]->display_rect);
  sshos::Pos want{};
  REQUIRE(wins[0]->app->wants_cursor(want));

  const std::optional<sshos::Pos> got = sess.cursor();
  REQUIRE(got.has_value());
  // Coordonnées de l'ÉCRAN : l'application parle de sa vue, et lui laisser
  // poser le caret chez sa voisine serait pire que pas de caret du tout.
  CHECK_EQ(got->x, cr.x + want.x);
  CHECK_EQ(got->y, cr.y + want.y);
}

// UNE FENÊTRE DE FOND N'A PAS LE CARET. Deux applications qui en veulent un
// n'en placeraient qu'un seul, et ce serait celle du dessous.
TEST(session_gives_the_cursor_to_the_focused_window_only) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);
  REQUIRE(sess.open_from_catalog("terminal") != 0);
  sess.render(s);

  const auto& wins = sess.windows_for_tests();
  REQUIRE_EQ(wins.size(), size_t{2});
  const sshos::Window* front = nullptr;
  for (const auto& w : wins) {
    if (w->id == sess.focused_for_tests()) front = w.get();
  }
  REQUIRE(front != nullptr);
  const sshos::Rect cr = sshos::client_rect(front->display_rect);

  const std::optional<sshos::Pos> got = sess.cursor();
  REQUIRE(got.has_value());
  CHECK(got->x >= cr.x);
  CHECK(got->x < cr.x + cr.w);
  CHECK(got->y >= cr.y);
  CHECK(got->y < cr.y + cr.h);
}

// LE MENU PREND LE CARET AVEC LE RESTE. Le laisser clignoter dans
// l'application ferait croire qu'on peut encore y taper, alors que tout va
// au menu ouvert par-dessus.
TEST(session_hides_the_cursor_while_the_menu_is_open) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);
  REQUIRE(sess.cursor().has_value());

  // Le menu, par l'accord habituel : lui aussi capture tout.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U' ', 0}});
  sess.render(s);
  CHECK(!sess.cursor().has_value());
}

// UNE FENÊTRE RÉDUITE N'A PAS DE CARET : elle n'est nulle part à l'écran, et
// son curseur se poserait sur ce qui a pris sa place.
TEST(session_shows_no_cursor_when_the_only_window_is_minimized) {
  FakePlatform plat;
  Session sess(plat, g_fds, 40, 12);
  Surface s(40, 12);
  sess.render(s);
  const auto& wins = sess.windows_for_tests();
  REQUIRE_EQ(wins.size(), size_t{1});

  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'-', 0}});
  sess.render(s);
  REQUIRE(wins[0]->mode == sshos::WinMode::Minimized);
  CHECK(!sess.cursor().has_value());
}

// LE CARET TRAVERSE LE FIL. Le démon passait `std::nullopt` à chaque
// trame : les quatre applications répondaient à `wants_cursor()`, `Differ`
// savait poser le caret, et rien ne les reliait -- mesuré à la sonde, six
// `?25l` envoyés pour zéro `?25h`. Ce cas monte le chemin en entier, du
// vrai démon au client, parce que c'est le seul endroit où le trou était.
TEST(daemon_shows_the_cursor_of_the_focused_application) {
  const std::string name = unique_name() + "-caret";
  DaemonHandle daemon(name);
  REQUIRE(daemon.valid());

  sshos::Fd client = connect_retry(name);
  REQUIRE(client.valid());
  REQUIRE(send_all(client.get(), sshos::encode(sshos::Msg{make_hello(80, 24)})));

  sshos::Decoder dec;
  auto welcome = recv_one(client.get(), dec, 3000);
  REQUIRE(welcome.has_value());
  REQUIRE(std::holds_alternative<sshos::Welcome>(*welcome));

  // `\033[?25h` : le curseur RENDU VISIBLE. Une trame sans caret le laisse
  // caché par le `?25l` de tête et ne le rallume jamais.
  CHECK(wait_for_frame_containing(client.get(), dec, "\033[?25h", "ssh_os",
                                  3000));
}

// UNE FENÊTRE RÉDUITE N'A PAS LE CARET, MÊME QUAND ELLE A LA MAIN. Elle
// n'est nulle part à l'écran ; le laisser à celle de derrière poserait un
// caret dans une fenêtre où la frappe suivante n'ira pas.
TEST(session_shows_no_cursor_when_the_focused_window_is_minimized) {
  FakePlatform plat;
  Session sess(plat, g_fds, 60, 20);
  Surface s(60, 20);
  sess.render(s);
  REQUIRE(sess.open_from_catalog("terminal") != 0);
  sess.render(s);
  REQUIRE_EQ(sess.windows_for_tests().size(), size_t{2});
  REQUIRE(sess.cursor().has_value());

  // La fenêtre qui a la main se réduit : l'autre reste visible derrière.
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'a', sshos::mod::Ctrl}});
  sess.on_input(sshos::InputEvent{sshos::KeyEvent{sshos::Key::Char, U'-', 0}});
  sess.render(s);

  CHECK(!sess.cursor().has_value());
}

namespace {

// Une application qui vise HORS de sa vue. Aucune vraie ne le fait, et
// c'est justement pourquoi la garde a besoin de celle-ci : un caret posé
// chez la voisine est un défaut qu'on ne verrait qu'à l'écran, sur une
// fenêtre qui n'a même pas la main.
class CursorRunaway : public sshos::App {
 public:
  void render(sshos::View) override {}
  bool wants_cursor(sshos::Pos& out) const override {
    out = sshos::Pos{9999, 9999};
    return true;
  }
};

std::unique_ptr<sshos::App> make_runaway() {
  return std::make_unique<CursorRunaway>();
}
std::unique_ptr<sshos::App> make_plain_double() {
  return std::make_unique<sshos::Bloc>();
}

}  // namespace

// UN CARET HORS DE SA VUE N'EST PAS POSÉ. On le laisse tomber plutôt que de
// le rabattre sur un bord : une application qui vise à côté ne veut pas de
// caret sur le bord, et l'y mettre serait un mensonge de plus.
TEST(session_drops_a_cursor_an_app_puts_outside_its_own_view) {
  Session::set_seed_factory_for_tests(&make_runaway);
  {
    FakePlatform plat;
    Session sess(plat, g_fds, 60, 20);
    Surface s(60, 20);
    sess.render(s);
    REQUIRE_EQ(sess.windows_for_tests().size(), size_t{1});
    CHECK(!sess.cursor().has_value());
  }
  // La fabrique est GLOBALE : la rendre est aussi obligatoire que la
  // restauration d'un fichier muté, sinon tous les cas suivants ouvrent une
  // application qui ne dessine rien.
  Session::set_seed_factory_for_tests(&make_plain_double);
}
