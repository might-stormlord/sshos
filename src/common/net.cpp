#include "common/net.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Classe une erreur de accept4()/getsockopt() (identifiée par son errno)
// comme transitoire (vaut la peine d'être retentée, typiquement après une
// pression sur les ressources qui a de bonnes chances de se résorber toute
// seule) ou permanente (le même appel, refait à l'identique sur le même
// descripteur, échouera indéfiniment).
//
// EMFILE/ENFILE sont des limites de descripteurs (processus / système) ;
// ENOBUFS/ENOMEM sont regroupées par accept(2) lui-même sous une seule
// entrée ("Not enough free memory") -- même condition de fond, la mémoire
// disponible peut varier d'un appel au suivant. Tout le reste (ENOTSOCK,
// EBADF, EINVAL en tête -- l'écouteur lui-même mal formé ou fermé) est
// permanent par défaut : un errno non reconnu ici est délibérément classé
// permanent plutôt que transitoire, parce qu'une boucle qui s'arrête à tort
// laisse une ligne dans le journal, alors qu'une boucle qui retente à tort
// une condition inconnue reproduit la boucle chaude que cette classification
// existe pour éliminer.
bool is_transient_errno(int err) {
  switch (err) {
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
      return true;
    default:
      return false;
  }
}

// Rend la longueur d'adresse à passer à bind/connect. Pour une adresse
// abstraite elle s'arrête au dernier octet du nom : pas de terminateur.
socklen_t fill(sockaddr_un& addr, std::string_view name) {
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 > sizeof addr.sun_path) {
    throw std::runtime_error("nom de socket trop long");
  }
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

Fd make_socket() {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw_errno("socket");
  return Fd(fd);
}

}  // namespace

std::string read_boot_id(std::string_view boot_id_path) {
  // Échappatoire explicite en premier : voir le contrat documenté sur
  // kBootIdEnvVar dans net.hpp. Une variable définie mais vide est traitée
  // comme absente plutôt que comme un identifiant valide -- un export raté
  // (`SSHOS_BOOT_ID=`) ne doit pas produire silencieusement un identifiant
  // partagé par toute la machine.
  if (const char* forced = std::getenv(kBootIdEnvVar); forced != nullptr && *forced != '\0') {
    return forced;
  }

  std::string id;
  {
    std::ifstream in{std::string(boot_id_path)};
    in >> id;
  }
  if (!id.empty()) return id;

  // Ancienne version de cette fonction : repli sur le "btime" de
  // /proc/stat, retiré. La justification écrite à l'époque ("lu à
  // l'identique par tout processus du même boot") est fausse -- btime n'est
  // pas une valeur stockée une fois pour toutes au démarrage, le noyau le
  // recalcule à chaque lecture comme CLOCK_REALTIME - CLOCK_BOOTTIME. Tout
  // pas d'horloge murale entre le démarrage du démon et l'attache d'un
  // client (resynchronisation NTP, absence d'horloge matérielle fiable au
  // premier démarrage...) déplace donc btime pour tous les processus de la
  // machine, et le démon comme chaque client recalculent le nom du socket
  // indépendamment : un déplacement fait chercher au client un nom que
  // personne n'écoute, indiscernable du cas ordinaire "pas de démon". La
  // session en cours est perdue en silence -- exactement ce que ce
  // sous-système existe pour permettre de retrouver.
  //
  // Aucun autre repli ambiant ne remplace celui-ci (l'inode de l'espace de
  // noms réseau, notamment, a été envisagée et écartée : immunisée contre
  // les sauts d'horloge, mais assignée par une séquence d'initialisation du
  // noyau qui la rend très probablement identique après un simple
  // redémarrage sur la machine hôte -- elle échouerait alors à chaque
  // redémarrage plutôt que seulement quand l'horloge saute). Une constante
  // fixe serait pire encore : identique à chaque redémarrage, elle
  // réintroduirait la confusion de référence périmée que boot_id existe
  // pour éviter -- au pire endroit possible, puisque c'est précisément là
  // que cette lecture est susceptible d'échouer. Échec bruyant à la place ;
  // kBootIdEnvVar reste l'unique échappatoire, et son identité entre le
  // démon et les clients est un contrat explicite porté par l'opérateur qui
  // la définit, pas une propriété ambiante supposée.
  throw std::runtime_error("aucun identifiant de demarrage stable disponible (" +
                            std::string(boot_id_path) + " inaccessible, et la variable " +
                            std::string(kBootIdEnvVar) + " n'est pas definie)");
}

