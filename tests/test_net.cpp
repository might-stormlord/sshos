#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "common/fd.hpp"
#include "common/net.hpp"
#include "harness.hpp"

using sshos::AcceptOutcome;
using sshos::AcceptResult;
using sshos::Fd;

namespace {

// L'espace de noms des sockets abstraites est global pour tout un espace de
// noms réseau, alors qu'un pid n'est unique que dans son propre espace de
// noms pid. Deux copies de cette suite lancées dans des conteneurs pid
// distincts (ex: `unshare --pid --fork --mount-proc`) mais un même espace de
// noms réseau peuvent donc très bien porter le même pid : c'est exactement
// le scénario qui a fait planter la suite avec une AddressInUse non
// rattrapée. Tirer un aléa frais à chaque appel, à partir d'une source
// d'entropie du noyau, rend une collision aussi improbable qu'un pid
// partagé l'est peu -- contrairement au pid seul, qui collide à coup sûr.
std::string unique_name() {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "sshos-test/" << ::getpid() << '-' << std::hex << dist(rng);
  return os.str();
}

// Sonde de connexion indépendante de connect_abstract() : elle doit pouvoir
// constater qu'elle bloquerait au lieu de bloquer réellement, donc le socket
// client est non bloquant. connect_abstract() reste délibérément bloquant
// pour un vrai client (son délai d'attente est une préoccupation à part,
// tâche 10) ; d'où cette petite duplication locale plutôt qu'un changement
// de comportement de l'API testée.
Fd try_queue_connection(const std::string& name) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    CHECK(false);  // socket() ne devrait jamais echouer ici
    return Fd();
  }
  Fd guard(fd);

  sockaddr_un addr{};
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  const socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());

  if (::connect(guard.get(), reinterpret_cast<sockaddr*>(&addr), len) == 0) {
    return guard;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) return Fd();
  CHECK(false);  // connect() a echoue de facon inattendue
  return Fd();
}

// Restaure une variable d'environnement à sa valeur d'avant le test, qu'elle
// ait été définie ou non. Tous les test_*.cpp sont liés dans le même binaire
// et tournent dans le même processus (voir tests/main.cpp) : sans ça, un
// test qui appelle setenv() sur SSHOS_BOOT_ID ferait fuiter cette valeur
// vers tous les cas suivants du run, et l'ordre d'exécution se mettrait à
// compter pour le résultat.
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

// Abaisse RLIMIT_NOFILE le temps du test pour forcer un EMFILE déterministe
// sur un accept4() par ailleurs sain (vrai écouteur, vrai pair en attente),
// puis restaure la limite d'origine à la sortie de portée -- y compris si
// une assertion échoue en cours de route, pour ne pas laisser un test suivant
// hériter d'une limite de descripteurs anormalement basse.
class RlimitNofileGuard {
 public:
  RlimitNofileGuard() { ::getrlimit(RLIMIT_NOFILE, &original_); }
  ~RlimitNofileGuard() { ::setrlimit(RLIMIT_NOFILE, &original_); }
  RlimitNofileGuard(const RlimitNofileGuard&) = delete;
  RlimitNofileGuard& operator=(const RlimitNofileGuard&) = delete;

  // Fixe rlim_cur au prochain numéro de descripteur que le noyau attribuerait
  // (le plus petit libre, trouvé en ouvrant puis refermant /dev/null) : le
  // tout prochain appel qui a besoin d'un nouveau descripteur -- ici
  // accept4() -- échoue alors avec EMFILE, sans toucher aux descripteurs déjà
  // ouverts (setrlimit n'en ferme aucun rétroactivement).
  bool lower_to_force_emfile() {
    const int probe = ::open("/dev/null", O_RDONLY);
    if (probe < 0) return false;
    const int next_fd = probe;
    ::close(probe);

    rlimit lim = original_;
    lim.rlim_cur = static_cast<rlim_t>(next_fd);
    return ::setrlimit(RLIMIT_NOFILE, &lim) == 0;
  }

 private:
  rlimit original_{};
};

}  // namespace

TEST(net_unique_name_does_not_depend_on_the_pid_alone) {
  // Deux appels consécutifs partagent forcément le même pid : si le nom ne
  // dépendait que du pid (l'ancien schéma), ils seraient identiques.
  CHECK(unique_name() != unique_name());
}

TEST(net_socket_name_is_stable) {
  CHECK_EQ(sshos::socket_name(1000, "abc"), std::string("sshos/1000/abc"));
}

