#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
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

TEST(net_accept_peer_reports_a_genuine_error_distinctly) {
  // Un descripteur qui n'est pas un socket en écoute : accept4() échoue
  // réellement (ENOTSOCK). Ni "rien en attente", ni un refus d'uid : la
  // troisième case que l'ancien code écrasait aussi sous un Fd() invalide.
  Fd not_a_listener(::open("/dev/null", O_RDONLY));
  CHECK(not_a_listener.valid());
  const AcceptResult r = sshos::accept_peer(not_a_listener.get(), ::getuid());
  CHECK(r.outcome == AcceptOutcome::Error);
  CHECK(!r.fd.valid());
  CHECK(r.err != 0);
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
  const std::string id = sshos::read_boot_id();
  CHECK(!id.empty());
  // Sur un système normal (pas un conteneur restreint), la source noyau est
  // lisible : vérifie qu'on ne bascule pas sur le repli alors qu'on n'en a
  // pas besoin.
  CHECK(id.rfind("btime-", 0) != 0);
}

TEST(net_boot_id_falls_back_to_btime_when_the_kernel_source_is_absent) {
  // Remplace l'ancien test vacueux ("non vide" est vrai par construction vu
  // le repli constant qu'on supprime) : ici on force l'absence de la source
  // primaire, comme le fait un conteneur restreint, et on vérifie que le
  // repli déterministe prend le relais au lieu d'une constante muette.
  const std::string id = sshos::read_boot_id("/does/not/exist/boot_id", "/proc/stat");
  CHECK(id.rfind("btime-", 0) == 0);
}

TEST(net_boot_id_throws_rather_than_inventing_a_constant) {
  // Aucune des deux sources disponible : l'ancien code aurait rendu
  // "nobootid" en silence. On veut un échec bruyant à la place.
  bool threw = false;
  try {
    sshos::read_boot_id("/does/not/exist/boot_id", "/does/not/exist/stat");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
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
