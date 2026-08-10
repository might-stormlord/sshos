#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "client/client.hpp"
#include "client/tty_guard.hpp"
#include "common/fd.hpp"
#include "common/net.hpp"
#include "harness.hpp"

using sshos::Fd;

namespace {

// Restaure une variable d'environnement à sa valeur d'avant le test, qu'elle
// ait été définie ou non. Copie locale du motif déjà utilisé dans
// tests/test_net.cpp (portée anonyme là-bas, donc non partageable telle
// quelle) : indispensable ici, le test env_delta_carries_the_ssh_variables_
// that_exist du plan manipule SSH_AUTH_SOCK/SSH_TTY sans restauration, et
// tous les test_*.cpp tournent dans le même processus (voir tests/main.cpp)
// -- sans ça une valeur ambiante réelle (SSH_AUTH_SOCK d'une vraie session
// SSH, par exemple) serait écrasée pour le reste du run.
class EnvVarGuard {
 public:
  explicit EnvVarGuard(const char* name) : name_(name) {
    if (const char* v = std::getenv(name)) {
      had_value_ = true;
      value_ = v;
    }
  }
  ~EnvVarGuard() {
    if (had_value_) {
      ::setenv(name_, value_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  EnvVarGuard(const EnvVarGuard&) = delete;
  EnvVarGuard& operator=(const EnvVarGuard&) = delete;

 private:
  const char* name_;
  bool had_value_ = false;
  std::string value_;
};

std::string unique_socket_name(const char* suffix) {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "sshos-test-tty/" << ::getpid() << '-' << suffix << '-' << std::hex << dist(rng);
  return os.str();
}

// Sonde de connexion non bloquante, indépendante de connect_with_timeout()
// et de connect_abstract() : elle sert à remplir un backlog sans jamais
// bloquer le test lui-même. Même motif que
// tests/test_net.cpp:try_queue_connection(), dupliqué localement pour la
// même raison (portée anonyme là-bas).
Fd queue_pending_connection(const std::string& name) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return Fd();
  Fd guard(fd);

  sockaddr_un addr{};
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());

  if (::connect(guard.get(), reinterpret_cast<sockaddr*>(&addr), len) == 0) return guard;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return Fd();
  return Fd();
}

struct TestPty {
  Fd master;
  Fd slave;
  bool ok = false;
};

// Ouvre un pseudo-terminal pour tester TtyGuard sans dépendre d'un vrai
// terminal attaché à la suite de tests. Échoue proprement (ok=false) plutôt
// que de lever si posix_openpt()/grantpt()/unlockpt() ne sont pas
// disponibles dans le bac à sable qui exécute la suite -- le test appelant
// doit alors sauter, pas échouer.
TestPty open_test_pty() {
  TestPty pty;
  const int m = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0) return pty;
  Fd master(m);
  if (::grantpt(master.get()) != 0) return pty;
  if (::unlockpt(master.get()) != 0) return pty;
  const char* name = ::ptsname(master.get());
  if (name == nullptr) return pty;
  const int s = ::open(name, O_RDWR | O_NOCTTY);
  if (s < 0) return pty;
  pty.master = std::move(master);
  pty.slave = Fd(s);
  pty.ok = true;
  return pty;
}

// Lit tout ce qui est disponible sur `fd` pendant au plus `timeout_ms`,
// avec un délai court entre deux lectures pour laisser le temps aux octets
// suivants d'arriver sans pour autant attendre indéfiniment.
std::string drain(int fd, int timeout_ms) {
  std::string out;
  for (;;) {
    pollfd pfd{fd, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || (pfd.revents & POLLIN) == 0) break;
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
    timeout_ms = 20;
  }
  return out;
}

}  // namespace

// --- Les quatre tests du plan (Task 10, Step 1), transcrits tels quels ---

TEST(tty_setup_sequence_is_exact) {
  CHECK_EQ(sshos::tty_setup_sequence(),
           std::string("\033[?1049h"    // écran alterné
                       "\033[?1002h"    // souris : boutons + glissement
                       "\033[?1006h"    // encodage SGR
                       "\033[?2004h"    // collage encadré
                       "\033[?1004h"    // rapports de focus
                       "\033[?7l"));    // pas de repli automatique
}