std::string socket_name(uid_t uid, std::string_view boot_id) {
  return "sshos/" + std::to_string(uid) + "/" + std::string(boot_id);
}

Fd bind_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    if (errno == EADDRINUSE) throw AddressInUse();
    throw_errno("bind");
  }
  // Une adresse abstraite n'a pas de permissions : la file d'attente se
  // remplit *avant* le contrôle d'uid dans accept_peer(), donc n'importe
  // quel processus local peut y déposer des connexions jamais acceptées.
  // Avec un backlog de 4, un tel processus (ou même une rafale légitime de
  // clients qui se reconnectent en même temps) suffit à faire bloquer
  // indéfiniment le connect_abstract() suivant d'un client honnête, qui n'a
  // pas de délai d'attente à lui. 16 absorbe une rafale normale sans être
  // démesuré : une connexion en attente ne coûte presque rien pour un socket
  // UNIX.
  // Reste ouvert : un processus hostile peut remplir n'importe quel backlog
  // fini, aussi grand soit-il. Ceci ne fait que reculer le seuil d'attaque
  // trivial, pas le fermer -- le vrai remède est un délai de connexion côté
  // client (tâche 10, boucle client), pas une valeur ici.
  if (::listen(fd.get(), 16) != 0) throw_errno("listen");
  return fd;
}

Fd connect_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    throw_errno("connect");
  }
  return fd;
}

AcceptResult accept_peer(int listen_fd, uid_t expected_uid) {
  int raw = -1;
  for (;;) {
    raw = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (raw >= 0) break;
    // Appel interrompu par un signal : ni "rien en attente" (une connexion
    // peut très bien être là), ni une erreur réelle. On refait l'appel.
    if (errno == EINTR) continue;
    // Pair qui a rompu la connexion après être entré dans la file d'écoute
    // mais avant d'être accepté : bénin et attendu (voir net.hpp), pas un
    // signe que l'écouteur est cassé. D'autres connexions peuvent être
    // derrière dans la file, donc on refait l'appel plutôt que de rendre
    // Empty ou une erreur.
    if (errno == ECONNABORTED) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return AcceptResult{AcceptOutcome::Empty, Fd(), {}, 0};
    }
    const AcceptOutcome outcome =
        is_transient_errno(errno) ? AcceptOutcome::TransientError : AcceptOutcome::FatalError;
    return AcceptResult{outcome, Fd(), {}, errno};
  }
  Fd fd(raw);

  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    const AcceptOutcome outcome =
        is_transient_errno(errno) ? AcceptOutcome::TransientError : AcceptOutcome::FatalError;
    return AcceptResult{outcome, Fd(), {}, errno};
  }

  const PeerCredentials peer{cred.pid, cred.uid, cred.gid};
  if (cred.uid != expected_uid) {
    // fd se referme ici (sortie de portee sans deplacement) : un pair
    // refuse ne garde pas sa connexion ouverte.
    return AcceptResult{AcceptOutcome::Rejected, Fd(), peer, 0};
  }
  return AcceptResult{AcceptOutcome::Accepted, std::move(fd), peer, 0};
}

pid_t peer_pid(int fd) {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
  return static_cast<pid_t>(cred.pid);
}

}  // namespace sshos
