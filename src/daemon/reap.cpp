#include "daemon/reap.hpp"

#include <sys/wait.h>

namespace sshos {

int reap_children(ChildSink& sink) {
  int reaped = 0;
  for (;;) {
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, WNOHANG);
    // 0 : il reste des enfants, mais aucun n'est mort. -1 : plus d'enfant
    // du tout (ECHILD), ou un appel interrompu -- dans les deux cas il n'y
    // a plus rien à récolter maintenant.
    if (pid <= 0) break;
    ++reaped;
    sink.on_child_exit(pid, status);
  }
  return reaped;
}

}  // namespace sshos
