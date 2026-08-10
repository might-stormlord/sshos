#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "common/fd.hpp"
#include "daemon/daemonize.hpp"
#include "harness.hpp"

namespace {

// Un pid seul n'est pas unique entre espaces de noms pid distincts qui
// partagent un même /tmp (deux suites lancées dans des conteneurs
// `unshare --pid` séparés peuvent porter le même pid) -- même précédent que
// unique_name() dans tests/test_net.cpp : un aléa frais tiré d'une source
// d'entropie du noyau à chaque appel, plutôt que le pid seul.
std::string unique_marker(const char* suffix) {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "/tmp/sshos-test-" << ::getpid() << '-' << std::hex << dist(rng) << '-' << suffix;
  return os.str();
}

bool wait_for_file(const std::string& path, int tries) {
  for (int i = 0; i < tries; ++i) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) return true;
    ::usleep(20 * 1000);
  }
  return false;
}

// Cherche une ligne "label   valeur" (format /proc/*/status) dans un
// fichier texte multi-lignes et rend la valeur, ou une chaine vide si le
// label est absent -- l'absence se traduit alors par un CHECK_EQ qui
// echoue reellement contre la valeur attendue, plutot que de comparer deux
// variables toutes deux vides par defaut sans avoir rien verifie.
std::string extract_field(const std::string& path, const std::string& label) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tag;
    std::string value;
    ls >> tag >> value;
    if (tag == label) return value;
  }
  return {};
}

// Restaure le masque de signaux du processus tel qu'il était avant le
// blocage, même si le test lève ou revient tôt via REQUIRE : tests/main.cpp
// intercepte les exceptions et saute le reste du corps du test, donc tout
// nettoyage placé en fin de fonction sans passer par un destructeur ne
// s'exécute jamais dans ce cas. SIG_SETMASK avec le masque sauvegardé (au
// lieu d'un SIG_UNBLOCK ciblé sur les seuls signaux ajoutés) restaure l'état
// exact d'avant, sans supposer que rien d'autre n'était déjà bloqué.
struct SigMaskGuard {
  sigset_t saved{};
  explicit SigMaskGuard(const sigset_t& to_block) { ::sigprocmask(SIG_BLOCK, &to_block, &saved); }
  ~SigMaskGuard() { ::sigprocmask(SIG_SETMASK, &saved, nullptr); }
  SigMaskGuard(const SigMaskGuard&) = delete;
  SigMaskGuard& operator=(const SigMaskGuard&) = delete;
};

// Retire un fichier temporaire même sur un retour anticipé par REQUIRE :
// même raison que SigMaskGuard ci-dessus (tests/main.cpp saute le reste du
// corps du test sur exception, donc un ::unlink() placé seulement en fin de
// fonction ne protège pas contre un REQUIRE échoué au milieu).
struct UnlinkGuard {
  std::string path;
  explicit UnlinkGuard(std::string p) : path(std::move(p)) {}
  ~UnlinkGuard() { ::unlink(path.c_str()); }
  UnlinkGuard(const UnlinkGuard&) = delete;
  UnlinkGuard& operator=(const UnlinkGuard&) = delete;
};

// Force temporairement une disposition SIG_IGN sur un signal, pour verifier
// ensuite qu'elle ne fuit pas a travers fork()+fork()+execve() -- meme
// raison d'etre que SigMaskGuard et UnlinkGuard ci-dessus (restauration
// garantie meme sur retour anticipe par REQUIRE).
struct SigIgnGuard {
  int sig;
  struct sigaction saved {};
  explicit SigIgnGuard(int s) : sig(s) {
    struct sigaction act {};
    act.sa_handler = SIG_IGN;
    ::sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    ::sigaction(sig, &act, &saved);
  }
  ~SigIgnGuard() { ::sigaction(sig, &saved, nullptr); }
  SigIgnGuard(const SigIgnGuard&) = delete;
  SigIgnGuard& operator=(const SigIgnGuard&) = delete;
};