// Le miroir exact, dans l'ordre inverse. Un mode oublié ici, c'est un
// terminal inutilisable après un plantage.
TEST(tty_restore_sequence_is_the_mirror) {
  CHECK_EQ(sshos::tty_restore_sequence(),
           std::string("\033[?25h"
                       "\033[?7h"
                       "\033[?1004l"
                       "\033[?2004l"
                       "\033[?1006l"
                       "\033[?1002l"
                       "\033[?1049l"));
}

TEST(tty_every_mode_set_is_also_unset) {
  const std::string on = sshos::tty_setup_sequence();
  const std::string off = sshos::tty_restore_sequence();
  for (const char* m : {"1049", "1002", "1006", "2004", "1004", "7"}) {
    const std::string set = std::string("\033[?") + m + "h";
    const std::string unset = std::string("\033[?") + m + "l";
    if (on.find(set) != std::string::npos) {
      CHECK(off.find(unset) != std::string::npos);
    }
  }
}

// Écarts par rapport au texte du plan : les deux ::setenv/::unsetenv sont
// désormais encadrés par EnvVarGuard (voir la définition ci-dessus). Le plan
// laisse SSH_AUTH_SOCK et SSH_TTY altérées après le test (un unsetenv final
// qui écrase, sans la restaurer, toute valeur ambiante préexistante) --
// exactement le défaut que la consigne de cette tâche demande de corriger
// plutôt que de transcrire. Le comportement vérifié est inchangé.
TEST(env_delta_carries_the_ssh_variables_that_exist) {
  EnvVarGuard auth_guard("SSH_AUTH_SOCK");
  EnvVarGuard tty_guard("SSH_TTY");

  ::setenv("SSH_AUTH_SOCK", "/tmp/agent.test", 1);
  ::unsetenv("SSH_TTY");
  const auto d = sshos::collect_env_delta();
  bool found_auth = false;
  bool found_tty = false;
  for (const auto& [k, v] : d) {
    if (k == "SSH_AUTH_SOCK") {
      found_auth = true;
      CHECK_EQ(v, std::string("/tmp/agent.test"));
    }
    if (k == "SSH_TTY") found_tty = true;
  }
  CHECK(found_auth);
  CHECK(!found_tty);  // une variable absente n'est pas transmise vide
}

// --- Ruling 1 : le littéral async-signal-safe et la fonction publique ---
// ---            doivent rester identiques, octet pour octet.          ---

TEST(tty_crash_restore_literal_matches_the_public_sequence_byte_for_byte) {
  // Ce test n'existe que contre le code corrigé : crash_restore_literal_
  // for_tests() n'a pas d'équivalent dans tty_guard.hpp du plan (le
  // gestionnaire de référence appelle tty_restore_sequence() directement,
  // ce qui alloue en contexte de signal -- voir tty_guard.cpp). Il ne
  // compile donc pas contre la version de référence ; c'est le point.
  const std::string via_function = sshos::tty_restore_sequence();
  const char* literal = sshos::crash_restore_literal_for_tests();
  CHECK_EQ(std::string(literal), via_function);
}

// --- Ruling 2 (+ 3 en creux) : le gestionnaire de plantage doit restaurer
// --- le termios, pas seulement les séquences d'échappement, sans jamais
// --- allouer -- vérifié en rejouant sa logique réelle sur un pseudo-
// --- terminal, sans déclencher de vrai signal.

