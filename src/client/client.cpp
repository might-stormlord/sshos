#include "client/client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <variant>

#include "client/tty_guard.hpp"
#include "common/net.hpp"
#include "common/proto.hpp"
#include "render/cell.hpp"  // Size

namespace sshos {
namespace {

// Délai maximal accordé à connect_with_timeout() avant d'abandonner. 5 s
// couvre largement le temps qu'un démon met à vider une rafale de
// reconnexions concurrentes (chacune coûte un accept() quasi instantané côté
// noyau) sans faire attendre un utilisateur interactif de façon perceptible
// dans le cas normal, où la connexion aboutit en quelques millisecondes.
constexpr int kConnectTimeoutMs = 5000;

// Intervalle entre deux tentatives de connexion. Voir connect_with_timeout()
// : un socket UNIX abstrait dont le backlog est plein rend EAGAIN
// immédiatement (pas d'état "connexion en cours" à surveiller avec poll(),
// contrairement à TCP), donc la seule option est de retenter -- ce délai
// borne la fréquence de ces tentatives sans allonger sensiblement le temps
// perçu quand une place se libère.
constexpr int kConnectRetryIntervalMs = 25;

volatile sig_atomic_t g_winch = 0;

extern "C" void on_winch(int) { g_winch = 1; }

Size term_size(int fd) {
  winsize ws{};
  if (::ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return Size{80, 24};
  return Size{ws.ws_col, ws.ws_row};
}

bool write_all(int fd, std::string_view s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Duplique délibérément common/net.cpp:fill() (anonyme, non exportée) plutôt
// que de changer la signature de connect_abstract() ou d'exposer fill() --
// net.cpp est un fichier partagé, hors du périmètre de cette tâche. Même
// convention que tests/test_net.cpp:try_queue_connection(), qui duplique
// aussi ce calcul pour les mêmes raisons.
socklen_t fill_abstract_addr(sockaddr_un& addr, std::string_view name) {
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 > sizeof addr.sun_path) {
    throw std::runtime_error("nom de socket trop long");
  }
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

// common/fd.hpp expose set_nonblock() mais pas l'inverse : cette fonction
// n'a besoin d'exister qu'ici, une fois la connexion établie, pour rendre le
// descriptif à son état bloquant attendu par le reste de run_client() (tous
// les write_all()/read() qui suivent supposent des appels bloquants).
void clear_nonblock(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags == -1) throw_errno("fcntl F_GETFL");
  if (::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) throw_errno("fcntl F_SETFL");
}

}  // namespace

Fd connect_with_timeout(std::string_view socket_name, int timeout_ms) {
  // timeout_ms <= 0 : la deadline calculée ci-dessous tombe immédiatement
  // (au plus tôt, dans le passé). Comportement obtenu, volontairement non
  // spécial-casé : une tentative de connexion est toujours faite avant tout
  // contrôle de délai -- si elle réussit du premier coup (place libre), le
  // résultat est un succès malgré un délai nul ou négatif ; si elle échoue
  // avec EAGAIN, le test `now >= deadline` plus bas est déjà vrai et la
  // fonction échoue sans jamais appeler poll(), donc sans jamais attendre.
  // Convention raisonnable ("délai nul ou négatif" == "une chance, pas
  // deux"), mais rien dans la signature ne la suggère : voir
  // client_connect_with_timeout_zero_fails_immediately_without_waiting dans
  // tests/test_tty.cpp.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    // Un socket neuf à chaque tentative. Ce n'est PAS parce qu'un socket
    // UNIX abstrait ayant essuyé un EAGAIN sur connect() ne redeviendrait
    // jamais connectable -- affirmation fausse, sondée à part de ce dépôt
    // (3 essais sur 3, sur ce noyau) : backlog saturé jusqu'à EAGAIN, file
    // entièrement vidée côté serveur, puis connect() rappelé sur CE MÊME
    // descripteur -- succès, et la connexion transporte réellement des
    // données (aller-retour write/read vérifié). La vraie raison est POSIX :
    // si connect() échoue, l'état du socket devient non spécifié, et une
    // application portable doit donc fermer le descripteur et en ouvrir un
    // neuf avant de retenter, plutôt que de compter sur un comportement de
    // fait -- aussi reproductible soit-il sur ce noyau précis -- que la
    // norme ne garantit pas.
    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (raw < 0) throw_errno("socket");
    Fd fd(raw);

    sockaddr_un addr{};
    const socklen_t len = fill_abstract_addr(addr, socket_name);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) == 0) {
      clear_nonblock(fd.get());
      return fd;
    }
    if (errno == EINTR) continue;  // fd se referme (sortie de portee), on repart avec un neuf
    if (errno != EAGAIN && errno != EWOULDBLOCK) throw_errno("connect");

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error("delai de connexion au demon depasse (" +
                                std::to_string(timeout_ms) + " ms) : file d'attente du demon " +
                                "probablement pleine");
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int wait_ms = static_cast<int>(
        remaining_ms < kConnectRetryIntervalMs ? remaining_ms : kConnectRetryIntervalMs);
    // poll(nullptr, 0, wait_ms) : minuteur POSIX portable, sans descripteur
    // à surveiller -- il n'y en a aucun qui vaille la peine ici, voir
    // ci-dessus. Retour ignoré à dessein plutôt que par oubli : un retour
    // prématuré (EINTR) ferait juste boucler un peu plus tôt sur le calcul
    // de `remaining_ms` ci-dessus, qui repose sur l'horloge et se recale
    // donc de lui-même au tour suivant -- sans conséquence réelle ici,
    // puisqu'aucun gestionnaire de signal n'est encore installé à ce stade
    // (TtyGuard::install_crash_handlers() et le gestionnaire SIGWINCH
    // n'arrivent qu'après une connexion réussie, dans run_client()).
    const int wait_rc = ::poll(nullptr, 0, wait_ms);
    (void)wait_rc;
  }
}

