#pragma once

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/platform.hpp"
#include "shell/update_state.hpp"

namespace sshos {

// Ce que le service donne au menu. Le menu ne connaît ni l'origine ni le
// sens de ces trois champs : il affiche un libellé, grise ce qui est inerte,
// et rend l'identifiant tel quel.
struct UpdateEntry {
  std::string label;
  bool enabled = true;
  std::string id;
};

// Le service de mise à jour, côté session. Sans interface, sans réseau, sans
// `git` : il lit un fichier d'état que des scripts écrivent, et il demande
// qu'on lance un enfant.
//
// TROIS COUTURES D'INJECTION, chacune avec un précédent dans le projet :
//   - le chemin du fichier d'état, comme read_boot_id(boot_id_path) ;
//   - le lanceur d'enfant, pour qu'aucun test ne forke ;
//   - un const Platform&, comme Session, pour l'horloge.
//
// IL NE FORKE PAS LUI-MÊME, et il ne récolte jamais : `waitpid` est global
// au processus, et deux composants qui appelleraient waitpid(-1) chacun de
// leur côté se voleraient mutuellement leurs enfants (voir app.hpp:49). La
// session lui fournit un lanceur qui inscrit le pid auprès du récolteur
// unique du démon, et l'appelle à la mort de l'enfant.
class UpdateService {
 public:
  // Rend le pid de l'enfant lancé, ou -1. L'argv est complet, argv[0]
  // compris.
  using Launcher = std::function<pid_t(const std::vector<std::string>& argv)>;

  UpdateService(const Platform& plat, std::string state_path, Launcher launch);

  // Relit le fichier d'état, et lance une vérification si l'échéance est
  // échue. Appelée par la session, au plus une fois par seconde.
  void tick();

  // Millisecondes avant la prochaine échéance, ou -1 s'il n'y a rien à
  // attendre. Le démon la replie dans le délai de son epoll_wait.
  int delay_ms() const;

  UpdateEntry entry() const;
  bool badge() const;
  std::string message() const;

  // Exécute une commande du menu : update:check, update:apply,
  // update:restart. Sans effet si l'entrée courante est inerte -- la garde
  // ne repose pas sur l'interface.
  void run(std::string_view id);

  // Vrai si cet enfant est le nôtre. La session le demande AVANT de chercher
  // le pid dans sa table de fenêtres, qui ignore silencieusement un pid
  // inconnu.
  bool owns(pid_t pid) const { return child_ > 0 && pid == child_; }
  void on_child_exit(pid_t pid, int status);

  // L'utilisateur a demandé le redémarrage. La session le lit, ferme le
  // bureau et laisse le client se rattacher.
  bool wants_restart() const { return wants_restart_; }

 private:
  void reload();
  void schedule_from(std::int64_t checked_at);
  std::string updater_path() const;

  const Platform* plat_;
  std::string state_path_;
  Launcher launch_;

  UpdateState state_;
  bool loaded_ = false;
  // Vrai quand l'état lu dit « en cours » mais que le pid n'existe plus :
  // le démon a redémarré pendant le travail, ou le script a été tué.
  bool stale_worker_ = false;
  std::string message_;

  pid_t child_ = -1;
  bool have_deadline_ = false;
  std::chrono::steady_clock::time_point deadline_{};
  bool wants_restart_ = false;
};

}  // namespace sshos
