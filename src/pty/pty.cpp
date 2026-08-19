#include "pty/pty.hpp"

#include "common/oom.hpp"

#include <csignal>
#include <cstring>
#include <cerrno>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace sshos {
namespace {

// Le plus grand numéro de signal qu'on remet à SIG_DFL. NSIG couvre les
// temps réel ; les remettre tous coûte trente appels et évite d'avoir à se
// demander lesquels comptent.
constexpr int kMaxSignal = 64;

// Le tampon de ptsname_r. Les noms sont « /dev/pts/NNN » ; on prend large.
constexpr size_t kNameMax = 128;

std::string strerror_fr(int e) { return std::string(std::strerror(e)); }

// Convertit un vecteur de chaînes en tableau à la execve. Fait AVANT le
// fork : entre fork et execve, allouer n'est pas sûr vis-à-vis des
// signaux, et un malloc pris au mauvais moment fige l'enfant pour de bon.
std::vector<char*> flatten(const std::vector<std::string>& v,
                           std::vector<std::string>& storage) {
  storage = v;
  std::vector<char*> out;
  out.reserve(storage.size() + 1);
  for (std::string& s : storage) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

}  // namespace

Pty::~Pty() { shutdown(); }

std::string Pty::spawn(const PtySpawn& s) {
  const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
  if (master < 0) return "impossible d'ouvrir un pseudo-terminal : " + strerror_fr(errno);
  if (::grantpt(master) != 0) {
    const std::string e = strerror_fr(errno);
    ::close(master);
    return "grantpt a echoue : " + e;
  }
  if (::unlockpt(master) != 0) {
    const std::string e = strerror_fr(errno);
    ::close(master);
    return "unlockpt a echoue : " + e;
  }
  char name[kNameMax];
  if (::ptsname_r(master, name, sizeof name) != 0) {
    const std::string e = strerror_fr(errno);
    ::close(master);
    return "ptsname_r a echoue : " + e;
  }

  // Le tuyau de rapport : l'enfant y écrit son errno si execve échoue, et
  // un exec RÉUSSI le referme tout seul, puisqu'il est CLOEXEC. Le parent
  // distingue donc les deux sans délai d'attente ni convention fragile.
  int report[2];
  if (::pipe2(report, O_CLOEXEC) != 0) {
    const std::string e = strerror_fr(errno);
    ::close(master);
    return "pipe2 a echoue : " + e;
  }

  // Tout ce qui alloue est fait ici, avant le fork -- y compris le pointeur
  // du dossier de depart : `c_str()` sur un std::string vide rend une chaine
  // valide, donc l'enfant n'a qu'un octet a regarder pour savoir s'il y a
  // quelque chose a faire.
  const char* const start_dir = s.cwd.c_str();
  std::vector<std::string> argv_store;
  std::vector<std::string> env_store;
  std::vector<char*> argv = flatten(s.argv, argv_store);
  std::vector<char*> env = flatten(s.env, env_store);
  const std::string path = s.path;
  const winsize ws{s.rows, s.cols, 0, 0};

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string e = strerror_fr(errno);
    ::close(master);
    ::close(report[0]);
    ::close(report[1]);
    return "fork a echoue : " + e;
  }

  if (pid == 0) {
    // ---- l'enfant : plus rien n'alloue à partir d'ici ----
    ::close(report[0]);

    // PREMIER héritage : le masque de signaux survit à execve. Le démon
    // bloque SIGCHLD pour le recevoir par signalfd ; en hériter casse
    // « make -j8 », qui n'apprend jamais que ses compilateurs sont morts.
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    // DEUXIÈME héritage : les dispositions SIG_IGN survivent aussi. Le
    // démon met SIGPIPE à SIG_IGN ; un enfant qui en hérite ne s'arrête
    // plus jamais sur un tuyau fermé.
    for (int sig = 1; sig < kMaxSignal; ++sig) {
      struct sigaction dfl {};
      dfl.sa_handler = SIG_DFL;
      sigemptyset(&dfl.sa_mask);
      ::sigaction(sig, &dfl, nullptr);
    }

    // LE DOSSIER DE DEPART, avant tout le reste du décor : rien de ce qui
    // suit n'en dépend, et le faire tôt garde l'échec sans conséquence.
    // On IGNORE l'échec -- un dossier effacé entre deux sessions ne doit pas
    // coûter la fenêtre, seulement l'endroit où elle s'ouvre.
    if (start_dir[0] != '\0') {
      const int ignored_chdir = ::chdir(start_dir);
      (void)ignored_chdir;
    }

    // QUATRIÈME héritage : le réglage du tueur de mémoire survit lui aussi,
    // et à execve() en plus. Le démon se met hors d'atteinte parce qu'il
    // porte toute la session ; un invité qui garderait cette immunité ferait
    // tuer la base de données de la machine à la place du « make -j12 »
    // lancé dans une fenêtre. Remonter à 0 est toujours permis, même sans
    // privilège -- seule la descente est gardée par le noyau.
    drop_oom_protection();

    // Quitter la session du démon, PUIS ouvrir l'esclave : c'est cet ordre
    // qui en fait le terminal de contrôle. L'esclave est ouvert par
    // l'enfant et non passé par le parent, sans quoi le parent le
    // garderait ouvert et le maître ne verrait jamais d'EOF.
    if (::setsid() < 0) {
      const int e = errno;
      const ssize_t ignored = ::write(report[1], &e, sizeof e);
      (void)ignored;
      ::_exit(127);
    }
    // O_NOCTTY, PUIS acquisition explicite. Sous Linux, un chef de
    // session qui ouvre un terminal sans O_NOCTTY l'acquiert par le seul
    // fait de l'ouvrir : s'appuyer là-dessus rendrait le TIOCSCTTY
    // ci-dessous décoratif, et le retirer par erreur ne se verrait pas.
    const int slave = ::open(name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
      const int e = errno;
      const ssize_t ignored = ::write(report[1], &e, sizeof e);
      (void)ignored;
      ::_exit(127);
    }
    // Sans TIOCSCTTY, Ctrl+C ne génère aucun signal et le contrôle de
    // tâches du shell ne fonctionne pas.
    ::ioctl(slave, TIOCSCTTY, 0);
    ::ioctl(slave, TIOCSWINSZ, &ws);
    ::dup2(slave, 0);
    ::dup2(slave, 1);
    ::dup2(slave, 2);
    if (slave > 2) ::close(slave);

    ::execve(path.c_str(), argv.data(), env.data());

    // TROISIÈME héritage : tous les descripteurs du démon sont CLOEXEC, y
    // compris celui-ci -- ce qui est justement pourquoi le parent sait
    // qu'un exec réussi n'écrira rien.
    const int e = errno;
    const ssize_t ignored = ::write(report[1], &e, sizeof e);
    (void)ignored;
    ::_exit(127);
  }

