#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "client/client.hpp"
#include "common/net.hpp"
#include "daemon/daemon.hpp"
#include "daemon/daemonize.hpp"

namespace {

constexpr int kConnectAttempts = 50;
constexpr int kConnectDelayUs = 20 * 1000;

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
  const pid_t mid = sshos::spawn_detached({sshos::daemon_exe_path(), "--daemon"});
  if (mid < 0) {
    std::fprintf(stderr, "sshos: impossible de lancer le demon\n");
    return 1;
  }
  int status = 0;
  ::waitpid(mid, &status, 0);  // l'intermédiaire meurt aussitôt

  for (int i = 0; i < kConnectAttempts; ++i) {
    try {
      sshos::Fd probe = sshos::connect_abstract(name);
      return 0;
    } catch (const std::exception&) {
      ::usleep(kConnectDelayUs);
    }
  }
  std::fprintf(stderr, "sshos: le demon n'a pas repondu\n");
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
  try {
    sshos::Fd probe = sshos::connect_abstract(name);
    probe.reset();
  } catch (const std::exception&) {
    if (logind_kills_user_processes()) {
      std::fprintf(stderr,
                   "sshos: attention, logind est configure avec "
                   "KillUserProcesses=yes ;\n        vos fenetres ne "
                   "survivront pas a la deconnexion.\n        Parade : "
                   "loginctl enable-linger %d\n",
                   static_cast<int>(::getuid()));
    }
    if (start_daemon_and_connect(name) != 0) return 1;
  }

  return sshos::run_client(name);
}
