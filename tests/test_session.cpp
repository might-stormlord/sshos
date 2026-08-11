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

#include "common/fd.hpp"
#include "common/net.hpp"
#include "common/platform.hpp"
#include "common/proto.hpp"
#include "daemon/daemon.hpp"
#include "daemon/session.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Session;
using sshos::Surface;

namespace {

// Horloge figée : sans elle le harnais n'est pas déterministe par
// construction, et un test d'affichage d'heure est ininspectable.
struct FakePlatform : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    // 2026-08-10 14:05:00 UTC
    return std::chrono::system_clock::time_point(std::chrono::seconds(1786370700));
  }
  std::string read_file(std::string_view) const override { return {}; }
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
  Session sess(plat, 40, 12);
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
  Session sess_jan(jan, 40, 12);
  Surface s_jan(40, 12);
  sess_jan.render(s_jan);
  const std::string panel_jan = s_jan.text_row(11);
  const auto pos_jan = panel_jan.find("07:00");  // EST : UTC-5
  CHECK(pos_jan != std::string::npos);

  FakePlatformAt aug(kAugustEpoch);
  Session sess_aug(aug, 40, 12);
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

TEST(session_draws_a_bordered_box_with_its_title) {
  FakePlatform plat;
  Session sess(plat, 40, 12);
  Surface s(40, 12);
  sess.render(s);

  bool found_title = false;
  for (int y = 0; y < 11; ++y) {
    if (s.text_row(y).find("ssh_os 2.0") != std::string::npos) found_title = true;
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
  Session sess(plat, 12, 3);
  Surface s(12, 3);
  sess.render(s);  // ne doit ni planter ni écrire hors surface
  CHECK(s.text_row(0).find("petit") != std::string::npos);
}

TEST(session_quits_on_ctrl_q) {
  FakePlatform plat;
  Session sess(plat, 40, 12);
  CHECK(!sess.wants_quit());
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'q', sshos::mod::Ctrl}});
  CHECK(sess.wants_quit());
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
  Session sess(plat, 40, 3);
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
  Session sess(plat, 12, 3);
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
  Session sess(plat, 999, 999);

  Surface a(40, 12);
  sess.render(a);
  CHECK(a.text_row(11).find("ssh_os") != std::string::npos);
  CHECK(a.text_row(10).find("ssh_os") == std::string::npos);
  bool found_title_a = false;
  for (int y = 0; y < 11; ++y) {
    if (a.text_row(y).find("ssh_os 2.0") != std::string::npos) found_title_a = true;
  }
  CHECK(found_title_a);

  Surface b(60, 20);
  sess.render(b);
  CHECK(b.text_row(19).find("ssh_os") != std::string::npos);
  CHECK(b.text_row(18).find("ssh_os") == std::string::npos);
  bool found_title_b = false;
  for (int y = 0; y < 19; ++y) {
    if (b.text_row(y).find("ssh_os 2.0") != std::string::npos) found_title_b = true;
  }
  CHECK(found_title_b);
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
  REQUIRE(wait_for_avail_at_least(client.get(), 174243, 12000));

  // Étape 3 : ce repaint (603 243 octets) doit déborder le plafond compte
  // tenu du reliquat non vidé de l'étape 2, avec off_ encore > 0 — Dirty.
  REQUIRE(
      send_all(client.get(), sshos::encode(sshos::Msg{sshos::Resize{1500, 400}})));

  CHECK(wait_for_peer_close(client.get(), 12000));
}

// Le test bout-en-bout exigé par la tâche : un vrai démon, un vrai client
// sur socket abstrait, un aller Hello/Welcome, une trame reçue contenant le
// titre de la boîte ("ssh_os 2.0") et le texte du panneau ("ssh_os"), puis
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

  CHECK(wait_for_frame_containing(client.get(), dec, "ssh_os 2.0", "ssh_os", 2000));

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
TEST(daemon_quits_on_ctrl_q_received_over_the_wire) {
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

  REQUIRE(send_all(client.get(),
                    sshos::encode(sshos::Msg{sshos::Input{"\x11"}})));

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