// Filet de securite pour un rendez-vous FIFO : garantit que le lecteur
// bloque en open() est libere meme si le test sort tot -- un REQUIRE
// echoue entre le mkfifo et la liberation normale plus loin dans la
// fonction. Sans ca, le petit-enfant reste bloque a vie dans open(),
// invisible : aucun fichier residuel (le mkfifo a deja ete efface par
// l'UnlinkGuard correspondant), et plus aucun moyen de le reveiller.
//
// release() porte la meme logique bornee (O_NONBLOCK + tentatives
// limitees) que l'ancien code du chemin heureux, qu'elle remplace : elle
// s'appelle explicitement au bon moment dans le corps du test, et se
// desarme aussitot apres, qu'elle ait reussi ou non -- un second appel
// (depuis le destructeur, en filet de securite) ne retente donc rien. Ce
// desarmement est ce qui evite d'ajouter du delai au chemin heureux :
// sans lui, le destructeur retenterait aveuglement les tentatives bornees
// alors que le petit-enfant, deja libere, n'est plus lecteur -- 200 x
// 20 ms perdues a chaque execution reussie. Reste bornee meme si le
// petit-enfant n'a jamais demarre (execv en echec, ou spawn_detached() a
// echoue avant meme de forker) : il n'y aura alors jamais de lecteur, et
// les tentatives s'epuisent normalement plutot que de bloquer
// indefiniment.
//
// Ne leve jamais : appelable depuis un destructeur, elle ignore
// silencieusement un echec d'ouverture ou d'ecriture -- il n'y a de toute
// facon personne a qui le rapporter depuis la ou elle peut s'executer.
class FifoReleaseGuard {
 public:
  explicit FifoReleaseGuard(std::string path) : path_(std::move(path)) {}
  ~FifoReleaseGuard() { release(); }

  bool release() {
    if (!armed_) return true;
    armed_ = false;
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; ++i) {
      fd = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK);
      if (fd < 0) ::usleep(20 * 1000);
    }
    if (fd < 0) return false;
    sshos::Fd write_end(fd);
    return ::write(write_end.get(), "g\n", 2) == 2;
  }

  FifoReleaseGuard(const FifoReleaseGuard&) = delete;
  FifoReleaseGuard& operator=(const FifoReleaseGuard&) = delete;

 private:
  std::string path_;
  bool armed_ = true;
};

}  // namespace

