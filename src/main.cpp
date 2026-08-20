#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "client/client.hpp"
#include "client/launch.hpp"
#include "common/net.hpp"
#include "daemon/daemon.hpp"
#include "daemon/daemonize.hpp"

namespace {

std::string current_socket_name() {
  return sshos::socket_name(::getuid(), sshos::read_boot_id());
}

// Vrai si logind tuera les processus de l'utilisateur à la déconnexion,
// auquel cas le démon ne survivra pas malgré le détachement. Le seul cas
// où la fonctionnalité phare du projet échoue sans que rien ne soit cassé
// chez nous : mieux vaut le dire au premier lancement que le laisser
// découvrir à la reconnexion.
bool logind_kills_user_processes() {
  std::ifstream in("/etc/systemd/logind.conf");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#", 0) == 0) continue;
    if (line.find("KillUserProcesses=yes") != std::string::npos) return true;
  }
  return false;
}

int start_daemon_and_connect(const std::string& name) {
  // Par CHEMIN, pas par inode : après une mise à jour, l'inode de ce
  // processus est l'ancienne version (voir daemon_exe_path).
  //
  // L'attente elle-même vit dans src/client/launch.cpp, à portée de la
  // suite de tests : le budget trop court qui a fait perdre un redémarrage
  // était ici, dans le seul fichier qu'aucun test ne peut atteindre.
  const sshos::DaemonLaunch r = sshos::launch_daemon(
      name, sshos::daemon_exe_path(), [] {
        std::fprintf(stderr, "sshos: le demon met du temps a demarrer, patience...\n");
      });
  switch (r) {
    case sshos::DaemonLaunch::Connected:
      return 0;
    case sshos::DaemonLaunch::SpawnFailed:
      std::fprintf(stderr, "sshos: impossible de lancer le demon\n");
      return 1;
    case sshos::DaemonLaunch::TimedOut:
      std::fprintf(stderr, "sshos: le demon n'a pas repondu\n");
      return 1;
  }
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  // A4 : current_socket_name() appelle read_boot_id(), qui lève depuis le
  // durcissement de la tâche 8 (le repli sur btime de /proc/stat a été
  // retiré, instable). C'est la toute première instruction de main() ;
  // sans protection, un échec ici tuerait le binaire par std::terminate au
  // lieu du message clair + code de retour non nul que produisent tous les
  // autres appels faillibles de cette fonction.
  std::string name;
  try {
    name = current_socket_name();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sshos: %s\n", e.what());
    return 1;
  }
  const std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "--daemon") {
    sshos::become_daemon();
    return sshos::run_daemon(name);
  }

  if (mode == "--status") {
    try {
      sshos::Fd s = sshos::connect_abstract(name);
      std::printf("demon actif (pid %d)\n", static_cast<int>(sshos::peer_pid(s.get())));
      return 0;
    } catch (const std::exception&) {
      std::printf("aucun demon\n");
      return 1;
    }
  }

  if (mode == "--kill") {
    try {
      sshos::Fd s = sshos::connect_abstract(name);
      const pid_t pid = sshos::peer_pid(s.get());
      s.reset();
      if (pid > 0 && ::kill(pid, SIGTERM) == 0) {
        std::printf("demon %d arrete\n", static_cast<int>(pid));
        return 0;
      }
    } catch (const std::exception&) {
    }
    std::printf("aucun demon\n");
    return 1;
  }

  if (!mode.empty()) {
    std::fprintf(stderr, "usage: sshos [--daemon|--status|--kill]\n");
    return 2;
  }

  // Mode normal : attacher, en démarrant le démon s'il n'existe pas.
  //
  // DEUX TOURS AU PLUS. Un démon qui s'arrête pour se mettre à jour nous
  // rend la main avec kClientRestartRequested : on repart alors sur le
  // binaire NEUF, que `daemon_exe_path()` désigne par chemin et non par
  // inode. Un seul tour de plus, jamais une boucle : si le redémarrage
  // échoue, mieux vaut rendre la main au shell avec un message que tourner.
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      sshos::Fd probe = sshos::connect_abstract(name);
      probe.reset();
    } catch (const std::exception&) {
      // L'avertissement logind n'a de sens qu'au premier lancement : le
      // répéter après une mise à jour serait du bruit.
      if (attempt == 0 && logind_kills_user_processes()) {
        std::fprintf(stderr,
                     "sshos: attention, logind est configure avec "
                     "KillUserProcesses=yes ;\n        vos fenetres ne "
                     "survivront pas a la deconnexion.\n        Parade : "
                     "loginctl enable-linger %d\n",
                     static_cast<int>(::getuid()));
      }
      if (start_daemon_and_connect(name) != 0) return 1;
    }

    const int rc = sshos::run_client(name);
    if (rc != sshos::kClientRestartRequested) return rc;

    std::fprintf(stderr, "sshos: mise a jour installee, redemarrage...\n");
  }

  std::fprintf(stderr, "sshos: le redemarrage n'a pas abouti\n");
  return 1;
}
