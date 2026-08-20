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

  // `self_exe` n'existe que pour les tests, comme `boot_id_path` de
  // net.hpp : il a une valeur par defaut pointant sur le vrai chemin, et il
  // permet de rejouer « le binaire qui tourne est-il celui qui est pose ? »
  // sans avoir a remplacer le processus courant.
  UpdateService(const Platform& plat, std::string state_path, Launcher launch,
                std::string self_exe = "/proc/self/exe");

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

  // Un redemarrage reste a faire : le binaire est pose, mais ce n'est pas
  // encore lui qui tourne.
  bool needs_restart() const {
    return state_.status == UpdateStatus::RestartPending;
  }

  // CE QU'ON DOIT DIRE A L'UTILISATEUR, ET UNE SEULE FOIS.
  //
  // Une verification AUTOMATIQUE se tait : elle ne doit pas harceler. Une
  // verification DEMANDEE, si -- l'utilisateur a clique, il attend une
  // reponse, et « rien ne se passe » est la pire des reponses. Le rapport se
  // prend une fois puis disparait.
  bool has_report() const { return !report_.empty(); }
  std::string take_report();

  // DEUX MORCEAUX, ET C'EST L'APPELANT QUI COMPOSE. Une confirmation doit
  // dire ce qu'elle FAIT (« Installer la mise a jour ? ») ; un constat dit
  // ce qui EST (« Mise a jour disponible »). La meme phrase ne peut pas
  // servir aux deux, et l'avoir essaye a fait disparaitre le verbe de la
  // confirmation.
  //
  // « 7 nouveautes », ou vide si le compte est inconnu.
  // Vrai si le processus en cours EST le binaire installe.
  //
  // C'est ce qui distingue « il faut redemarrer » de « on a redemarre ». Le
  // demon ne connait pas son propre commit -- rien ne le grave dedans,
  // CMakeLists etant intouchable -- mais il peut comparer son inode a celle
  // du fichier pose. Un demon reste sur l'ancienne version tient une inode
  // differente : deliee, ou devenue sshos.previous.
  bool running_is_installed() const;

  // « Mise a jour en cours : compilation... », ou l'etape est celle que le
  // script vient d'ecrire. Vide si rien ne court.
  std::string progress_line() const;

  // OU EN EST LE TRAVAIL, EN POUR CENT, ou -1 quand on ne sait pas -- soit
  // que rien ne travaille, soit que le script qui travaille soit trop
  // ancien pour le dire. Meme garde que progress_line() : un travail
  // annonce « en cours » dont le pid est mort n'est pas un travail en
  // cours, et sa barre mentirait.
  int progress_percent() const;

  std::string news() const;
  // « Version 1.12 -> 1.13 », ou « cce9d11 -> 3512ffe » a defaut de numeros,
  // ou vide. On n'invente jamais un numero.
  std::string version_line() const;

 private:
  // « Un travail court-il ? » -- la seule definition, partagee par
  // progress_line() et progress_percent(). Un enfant vivant compte ; un etat
  // qui dit « en cours » avec un pid mort ne compte pas.
  bool working() const;

  void reload();
  void launch(std::string_view mode, bool manual);
  void build_report();
  void schedule_from(std::int64_t checked_at);
  std::string updater_path() const;

  const Platform* plat_;
  std::string state_path_;
  Launcher launch_;
  std::string self_exe_;

  UpdateState state_;
  bool loaded_ = false;
  // Vrai quand l'état lu dit « en cours » mais que le pid n'existe plus :
  // le démon a redémarré pendant le travail, ou le script a été tué.
  bool stale_worker_ = false;
  std::string message_;

  pid_t child_ = -1;
  // Vrai quand l'enfant courant a ete lance par un clic, pas par l'echeance.
  bool child_is_manual_ = false;
  std::string report_;
  bool have_deadline_ = false;
  std::chrono::steady_clock::time_point deadline_{};
  bool wants_restart_ = false;
  // Vrai une fois qu'on a constate que le redemarrage avait eu lieu.
  bool restart_done_ = false;
};

}  // namespace sshos