int run_client(std::string_view socket_name) {
  Fd sock;
  try {
    sock = connect_with_timeout(socket_name, kConnectTimeoutMs);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sshos: connexion au demon impossible : %s\n", e.what());
    return 1;
  }

  // Un démon qui meurt ou ferme la connexion pendant qu'on lui écrit fait
  // échouer le write() avec EPIPE -- géré normalement par write_all() qui
  // rend false, déclenchant un `break` et donc le déroulement normal des
  // destructeurs (TtyGuard restaure le terminal). Sans cette ligne, la
  // disposition par défaut de SIGPIPE termine le processus immédiatement,
  // sans dérouler la pile : aucun destructeur ne s'exécute, et c'est
  // précisément la garantie que cette tâche existe pour fournir qui saute.
  ::signal(SIGPIPE, SIG_IGN);

  TtyGuard::install_crash_handlers();
  TtyGuard guard(STDIN_FILENO);
  ::signal(SIGWINCH, on_winch);

  const Size sz = term_size(STDIN_FILENO);
  Hello hello;
  hello.cols = static_cast<uint16_t>(sz.w);
  hello.rows = static_cast<uint16_t>(sz.h);
  if (const char* t = std::getenv("TERM")) hello.term = t;
  if (const char* c = std::getenv("COLORTERM")) hello.colorterm = c;
  const char* lang = std::getenv("LC_ALL");
  if (lang == nullptr) lang = std::getenv("LANG");
  hello.utf8 = lang != nullptr && std::string(lang).find("UTF-8") != std::string::npos;
  hello.env = collect_env_delta();
  if (!write_all(sock.get(), encode(Msg{hello}))) return 1;

  Decoder dec;
  std::string in_buf(65536, '\0');
  int rc = 0;

  for (;;) {
    if (g_winch != 0) {
      g_winch = 0;
      const Size s = term_size(STDIN_FILENO);
      Resize r;
      r.cols = static_cast<uint16_t>(s.w);
      r.rows = static_cast<uint16_t>(s.h);
      if (!write_all(sock.get(), encode(Msg{r}))) {
        // Écriture vers un démon peut-être mort : même ambiguïté qu'un
        // rc=0 muet pour les autres pannes d'écriture/lecture ci-dessous --
        // indiscernable d'un détachement propre pour l'utilisateur comme
        // pour un script qui inspecte le code de retour.
        std::fprintf(stderr, "\r\nsshos: envoi du redimensionnement au demon impossible : %s\r\n",
                     std::strerror(errno));
        rc = 1;
        break;
      }
    }

    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sock.get(), POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      // Erreur réelle de poll() (EBADF, ENOMEM...), pas simplement "aucun
      // évènement" : la référence initiale sortait ici en silence avec
      // rc=0, indiscernable d'un détachement propre. EINVAL/EBADF signalent
      // un vrai défaut plutôt qu'une condition attendue.
      std::fprintf(stderr, "\r\nsshos: poll() a echoue : %s\r\n", std::strerror(errno));
      rc = 1;
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      const ssize_t n = ::read(STDIN_FILENO, in_buf.data(), in_buf.size());
      if (n <= 0) break;
      // Écriture bloquante vers le démon : si son tampon de réception est
      // plein (rendu trop lent côté démon, ou utilisateur/pty qui produit
      // plus vite que le démon ne consomme), ce write() attend -- et
      // pendant ce temps le client ne lit plus rien non plus, alors que le
      // démon peut lui-même être bloqué à écrire vers CE client si son
      // propre tampon de sortie est plein : un blocage mutuel est possible
      // dans les deux sens. Contre-pression et cadence de rendu (tâche 12) :
      // hors sujet ici, délibérément non traité par cette tâche.
      const bool sent = write_all(
          sock.get(), encode(Msg{Input{in_buf.substr(0, static_cast<size_t>(n))}}));
      if (!sent) {
        // Même panne, même traitement que le redimensionnement ci-dessus :
        // un rc=0 muet se confondrait avec un détachement propre.
        std::fprintf(stderr, "\r\nsshos: envoi de l'entree au demon impossible : %s\r\n",
                     std::strerror(errno));
        rc = 1;
        break;
      }
    }

    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t n = ::read(sock.get(), in_buf.data(), in_buf.size());
      if (n < 0) {
        // Là aussi la référence sortait en silence avec rc=0 : une erreur de
        // lecture réelle (ECONNRESET...) est pourtant distincte d'un
        // détachement propre annoncé par le protocole (message Detached).
        std::fprintf(stderr, "\r\nsshos: lecture depuis le demon impossible : %s\r\n",
                     std::strerror(errno));
        rc = 1;
        break;
      }
      if (n == 0) {
        // Le démon a fermé la connexion sans passer par Detached (crash,
        // kill -9...). Un exit silencieux à 0 masquerait cette différence à
        // l'utilisateur comme à tout script qui inspecte le code de retour.
        std::fprintf(stderr,
                     "\r\nsshos: le demon a ferme la connexion sans annoncer de detachement\r\n");
        rc = 1;
        break;
      }
      dec.feed(std::string_view(in_buf.data(), static_cast<size_t>(n)));
      bool stop = false;
      while (auto m = dec.next()) {
        if (const auto* f = std::get_if<FrameMsg>(&*m)) {
          if (!write_all(STDOUT_FILENO, f->ansi)) {
            std::fprintf(stderr, "\r\nsshos: ecriture vers stdout impossible : %s\r\n",
                         std::strerror(errno));
            rc = 1;
            stop = true;
            break;
          }
        } else if (const auto* d = std::get_if<Detached>(&*m)) {
          // La seule raison qui porte un comportement. Comparaison par
          // ÉGALITÉ sur une constante partagée : le démon vient de poser un
          // binaire neuf et s'arrête pour qu'on reparte dessus.
          if (d->reason == kDetachReasonUpdate) {
            rc = kClientRestartRequested;
            stop = true;
            break;
          }
          std::fprintf(stderr, "\r\nsshos: detache (%s)\r\n", d->reason.c_str());
          stop = true;
          break;
        } else if (const auto* i = std::get_if<Incompatible>(&*m)) {
          std::fprintf(stderr, "\r\nsshos: %s\r\n", i->reason.c_str());
          rc = 1;
          stop = true;
          break;
        }
      }
      // Item 1 (voir le rapport de tâche) : même défaut que côté démon --
      // Decoder::failed() (proto.hpp) documente qu'un pair qui viole le
      // protocole laisse la connexion marquée en échec définitif, à charge
      // pour l'appelant de la fermer. Personne ne le faisait ici non plus :
      // next() ci-dessus aurait fini par ne plus jamais rien renvoyer sans
      // que la boucle ne s'arrête, gelant le client en silence sur un démon
      // qui ne dira plus jamais rien de valide. Terminer proprement (rc=1,
      // destructeurs déroulés -- TtyGuard restaure le terminal) plutôt que
      // de reboucler indéfiniment sur un décodeur inerte.
      if (!stop && dec.failed()) {
        std::fprintf(stderr, "\r\nsshos: message de protocole invalide recu du demon\r\n");
        rc = 1;
        stop = true;
      }
      if (stop) break;
    }
  }
  return rc;
}

}  // namespace sshos