// Quatre propriétés en un test : l'intermédiaire meurt tout de suite, le
// petit-enfant vit après sa mort, son parent n'est plus nous, et -- la
// raison d'être du second fork, cf. la correction PÉRIMÉ ci-dessus dans le
// plan -- il n'est pas chef de session : sa session est celle de
// l'intermédiaire (qui a appelé setsid()), pas la nôtre ni la sienne
// propre.
TEST(daemonize_detaches_the_grandchild) {
  const std::string marker = unique_marker("ppid");
  const std::string fifo = unique_marker("go-fifo");
  const std::string hold_fifo = unique_marker("hold-fifo");
  ::unlink(marker.c_str());
  ::unlink(fifo.c_str());
  ::unlink(hold_fifo.c_str());
  UnlinkGuard marker_guard(marker);
  UnlinkGuard fifo_guard(fifo);
  UnlinkGuard hold_fifo_guard(hold_fifo);

  // Filet de securite pour les deux rendez-vous (voir FifoReleaseGuard
  // ci-dessus) : si un REQUIRE echoue avant la liberation normale plus
  // bas, ces destructeurs liberent le petit-enfant a la place. Ordre de
  // declaration significatif, deux fois :
  //  - Apres les UnlinkGuard correspondants, pour que la liberation
  //    (destructeur execute en premier, ordre inverse de declaration) ait
  //    lieu AVANT que le chemin de la FIFO ne soit efface -- sinon
  //    l'open() par chemin du destructeur echouerait ENOENT.
  //  - hold_release avant fifo_release, pour que le destructeur de
  //    fifo_release s'execute EN PREMIER (toujours l'ordre inverse) :
  //    le petit-enfant ne peut atteindre `read y < hold_fifo` qu'apres
  //    avoir franchi `read x < fifo`, donc liberer hold_fifo avant fifo
  //    dans un scenario ou aucun des deux rendez-vous normaux n'a encore
  //    eu lieu ne debloquerait rien : le lecteur n'y est pas encore.
  FifoReleaseGuard hold_release(hold_fifo);
  FifoReleaseGuard fifo_release(fifo);

  // Synchronisation par tube nomme (FIFO) plutot que par sleep + pari
  // d'horloge murale : ce projet a deja retire un tel pari ailleurs pour la
  // meme raison (le seuil chronometre I4 du scan de collage, remplace par
  // un compteur deterministe). Le petit-enfant bloque avant d'ecrire le
  // marqueur ; il ne peut se debloquer que lorsque nous ouvrons la FIFO en
  // ecriture -- l'absence du marqueur au moment ou on la verifie est donc
  // garantie par construction, pas par une estimation de delai.
  //
  // Un tube ANONYME (pipe() + dup2 sur notre propre fd 0 avant l'appel,
  // dans l'espoir que le petit-enfant en herite via fork()+fork()+execve())
  // a ete tente en premier et s'est revele errone : spawn_detached()
  // redirige TOUJOURS fds 0/1/2 vers /dev/null dans le petit-enfant, juste
  // avant l'execv (voir redirect_std_to_devnull() dans daemonize.cpp), donc
  // la lecture inheritee sur fd 0 est fermee et remplacee avant meme que le
  // shell ne demarre -- `read x` du script lit alors /dev/null (EOF
  // immediat) au lieu du tube, et le script continue sans jamais attendre.
  // Verifie empiriquement (premiere version de ce test) : le marqueur
  // existait deja au moment du CHECK cense constater son absence, et pire,
  // le write() suivant sur l'extremite d'ecriture du tube -- desormais sans
  // aucun lecteur, tous les porteurs de l'extremite de lecture l'ayant deja
  // refermee -- declenchait SIGPIPE et tuait le processus de test entier
  // (code de sortie 141), sortie perdue car le stdio du binaire est
  // pleinement bufferise hors tty.
  //
  // Une FIFO du systeme de fichiers echappe a ce probleme : le shell lance
  // par spawn_detached() l'ouvre lui-meme par CHEMIN au moment de son choix
  // (via `< fifo` dans son script), independamment de ce que
  // redirect_std_to_devnull() a fait a ses fds 0/1/2 -- ce n'est pas un
  // descripteur herite, donc rien ne peut le lui retirer avant qu'il ne
  // l'ouvre. L'ouverture en lecture d'une FIFO bloque tant qu'aucun
  // ecrivain ne l'a ouverte a son tour : c'est exactement le rendez-vous
  // voulu, sans deviner de delai.
  REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

  // Deuxieme tube, cree des maintenant pour la meme raison que le premier :
  // le script du petit-enfant doit trouver le chemin deja present quand il
  // l'ouvrira, apres avoir ecrit le marqueur. Il sert a le maintenir vivant
  // le temps d'interroger sa session (cf. plus bas) sans deviner de delai.
  REQUIRE(::mkfifo(hold_fifo.c_str(), 0600) == 0);

  // Le ppid vivant (relu dans /proc/$$/stat, PAS $PPID) et $$ tous deux
  // vers le marqueur : le premier sert aux CHECK de reparentage ci-dessous
  // (c'est le pid de qui a recueilli le petit-enfant apres la mort de
  // l'intermediaire, PAS le pid du petit-enfant lui-meme) ; $$ est le pid
  // du petit-enfant en tant que tel, celui dont la session doit etre
  // interrogee plus bas -- lui ne change pas, seul le choix pour le ppid
  // est en cause ci-dessous.
  //
  // $PPID a ete essaye en premier et s'est revele source d'un flake :
  // reproduit empiriquement a 1 echec sur ~20 executions du seul test
  // daemonize, toujours sur le CHECK de reparentage plus bas. Cause :
  // $PPID est FIGE a l'initialisation du shell, il n'est pas relu a chaque
  // usage. Sonde independante : un /bin/sh dont le parent meurt juste
  // apres son demarrage rapporte un $PPID qui reste egal au pid deja mort,
  // alors que le ppid reel lu dans /proc a deja bascule sur 1. Course
  // precise ici : l'intermediaire fait _exit() un seul syscall apres le
  // second fork() (daemonize.cpp), pendant que le petit-enfant doit encore
  // traverser chdir, trois dup2, la boucle de reset_signal_state(), puis
  // execv, puis l'initialisation de dash -- c'est a ce moment-la que $PPID
  // se fige. Le plus souvent l'intermediaire gagne largement et $PPID vaut
  // deja 1, mais pas toujours : un futur lecteur qui verrait $PPID
  // reapparaitre ici doit savoir que c'est faux, pas juste rare.
  //
  // Corrige sans rien ajouter au dispositif existant : le script bloque
  // sur `read x < fifo` avant d'ecrire le marqueur, et REQUIRE(mid > 0)
  // puis waitpid(mid, ...) plus haut s'executent avant l'ouverture de
  // cette FIFO -- donc au moment precis ou le marqueur s'ecrit,
  // l'intermediaire est deja garanti mort ET recolte. Relire le ppid en
  // direct dans /proc/$$/stat (quatrieme champ) au lieu de $PPID rend la
  // propriete exacte par construction, comme le reste de ce test -- meme
  // raisonnement que celui qui a fait preferer une FIFO a un sleep.
  // `cut -d' ' -f4` suppose que le champ comm (deuxieme champ, entre
  // parentheses) ne contient aucune espace : vrai pour "sh" (comm vaut
  // "(sh)"), donc sur pour ce script precis.
  const pid_t mid = sshos::spawn_detached(
      {"/bin/sh", "-c",
       "read x < " + fifo + "; echo $(cut -d' ' -f4 /proc/$$/stat) $$ > " +
           marker + "; read y < " + hold_fifo});

  // mid <= 0 ne doit jamais atteindre le waitpid ci-dessous : waitpid(-1, ...)
  // n'est pas une erreur, c'est une attente sur N'IMPORTE QUEL enfant, donc
  // un mid invalide ferait bloquer sur, ou recolter en silence, un processus
  // sans rapport.
  REQUIRE(mid > 0);

  int status = 0;
  const pid_t reaped = ::waitpid(mid, &status, 0);
  CHECK_EQ(reaped, mid);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);

  // Le petit-enfant est bloque sur l'ouverture de la FIFO (personne ne l'a
  // encore ouverte en ecriture) : il ne peut pas encore avoir execute la
  // ligne qui ecrit le marqueur, par construction plutot que par pari sur
  // un delai ecoule.
  struct stat st {};
  CHECK_EQ(::stat(marker.c_str(), &st), -1);

  // Signal de depart : ouvrir la FIFO en ecriture debloque l'ouverture en
  // lecture du petit-enfant ; ecrire puis fermer lui livre une ligne suivie
  // d'EOF, que `read` consomme avec succes. FifoReleaseGuard::release()
  // porte la logique bornee (O_NONBLOCK + tentatives limitees, voir sa
  // definition plus haut) qui evite un blocage infini si le petit-enfant
  // n'a jamais pu demarrer (execv en echec, par exemple) sans pour autant
  // parier sur un delai d'execution normal -- seul le cas pathologique est
  // borne, pas le cas nominal. Le meme appel desarme le filet de securite
  // du destructeur : pas de double tentative.
  REQUIRE(fifo_release.release());

  CHECK(wait_for_file(marker, 100));

  std::ifstream in(marker);
  pid_t recorded = 0;
  pid_t grandchild_pid = 0;
  in >> recorded >> grandchild_pid;
  // L'extraction doit reussir avant que la valeur de `recorded` ne
  // signifie quoi que ce soit : sur un fichier absent ou tronque, elle
  // resterait a 0, et `0 != getpid()`/`0 != mid` passeraient sans avoir
  // rien verifie.
  REQUIRE(bool(in));
  // Ni notre propre pid, ni celui de l'intermediaire : le petit-enfant est
  // reparente ailleurs (init en pratique, mais pas necessairement -- un
  // sous-reaper present dans l'arbre de processus peut le recuperer a la
  // place, et le test doit rester correct dans ce cas plutot que d'exiger
  // recorded == 1).
  CHECK(recorded != ::getpid());
  CHECK(recorded != mid);

  // La correction PERIME du plan tient en une phrase : l'intermediaire
  // devient chef de session (setsid()), pas le petit-enfant. Les deux
  // CHECK ci-dessus ne le verifient pas -- n'importe quel processus
  // orphelin herite d'un nouveau parent, chef de session ou non, donc ils
  // passeraient tout autant si le second fork() ou le setsid() disparaissait
  // (verifie par mutation, voir le rapport final). getsid() est la seule des
  // deux proprietes du second fork qui reste absente. Piege deja tombe une
  // fois en ecrivant ce test : interroger getsid(recorded) est FAUX --
  // `recorded` ($PPID) est le pid de qui a recueilli le petit-enfant apres
  // coup (init, typiquement), pas le petit-enfant lui-meme ; ca a fait
  // echouer ce CHECK meme contre l'implementation correcte (obtenu = 1, la
  // session d'init). Il faut la session de `grandchild_pid` ($$), le pid du
  // petit-enfant en tant que tel. Il est encore vivant au moment de
  // l'interroger, bloque sur l'ouverture de hold_fifo (personne ne l'a
  // encore ouverte en ecriture) -- garanti par construction, donc getsid()
  // n'interroge pas un pid deja recycle par le noyau apres une sortie
  // prematuree. Sa session doit etre celle de l'intermediaire (qui a appele
  // setsid(), donc sid == son propre pid == mid), pas la notre ni, si
  // setsid() manquait, la sienne propre (auquel cas sid == grandchild_pid).
  CHECK_EQ(::getsid(grandchild_pid), mid);

  // Libere le petit-enfant : sans ce second signal de depart, il resterait
  // bloque indefiniment sur hold_fifo, reparente a init -- une fuite de
  // processus que le test ne doit pas causer lui-meme. Meme guard qu'au
  // premier rendez-vous (FifoReleaseGuard::release(), O_NONBLOCK +
  // tentatives bornees), et meme desarmement du filet de securite du
  // destructeur.
  REQUIRE(hold_release.release());
}

