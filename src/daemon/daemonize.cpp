#include "daemon/daemonize.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "common/oom.hpp"

namespace sshos {
namespace {

void reset_signal_state() {
  // Le masque survit à execve : le laisser en place casse tout enfant qui
  // attend ses propres processus.
  sigset_t empty;
  sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);

  // Les dispositions SIG_IGN survivent aussi : un SIGPIPE ignoré hérité
  // fait que `yes | head -1` ne s'arrête jamais.
  for (int sig = 1; sig < NSIG; ++sig) {
    if (sig == SIGKILL || sig == SIGSTOP) continue;
    ::signal(sig, SIG_DFL);
  }
}

void redirect_std_to_devnull() {
  const int null_fd = ::open("/dev/null", O_RDWR);
  // Sans consequence si /dev/null est indisponible (chroot degrade, par
  // exemple) : les fds 0/1/2 restent simplement ceux herites de l'appelant
  // au lieu d'etre redecouples vers un point mort.
  if (null_fd < 0) return;
  ::dup2(null_fd, STDIN_FILENO);
  ::dup2(null_fd, STDOUT_FILENO);
  ::dup2(null_fd, STDERR_FILENO);
  if (null_fd > STDERR_FILENO) ::close(null_fd);
}

}  // namespace

pid_t spawn_detached(const std::vector<std::string>& argv) {
  // argv vide : raw[0] lirait le nullptr terminal ci-dessous et le
  // passerait à execv. Rien à exécuter, donc rien à forker -- même signal
  // d'échec que l'échec du fork() qui suit (voir daemonize.hpp).
  if (argv.empty()) return -1;

  const pid_t first = ::fork();
  if (first != 0) return first;  // parent : rend le pid a recolter (ou -1)

  // Enfant intermédiaire.
  if (::setsid() == static_cast<pid_t>(-1)) ::_exit(127);

  const pid_t second = ::fork();
  if (second != 0) ::_exit(second < 0 ? 127 : 0);

  // Petit-enfant : le démon.
  if (::chdir("/") != 0) ::_exit(127);
  redirect_std_to_devnull();
  reset_signal_state();

  // PAS de signal(SIGHUP, SIG_IGN) ici, contrairement à la version initiale
  // de cette fonction. SIG_IGN survit à execv (voir reset_signal_state
  // ci-dessus, sur SIGPIPE hérité) et spawn_detached() lance un argv
  // arbitraire fourni par l'appelant -- lui imposer une politique de
  // signaux figée ferait fuiter une décision du démon dans n'importe quel
  // programme lancé, y compris un programme qui ne devrait jamais ignorer
  // SIGHUP. Vérifié empiriquement : SigIgn du programme exécuté passe de
  // 0000000000000001 à 0000000000000000 une fois cette ligne retirée.
  //
  // Aucune fenêtre n'est ouverte par ce retrait. SIGHUP sur mort du
  // processus de contrôle n'est délivré que si la session possède un
  // terminal de contrôle ; setsid() ci-dessus vient de créer une session
  // qui n'en a aucun -- c'est précisément sa raison d'être. La mort
  // immédiate de l'intermédiaire, juste après ce second fork(), ne génère
  // donc aucun SIGHUP à ignorer : il n'y a rien entre execv() et
  // become_daemon() dont ce dernier aurait besoin de se protéger.
  // become_daemon() reste l'endroit qui pose SIG_IGN sur SIGHUP : c'est le
  // démon lui-même qui a besoin de cette politique, pas son lanceur
  // générique.

  // Le reglage du tueur de memoire s'herite et survit a execve : ce qu'on
  // lance detache est un processus ORDINAIRE, jamais un heritier de
  // l'immunite du demon (common/oom.hpp).
  drop_oom_protection();

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const auto& s : argv) raw.push_back(const_cast<char*>(s.c_str()));
  raw.push_back(nullptr);

  ::execv(raw[0], raw.data());
  ::_exit(127);
}

void become_daemon() {
  if (::chdir("/") != 0) { /* sans consequence si le cwd est deja valide */ }
  redirect_std_to_devnull();
  ::signal(SIGHUP, SIG_IGN);
  ::signal(SIGPIPE, SIG_IGN);  // un client qui meurt ne doit pas tuer le démon

  // Piège d'intégration, à ne PAS résoudre ici (tâche 13) : read_boot_id()
  // (src/common/net.hpp) lève std::runtime_error en cas d'échec total --
  // décision prise en tâche 8, car un repli silencieux ferait calculer un
  // nom de socket différent au démon et à chacun de ses clients,
  // orphelinant la session en cours. Si le démarrage du démon appelle
  // read_boot_id() après become_daemon(), ce throw atterrit une fois
  // stderr déjà redirigé vers /dev/null ci-dessus : le message est perdu,
  // silencieusement. L'ordre entre become_daemon() et le reste du
  // démarrage du démon appartient à la tâche 13. Ne pas ajouter de
  // dispositif de journalisation ici pour contourner le problème.
}

std::string daemon_exe_path() {
  if (const char* p = std::getenv("TERMOS_EXE")) {
    if (*p != '\0') return p;
  }
  return "/proc/self/exe";
}

}  // namespace sshos