TEST(net_bind_acts_as_a_mutex) {
  const std::string name = unique_name();
  Fd first = sshos::bind_abstract(name);
  CHECK(first.valid());

  bool threw = false;
  try {
    Fd second = sshos::bind_abstract(name);
  } catch (const sshos::AddressInUse&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_connect_reaches_the_listener_and_peer_uid_matches) {
  const std::string name = unique_name() + "-conn";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  CHECK(client.valid());

  const AcceptResult r = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(r.outcome == AcceptOutcome::Accepted);
  CHECK(r.fd.valid());
  // Ce que l'ancien code perdait même en cas de succès : sans pid/gid du
  // pair, un démon ne peut jamais journaliser qui s'est attaché.
  CHECK_EQ(r.cred.pid, ::getpid());
  CHECK_EQ(r.cred.uid, ::getuid());
}

TEST(net_accept_rejects_a_foreign_uid_but_reports_who) {
  const std::string name = unique_name() + "-uid";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  // Un uid qui n'est certainement pas le nôtre.
  const uid_t bogus = ::getuid() + 4242;
  const AcceptResult r = sshos::accept_peer(listener.get(), bogus);
  CHECK(r.outcome == AcceptOutcome::Rejected);
  CHECK(!r.fd.valid());
  // De quoi écrire une ligne de journal utile : qui a essayé de se
  // connecter, pas seulement "quelqu'un a été refusé".
  CHECK_EQ(r.cred.pid, ::getpid());
  CHECK_EQ(r.cred.uid, ::getuid());
}

TEST(net_accept_peer_reports_empty_when_nothing_is_pending) {
  const std::string name = unique_name() + "-empty";
  Fd listener = sshos::bind_abstract(name);
  sshos::set_nonblock(listener.get());  // sinon accept4() bloquerait le test
  const AcceptResult r = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(r.outcome == AcceptOutcome::Empty);
  CHECK(!r.fd.valid());
}

TEST(net_accept_peer_reports_a_fatal_error_distinctly) {
  // Un descripteur qui n'est pas un socket en écoute : accept4() échoue
  // réellement (ENOTSOCK). Ni "rien en attente", ni un refus d'uid, et une
  // erreur qu'aucune nouvelle tentative ne corrigera jamais -- l'écouteur
  // lui-même est mal formé, pas temporairement indisponible.
  Fd not_a_listener(::open("/dev/null", O_RDONLY));
  CHECK(not_a_listener.valid());
  const AcceptResult r = sshos::accept_peer(not_a_listener.get(), ::getuid());
  CHECK(r.outcome == AcceptOutcome::FatalError);
  CHECK(!r.fd.valid());
  CHECK_EQ(r.err, ENOTSOCK);
}

TEST(net_accept_peer_reports_a_transient_error_distinctly) {
  // Un vrai écouteur avec un vrai pair déjà en attente dans le backlog,
  // mais RLIMIT_NOFILE abaissé juste avant l'appel pour forcer un EMFILE
  // déterministe : accept4() trouve bien la connexion en attente mais
  // échoue à lui allouer un descripteur. Rien n'est cassé dans l'écouteur
  // lui-même -- une nouvelle tentative après que de la place se soit
  // libérée ailleurs dans le processus a une vraie chance de réussir.
  const std::string name = unique_name() + "-emfile";
  Fd listener = sshos::bind_abstract(name);
  Fd pending = try_queue_connection(name);
  CHECK(pending.valid());

  RlimitNofileGuard rlimit_guard;
  CHECK(rlimit_guard.lower_to_force_emfile());

  const AcceptResult r = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(r.outcome == AcceptOutcome::TransientError);
  CHECK(!r.fd.valid());
  CHECK_EQ(r.err, EMFILE);
}

TEST(net_accept_peer_distinguishes_transient_from_fatal_errors) {
  // Le point du correctif, vérifié ici sans nommer TransientError ni
  // FatalError : une cause permanente (ENOTSOCK) et une cause transitoire
  // (EMFILE) doivent produire des `outcome` différents, parce qu'une boucle
  // "journalise puis continue" au-dessus de accept_peer() doit pouvoir
  // réagir différemment aux deux -- boucler à froid sur la première (mesuré
  // contre l'objet compilé : ~144k appels/s en continu) et affamer un
  // accept qui aurait pu réussir sous la seconde (mesuré : ~48k appels/s,
  // zéro acceptation). N'utiliser que la comparaison d'`outcome` (plutôt que
  // les noms des deux nouvelles valeurs) laisse ce cas compiler tel quel
  // contre net.hpp/net.cpp d'avant ce correctif, où les deux valaient la
  // même AcceptOutcome::Error : contre cette version-là, ce CHECK échoue.
  Fd not_a_listener(::open("/dev/null", O_RDONLY));
  CHECK(not_a_listener.valid());
  const AcceptResult fatal = sshos::accept_peer(not_a_listener.get(), ::getuid());
  CHECK_EQ(fatal.err, ENOTSOCK);

  const std::string name = unique_name() + "-classify";
  Fd listener = sshos::bind_abstract(name);
  Fd pending = try_queue_connection(name);
  CHECK(pending.valid());

  RlimitNofileGuard rlimit_guard;
  CHECK(rlimit_guard.lower_to_force_emfile());

  const AcceptResult transient = sshos::accept_peer(listener.get(), ::getuid());
  CHECK_EQ(transient.err, EMFILE);

  CHECK(transient.outcome != fatal.outcome);
}

TEST(net_connect_fails_when_nobody_listens) {
  bool threw = false;
  try {
    Fd f = sshos::connect_abstract(unique_name() + "-absent");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_boot_id_prefers_the_kernel_source_when_available) {
  // Neutralise une SSHOS_BOOT_ID qui traînerait dans l'environnement du run
  // (peu probable, mais sinon ce test dépendrait de l'invocateur) : on veut
  // vérifier la source noyau elle-même, pas l'échappatoire.
  EnvVarGuard env_guard("SSHOS_BOOT_ID");
  ::unsetenv("SSHOS_BOOT_ID");

  const std::string id = sshos::read_boot_id();
  CHECK(!id.empty());
  // Deux lectures dans le même run doivent s'accorder : boot_id ne change
  // pas en cours de vie du noyau.
  CHECK_EQ(id, sshos::read_boot_id());
}

TEST(net_boot_id_throws_when_the_kernel_source_is_absent) {
  // Remplace l'ancien test "tombe sur le repli btime", supprimé avec le
  // repli lui-même (voir net.cpp) : la source noyau absente doit maintenant
  // échouer bruyamment, pas glisser vers une horloge murale qui peut se
  // faire corriger entre le démarrage du démon et l'attache d'un client.
  //
  // Appel à un seul argument délibéré : il reste valide aussi bien contre
  // la signature à deux paramètres d'avant ce correctif (le second prend
  // alors la valeur par défaut "/proc/stat", bien réel, et l'ancien code
  // réussit via le repli qu'on supprime -- ce CHECK échoue) que contre la
  // signature actuelle à un seul paramètre.
  EnvVarGuard env_guard("SSHOS_BOOT_ID");
  ::unsetenv("SSHOS_BOOT_ID");

  bool threw = false;
  try {
    sshos::read_boot_id("/does/not/exist/boot_id");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_boot_id_treats_an_empty_override_as_absent) {
  // Un export raté (`SSHOS_BOOT_ID=` sans valeur) ne doit pas être pris pour
  // un identifiant valide : sinon tout le monde sur la machine se
  // retrouverait avec la même chaîne vide, silencieusement.
  EnvVarGuard env_guard("SSHOS_BOOT_ID");
  ::setenv("SSHOS_BOOT_ID", "", 1);

  bool threw = false;
  try {
    sshos::read_boot_id("/does/not/exist/boot_id");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_boot_id_env_override_takes_priority_over_the_kernel_source) {
  // L'échappatoire documentée sur kBootIdEnvVar (net.hpp) : une valeur
  // définie doit gagner même quand la source noyau, elle, est parfaitement
  // lisible -- sinon un opérateur qui force la valeur pour contourner un
  // conteneur restreint sur une machine, mais teste d'abord sur une machine
  // normale, obtiendrait un résultat différent selon la machine.
  //
  // "SSHOS_BOOT_ID" est répété en toutes lettres plutôt que via
  // sshos::kBootIdEnvVar : uniquement pour ce test, c'est voulu -- rendre le
  // test insensible au nom du symbole (qui n'existe pas dans le net.hpp
  // d'avant ce correctif) est ce qui permet de le compiler tel quel contre
  // l'ancien code et de constater, à l'exécution, qu'il ignorait
  // entièrement la variable : ce CHECK_EQ échoue contre lui.
  EnvVarGuard env_guard("SSHOS_BOOT_ID");
  ::setenv("SSHOS_BOOT_ID", "sentinelle-de-test-3c9f", 1);

  CHECK_EQ(sshos::read_boot_id(), std::string("sentinelle-de-test-3c9f"));
}

TEST(net_listen_backlog_holds_more_than_a_handful_of_pending_clients) {
  const std::string name = unique_name() + "-backlog";
  Fd listener = sshos::bind_abstract(name);

  // Personne n'accepte : on vérifie qu'on peut empiler plus que l'ancien
  // backlog défaillant de 4 sans jamais provoquer un blocage réel (sondes
  // non bloquantes uniquement, cf. try_queue_connection).
  std::vector<Fd> queued;
  constexpr int kTarget = 8;  // > 4 (l'ancienne valeur défaillante)
  for (int i = 0; i < kTarget; ++i) {
    Fd c = try_queue_connection(name);
    CHECK(c.valid());
    if (c.valid()) queued.push_back(std::move(c));
  }
}