// Le masque de signaux survit à execve : un SIGCHLD encore bloqué casse
// tout enfant qui attend ses propres processus (make -j8, par exemple).
TEST(daemonize_clears_the_signal_mask_before_exec) {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGCHLD);
  sigaddset(&block, SIGUSR1);
  SigMaskGuard mask_guard(block);

  const std::string marker = unique_marker("sigmask");
  ::unlink(marker.c_str());
  UnlinkGuard marker_guard(marker);
  // /bin/cp directement, sans passer par un shell -- `/bin/sh -c "grep ... >
  // marker"` a ete essaye en premier et s'est revele impropre a ce test
  // precis : verifie empiriquement (execv direct de dash avec SIGCHLD et
  // SIGUSR1 bloques, sans aucun fork ni aucune ligne de daemonize.cpp dans
  // la boucle) que dash reinitialise lui-meme le masque de signaux herite
  // avant d'executer la commande externe qu'on lui donne. Le test passerait
  // alors a tort meme si reset_signal_state() ne faisait plus rien -- c'est
  // dash, pas notre code, qui produirait un SigBlk a zero. Un execv direct
  // de /bin/cp (verifie empiriquement qu'il preserve fidelement un masque
  // herite, contrairement a dash) elimine cet intermediaire.
  const pid_t mid =
      sshos::spawn_detached({"/bin/cp", "/proc/self/status", marker});
  // Meme garde qu'au test precedent : un mid invalide ne doit pas atteindre
  // waitpid(-1, ...).
  REQUIRE(mid > 0);

  int status = 0;
  ::waitpid(mid, &status, 0);
  CHECK(wait_for_file(marker, 100));

  CHECK_EQ(extract_field(marker, "SigBlk:"), std::string("0000000000000000"));
}

