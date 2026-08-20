#include "client/launch.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include "common/net.hpp"
#include "daemon/daemonize.hpp"

namespace sshos {

DaemonLaunch launch_daemon(const std::string& socket_name,
                           const std::string& exe_path,
                           const std::function<void()>& on_slow,
                           LaunchBudget budget, DaemonSpawner spawn) {
  const pid_t mid = spawn ? spawn({exe_path, "--daemon"})
                          : spawn_detached({exe_path, "--daemon"});
  if (mid < 0) return DaemonLaunch::SpawnFailed;

  // L'intermédiaire meurt aussitôt ; sans cette récolte il resterait
  // zombie (daemonize.hpp).
  //
  // EINTR EST UN ÉCHEC QUI N'EN EST PAS UN : `waitpid` rend -1 sans avoir
  // récolté, et abandonner là laisse précisément le zombie qu'on venait
  // éviter. Un signal quelconque suffit à le déclencher, et le client tourne
  // sur un terminal -- `SIGWINCH` arrive tout seul quand on redimensionne la
  // fenêtre pendant que le démon se lève.
  int status = 0;
  while (::waitpid(mid, &status, 0) < 0 && errno == EINTR) {
  }

  const auto debut = std::chrono::steady_clock::now();
  bool prevenu = false;
  for (;;) {
    try {
      Fd probe = connect_abstract(socket_name);
      return DaemonLaunch::Connected;
    } catch (const std::exception&) {
    }
    const auto ecoule = std::chrono::steady_clock::now() - debut;
    if (ecoule >= budget.total) return DaemonLaunch::TimedOut;
    if (!prevenu && ecoule >= budget.patience) {
      prevenu = true;
      if (on_slow) on_slow();
    }
    ::usleep(static_cast<useconds_t>(budget.interval.count()) * 1000);
  }
}

}  // namespace sshos
