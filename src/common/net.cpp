#include "common/net.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
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

// Repli déterministe pour read_boot_id() : l'heure de démarrage du noyau
// (secondes depuis l'époque), telle que rapportée par la ligne "btime" de
// /proc/stat. Ce n'est pas un uuid, mais c'est lu à l'identique par tout
// processus du même boot -- exactement la propriété qui compte ici, puisque
// le nom du socket est recalculé indépendamment par le démon et par chaque
// client. Un préfixe distingue ce repli d'un vrai boot_id, par hygiène,
// même si rien n'en dépend fonctionnellement.
std::string read_btime_fallback(std::string_view proc_stat_path) {
  std::ifstream in{std::string(proc_stat_path)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("btime ", 0) == 0) {
      const std::string value = line.substr(6);
      if (!value.empty()) return "btime-" + value;
      break;
    }
  }
  return {};
}

}  // namespace

std::string read_boot_id(std::string_view boot_id_path, std::string_view proc_stat_path) {
  std::string id;
  {
    std::ifstream in{std::string(boot_id_path)};
    in >> id;
  }
  if (!id.empty()) return id;

  id = read_btime_fallback(proc_stat_path);
  if (!id.empty()) return id;

  // Aucune des deux sources n'est exploitable (conteneur très restreint,
  // /proc masqué...). Une constante fixe serait pire que l'échec : elle
  // serait identique à chaque redémarrage et réintroduirait exactement la
  // confusion de référence périmée que boot_id existe pour éviter -- au
  // pire endroit possible, puisque c'est précisément là que cette lecture
  // est susceptible d'échouer.
  throw std::runtime_error("aucun identifiant de demarrage stable disponible (" +
                            std::string(boot_id_path) + " et " +
                            std::string(proc_stat_path) + " inaccessibles)");
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
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return AcceptResult{AcceptOutcome::Empty, Fd(), {}, 0};
    }
    return AcceptResult{AcceptOutcome::Error, Fd(), {}, errno};
  }
  Fd fd(raw);

  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return AcceptResult{AcceptOutcome::Error, Fd(), {}, errno};
  }

  const PeerCredentials peer{cred.pid, cred.uid, cred.gid};
  if (cred.uid != expected_uid) {
    // fd se referme ici (sortie de portee sans deplacement) : un pair
    // refuse ne garde pas sa connexion ouverte.
    return AcceptResult{AcceptOutcome::Rejected, Fd(), peer, 0};
  }
  return AcceptResult{AcceptOutcome::Accepted, std::move(fd), peer, 0};
}

}  // namespace sshos