TEST(tty_crash_restore_recovers_the_saved_termios_and_writes_the_mirror_sequence) {
  TestPty pty = open_test_pty();
  if (!pty.ok) {
    std::fprintf(stderr,
                 "  SKIP tty_crash_restore_recovers_the_saved_termios_and_writes_the_mirror_"
                 "sequence : pseudo-terminal indisponible dans ce bac a sable\n");
    return;
  }

  // État de référence délibérément "cuit" (ICANON+ECHO), distinct du mode
  // brut que TtyGuard va imposer : si la restauration ne repassait pas par
  // tcsetattr(), ces bits resteraient à plat et le test le verrait.
  termios baseline{};
  REQUIRE_EQ(::tcgetattr(pty.slave.get(), &baseline), 0);
  baseline.c_lflag |= (ICANON | ECHO);
  REQUIRE_EQ(::tcsetattr(pty.slave.get(), TCSANOW, &baseline), 0);
  termios confirmed{};
  REQUIRE_EQ(::tcgetattr(pty.slave.get(), &confirmed), 0);
  REQUIRE((confirmed.c_lflag & ICANON) != 0);
  REQUIRE((confirmed.c_lflag & ECHO) != 0);

  {
    sshos::TtyGuard guard(pty.slave.get());

    // Le mode brut doit avoir effacé ICANON/ECHO -- sinon rien de ce qui
    // suit ne prouverait quoi que ce soit (rien à restaurer).
    termios raw_now{};
    REQUIRE_EQ(::tcgetattr(pty.slave.get(), &raw_now), 0);
    REQUIRE((raw_now.c_lflag & ICANON) == 0);
    REQUIRE((raw_now.c_lflag & ECHO) == 0);

    // Vide la séquence d'installation écrite par le constructeur : ce n'est
    // pas ce que ce test vérifie.
    (void)drain(pty.master.get(), 200);

    // Rejoue exactement ce que ferait le gestionnaire de signal fatal, sans
    // passer par un vrai signal (voir tty_guard.hpp pour la justification).
    sshos::run_crash_restore_for_tests();

    const std::string written = drain(pty.master.get(), 300);
    CHECK_EQ(written, sshos::tty_restore_sequence());

    // Le cœur de la ruling 2 : contre le gestionnaire de référence du plan
    // (qui n'appelle jamais tcsetattr()), ces deux CHECK échoueraient --
    // ICANON/ECHO resteraient à plat, le pseudo-terminal serait laissé en
    // mode brut après un "plantage".
    termios restored{};
    REQUIRE_EQ(::tcgetattr(pty.slave.get(), &restored), 0);
    CHECK((restored.c_lflag & ICANON) != 0);
    CHECK((restored.c_lflag & ECHO) != 0);
  }
}

// --- Ruling 4 : connect_with_timeout() doit abandonner avec un diagnostic
// --- nommant le délai plutôt que de bloquer indéfiniment, ce que fait
// --- connect_abstract() nu (référence du plan) contre un backlog qui ne se
// --- vide jamais -- confirmé hors suite, en dehors de ce dépôt : un
// --- processus de sonde reproduisant exactement bind_abstract()/
// --- connect_abstract() reste bloqué après 4 s (timeout 4, code de sortie
// --- 124) contre un backlog plein qui ne se vide jamais.

TEST(client_connect_with_timeout_gives_up_promptly_when_the_backlog_never_frees) {
  const std::string name = unique_socket_name("timeout");
  Fd listener = sshos::bind_abstract(name);

  // Remplit le backlog et ne l'accepte jamais : connect_with_timeout() ne
  // doit trouver aucune place libre pendant toute la durée du test.
  std::vector<Fd> queued;
  for (int i = 0; i < 64; ++i) {
    Fd c = queue_pending_connection(name);
    if (!c.valid()) break;
    queued.push_back(std::move(c));
  }
  CHECK(!queued.empty());  // sinon le reste du test ne prouve rien

  const auto t0 = std::chrono::steady_clock::now();
  bool threw = false;
  std::string what;
  try {
    Fd fd = sshos::connect_with_timeout(name, 150);
    (void)fd;
  } catch (const std::exception& e) {
    threw = true;
    what = e.what();
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

  CHECK(threw);
  // Le diagnostic doit nommer le délai : quelqu'un qui le lit doit
  // comprendre que c'est une connexion qui a expiré, pas une erreur réseau
  // quelconque.
  CHECK(what.find("150") != std::string::npos);
  // Marge large pour un CI chargé, mais sans commune mesure avec un blocage
  // indéfini : ce test échouerait (au lieu de simplement dépasser sa marge)
  // contre connect_abstract() nu, qui ne revient jamais ici.
  CHECK(elapsed_ms < 2000);
}

// Cas de contrôle : quand une place est disponible, connect_with_timeout()
// doit réussir tout de suite, pas seulement échouer proprement au bout du
// délai. Sans ce test, une implémentation qui attendrait bêtement le délai
// complet avant de tenter quoi que ce soit passerait quand même le test
// précédent.
TEST(client_connect_with_timeout_succeeds_quickly_when_room_is_available) {
  const std::string name = unique_socket_name("room");
  Fd listener = sshos::bind_abstract(name);

  const auto t0 = std::chrono::steady_clock::now();
  bool threw = false;
  Fd client;
  try {
    client = sshos::connect_with_timeout(name, 5000);
  } catch (const std::exception&) {
    threw = true;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

  CHECK(!threw);
  CHECK(client.valid());
  CHECK(elapsed_ms < 500);

  const sshos::AcceptResult r = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(r.outcome == sshos::AcceptOutcome::Accepted);
}