  // ---- le parent ----
  ::close(report[1]);
  int child_errno = 0;
  const ssize_t got = ::read(report[0], &child_errno, sizeof child_errno);
  ::close(report[0]);

  if (got == static_cast<ssize_t>(sizeof child_errno)) {
    // L'enfant a rapporté un échec : on le récolte tout de suite plutôt
    // que de laisser un zombie derrière une erreur.
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    ::close(master);
    return "impossible de lancer « " + path + " » : " + strerror_fr(child_errno);
  }

  master_ = master;
  pid_ = pid;
  return {};
}

void Pty::resize(unsigned short cols, unsigned short rows) {
  if (master_ < 0) return;
  const winsize ws{rows, cols, 0, 0};
  ::ioctl(master_, TIOCSWINSZ, &ws);
}

ssize_t Pty::read(char* buf, size_t n) {
  if (master_ < 0) return -1;
  return ::read(master_, buf, n);
}

ssize_t Pty::write(const char* buf, size_t n) {
  if (master_ < 0) return -1;
  return ::write(master_, buf, n);
}

void Pty::close_master() {
  if (master_ < 0) return;
  ::close(master_);
  master_ = -1;
}

void Pty::hangup() {
  if (pid_ <= 0 || exited_) return;
  // Au GROUPE, pas au seul enfant : un shell a des petits-enfants, et ne
  // prévenir que lui laisse la compilation tourner. Le groupe existe parce
  // que l'enfant a fait setsid().
  ::kill(-pid_, SIGHUP);
}

void Pty::kill_now() {
  if (pid_ <= 0 || exited_) return;
  ::kill(-pid_, SIGKILL);
}

// MESURÉ, sur quatre scénarios, à la destruction d'un `Pty` (13 août 2026).
// L'état est relu dans `/proc` : `kill(pid, 0)` réussit sur un zombie et
// ferait passer un shell mort pour un survivant.
//
//                     fermer le maître seul   + SIGKILL au groupe
//   shell ordinaire         mort                    mort
//   tâche de fond           partie                  partie
//   `trap '' HUP`           VIVANT, pour toujours   mort
//   enfant `setsid`         vivant                  vivant
//
// Trois choses en découlent, et chacune est un cas de `test_pty.cpp` :
//
// 1. `close_master()` SUFFIT au cas ordinaire : le noyau envoie SIGHUP au
//    groupe au premier plan du terminal quand le dernier maître se ferme.
// 2. IL NE SUFFIT PAS à qui refuse le raccrochage. Un tel shell survivait à
//    la fermeture de sa fenêtre -- sans pseudo-terminal, sans fenêtre,
//    injoignable -- pour toute la vie du démon, qui se compte en semaines
//    puisque la session survit à la déconnexion. C'est ce que SIGKILL clôt.
// 3. UN ENFANT QUI A QUITTÉ LA SESSION SURVIT, et c'est voulu : `nohup`,
//    `setsid` et `disown` sortent du groupe, et rien de ce qu'on envoie au
//    groupe ne les atteint. C'est la seule porte de sortie, et l'ôter
//    empêcherait de faire survivre un travail à sa fenêtre.
//
// `hangup()` en tête N'A CHANGÉ AUCUNE des quatre lignes -- ni seul, ni
// devant le SIGKILL. Il est gardé, et c'est une garde NON DISCRIMINABLE
// déclarée comme telle : SIGHUP est le signal qui dit « le terminal est
// parti », un programme correct y répond, et le lui refuser pour n'envoyer
// que SIGKILL serait brutal sans être plus sûr.
void Pty::shutdown() {
  hangup();
  close_master();
  kill_now();
}

bool Pty::try_reap() {
  if (pid_ <= 0 || exited_) return false;
  int status = 0;
  const pid_t got = ::waitpid(pid_, &status, WNOHANG);
  if (got != pid_) return false;
  exited_ = true;
  if (WIFEXITED(status)) {
    code_ = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    signalled_ = true;
    code_ = WTERMSIG(status);
  }
  return true;
}

}  // namespace sshos