// SIG_IGN survit à execve (contrairement à un gestionnaire installé par
// sigaction) : si l'appelant ignore SIGHUP avant de lancer un démon et que
// rien ne réinitialise cette disposition avant l'execv du petit-enfant, le
// programme lancé hérite silencieusement de cette décision -- y compris un
// programme qui ne devrait jamais ignorer SIGHUP. C'est ce que
// reset_signal_state() doit empêcher en général, et ce que la règle
// documentée dans daemonize.cpp (ne pas réappliquer SIG_IGN(SIGHUP) après
// coup pour le seul spawn_detached()) ne doit pas réintroduire par un autre
// chemin.
TEST(daemonize_clears_signal_dispositions_before_exec) {
  SigIgnGuard ignore_hup(SIGHUP);

  const std::string marker = unique_marker("sigign");
  ::unlink(marker.c_str());
  UnlinkGuard marker_guard(marker);
  const pid_t mid = sshos::spawn_detached(
      {"/bin/sh", "-c", "grep SigIgn /proc/self/status > " + marker});
  REQUIRE(mid > 0);

  int status = 0;
  ::waitpid(mid, &status, 0);
  CHECK(wait_for_file(marker, 100));

  std::ifstream in(marker);
  std::string label;
  std::string mask;
  in >> label >> mask;
  CHECK_EQ(label, std::string("SigIgn:"));
  CHECK_EQ(mask, std::string("0000000000000000"));
}

// argv vide : rien à exécuter, donc rien à forker -- même signal d'échec
// que l'échec du premier fork() (voir daemonize.hpp). Sans cette garde,
// raw[0] lirait directement le nullptr terminal (aucun élément avant lui
// dans un argv vide) et le passerait tel quel à execv.
TEST(daemonize_spawn_detached_rejects_empty_argv) {
  CHECK_EQ(sshos::spawn_detached({}), -1);
}
