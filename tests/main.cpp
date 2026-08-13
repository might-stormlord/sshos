#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "harness.hpp"

// UBSan imprime un diagnostic sur undefined behavior puis, par défaut,
// *continue* — le processus peut sortir en 0 après un signed overflow ou un
// shift hors bornes, comme si de rien n'était. `halt_on_error`/
// `abort_on_error` corrigent ça, mais ce sont des options lues depuis
// UBSAN_OPTIONS ou depuis ce symbole faible ; un export oublié dans
// l'environnement (ou un lanceur de test qui ne le propage pas) désarme
// silencieusement le gardien. Définir __ubsan_default_options() ici fixe le
// réglage dans le binaire lui-même, quel que soit l'invocateur.
//
// GCC ne définit aucune macro de détection équivalente à
// __has_feature(undefined_behavior_sanitizer) de Clang, donc ce symbole est
// défini inconditionnellement plutôt que sous #ifdef. Inoffensif en
// Release : aucun runtime UBSan n'y est lié (CMakeLists.txt n'ajoute
// -fsanitize=undefined qu'en Debug), le symbole reste alors exporté mais
// jamais appelé.
extern "C" const char* __ubsan_default_options() {
  return "halt_on_error=1:abort_on_error=1";
}

// --- Garde-temps par cas ----------------------------------------------
//
// Sans ça, un cas qui bloque (attente sur un mauvais fd, waitpid() sur un
// enfant qui ne sortira jamais...) fait pendre le binaire entier : la boucle
// n'atteint jamais le cas suivant ni la ligne de résumé, et rien ne le
// récupère à part un `timeout` externe. Or le SIGTERM que ce `timeout`
// envoie ne déroule aucun destructeur RAII -- le process bloqué et les
// ressources qu'il tenait (fd, fichier temporaire...) survivent tous les
// deux à sa propre mort. Un blocage devient donc une suite qui ne rend
// jamais la main ET une fuite de ressources. C'est ce que ce round corrige.
//
// Conception retenue : SUPERVISEUR / OUVRIER A LONGUE DUREE DE VIE.
//
// Un premier jet (conservé dans l'historique git de cette tâche) forkait un
// processus PAR CAS. Il fonctionnait -- 166 cas / 0 échec, 0 avertissement,
// aucune sortie dupliquée -- mais mesuré comme l'exige cette tâche, il a
// fait passer la suite Debug (ASan+UBSan) d'environ 2.9 s à 10-12 s. Un
// micro-banc isolé (mêmes options de compilation que ce binaire) a montré
// que fork() lui-même coûte ~8.5 ms sous ASan/Debug dans cet environnement
// (jusqu'à ~22.5 ms avec exit() plutôt que _exit(), à cause du scan de
// fuites qu'exit() déclenche via atexit) -- un coût par fork, pas par ligne
// de code ajoutée ici (setpgid/waitpid mesurent ~0.03 s cumulés sur les 166
// cas). Le seul levier restant pour respecter "le chemin nominal ne doit
// rien coûter de perceptible" est de ne PAS forker à chaque cas.
//
// Ici, le superviseur (ce processus) forke un unique OUVRIER qui exécute
// TOUS les cas restants en séquence, exactement comme le faisait l'ancienne
// boucle simple -- un seul appel à std::exit() pour tout le lot, donc le
// scan LeakSanitizer ne coûte plus qu'une fois pour toute la suite, comme
// avant ce round. Le superviseur ne fork() une seconde fois QUE si l'ouvrier
// dépasse son délai ou meurt anormalement (crash, signal) : sur les 168 cas
// qui passent aujourd'hui, un seul fork() a lieu pour toute l'exécution.
//
// Superviseur et ouvrier se coordonnent via DEUX sémaphores POSIX anonymes
// (sem_t, pshared=1) placés dans une région mmap(MAP_SHARED|MAP_ANONYMOUS)
// (pas de tube : aucun descripteur qui pourrait fuiter à travers un
// execve() plus bas dans un cas comme spawn_detached()) :
//   - `case_done` : posté par l'ouvrier juste après avoir fini un cas, avec
//     son nom et son delta d'échecs déjà écrits dans la mémoire partagée.
//     Le superviseur l'attend avec sem_timedwait() et une échéance absolue
//     -- c'est ce qui donne le délai par cas, sans SIGALRM ni minuteur :
//     sem_timedwait() encapsule déjà l'attente-avec-échéance de façon
//     atomique et sans la fenêtre de course qu'un gestionnaire de signal
//     externe imposerait entre "armer" et "attendre".
//   - `case_ack` : posté par le superviseur une fois qu'il a fini de lire
//     les champs ci-dessus ; l'ouvrier l'attend avant de passer au cas
//     suivant. Ce second sémaphore rend le protocole STRICTEMENT
//     séquentiel (l'ouvrier ne peut jamais avoir plus d'un cas d'avance) --
//     sans lui, l'ouvrier pourrait terminer deux cas avant que le
//     superviseur n'ait consommé le premier et écraserait le nom/delta du
//     premier avec ceux du second, un vrai bogue trouvé pendant la
//     conception de ce round et corrigé avant qu'aucun code ne parte en
//     revue. Le coût de cet aller-retour (quelques appels futex) est sans
//     commune mesure avec un fork() -- voir la mesure dans le rapport de
//     cette tâche.
//
// Pourquoi un sémaphore plutôt qu'un signal (SIGUSR1...) pour la moitié
// ouvrier->superviseur : les signaux Unix standard (non temps réel) peuvent
// fusionner deux occurrences rapprochées ; même avec le verrouillage
// séquentiel ci-dessus qui limite le risque, sem_timedwait() reste le seul
// des deux à offrir une attente-avec-échéance atomique et sans réglage de
// gestionnaire process-wide (SIGALRM aurait fallu l'installer/désinstaller
// autour de chaque fork() de reprise, avec sa propre fenêtre de course).
//
// Pourquoi un processus plutôt qu'un fil ou un longjmp depuis un
// gestionnaire de signal : la détection de dépassement de délai
// (sem_timedwait) n'interrompt jamais que le SUPERVISEUR, qui n'exécute
// aucun code de cas -- aucune pile C++ de test n'est jamais déroulée par un
// mécanisme de recouvrement. Un SIGALRM armé dans le processus qui exécute
// le cas lui-même reproduirait exactement le défaut du SIGTERM externe
// décrit plus haut (destructeurs sautés). Un fil + pthread_cancel() différé
// aurait le même problème pour un cas bloqué en calcul pur (aucun point
// d'annulation à atteindre), en plus de partager l'espace mémoire du
// binaire de test : un cas qui corromprait la mémoire en mourant
// menacerait tous les cas suivants. Un processus séparé isole ça par
// construction.
//
// Ce que cette conception NE récupère PAS (voir le rapport de cette tâche
// pour la discussion complète) :
//   - les destructeurs RAII du cas tué (fichiers temporaires non
//     supprimés, sockets non fermées côté disque/abstract namespace) --
//     SIGKILL ne déroule rien, par construction, exactement comme le
//     SIGTERM externe qu'il remplace ; la différence est que le PROCESSUS
//     BINAIRE DE TEST, lui, ne bloque plus et continue les cas suivants ;
//   - un démon détaché via spawn_detached() : son setsid() le sort de son
//     groupe de processus dès qu'il réussit, par construction (voir
//     daemonize.cpp) -- le SIGKILL de groupe ne l'atteint donc pas, ce qui
//     est le comportement attendu d'un démon correctement détaché, pas une
//     lacune de ce garde-temps ;
//   - un processus coincé en E/S noyau non interruptible (état D) :
//     SIGKILL ne le réveille pas non plus, limitation POSIX universelle. Le
//     garde-temps abandonne après une seconde de récolte infructueuse
//     plutôt que de reproduire le blocage qu'il corrige ;
//   - un ouvrier qui plante (crash, signal, abort ASan) est détecté au plus
//     tard après kDeathPollMs (50 ms), pas après `timeout_ms` -- voir la
//     section "Suivi" ci-dessous ; la marge encore non recouverte est cette
//     fenêtre de sondage elle-même, pas la totalité du délai.
//
// --- Suivi : détection lente d'un ouvrier mort (corrigé) --------------
//
// Le premier jet de ce garde-temps utilisait un unique sem_timedwait()
// jusqu'à l'échéance complète du cas : un ouvrier qui plantait (SIGSEGV,
// abort ASan...) au lieu de bloquer n'était donc constaté qu'à l'échéance,
// pas immédiatement -- prédit dans la première version de cette note
// ("pas de chemin rapide type SIGCHLD"), puis mesuré en revue : une suite
// contenant un cas plantant sous `::raise(SIGSEGV)`, garde au défaut
// (30 s), prenait 38 s de bout en bout au lieu de la poignée de secondes
// de la suite normale -- inutilisable pendant un développement où les
// crashs sont justement fréquents.
//
// Corrigé en tranchant l'attente : wait_for_case() ne fait plus un seul
// sem_timedwait(case_deadline), mais une boucle de sem_timedwait() bornés
// à kDeathPollMs (50 ms) chacun, entrecoupés d'un waitpid(worker,
// WNOHANG) -- suggestion reçue en revue, retenue plutôt qu'un gestionnaire
// SIGCHLD : ce dernier réintroduirait exactement la classe de problème que
// ce fichier évite déjà pour SIGALRM (une fenêtre de course entre "armer"
// et "attendre", plus un état process-wide à restaurer autour de chaque
// fork() de reprise). Le sondage, lui, ne touche à aucun gestionnaire
// global et reste local à wait_for_case().
//
// L'échéance globale du cas (case_deadline, calculée une seule fois par
// compute_absolute_deadline avant la boucle) n'est JAMAIS recalculée par
// ce tranchage -- next_poll_slice() ne renvoie jamais une échéance plus
// tardive que case_deadline, donc un vrai blocage (ouvrier toujours
// vivant) est encore détecté exactement à timeout_ms, ni plus tôt ni plus
// tard qu'avant ce correctif ; seul un ouvrier déjà MORT est désormais vu
// plus tôt.
//
// Coût nominal : toujours nul en pratique. Voir le commentaire de
// wait_for_case() pour le détail, mais en résumé -- un cas qui rend la
// main avant la fin de sa propre première tranche (le cas de toute la
// suite aujourd'hui) ne fait qu'UN SEUL sem_timedwait(), jamais de
// waitpid(WNOHANG) superflu : la boucle ne boucle que si la tranche a
// expiré. Mesuré en alternance stricte (voir le rapport de ce round) pour
// éviter de comparer contre un chiffre mémorisé sous une charge machine
// différente.
//
// --- Suivi : un crash n'est pas une exception C++ (corrigé) -----------
//
// Un ouvrier mort (branche kWorkerDead ci-dessous) était rapporté via
// th::fail_uncaught(), dont le message affirme "exception non
// interceptee" -- faux ici : rien n'a été lancé ni capturé dans une pile
// C++, le PROCESSUS entier a disparu. Repéré en revue sur un cas concret :
// un ::raise(SIGSEGV) sous ASan/Debug se termine en pratique par un
// _exit(1) déclenché par le sanitizer après son propre rapport, pas par le
// signal lui-même (WIFSIGNALED false, code de sortie 1) -- describe_exit()
// rapportait donc, à raison, "code de sortie 1", mais fail_uncaught()
// l'encadrait quand même d'un "exception non interceptee" mensonger,
// imputant la panne à la mauvaise cause. Corrigé en ajoutant
// th::fail_worker_died() (harness.hpp), une étiquette neutre qui ne
// prétend ni exception ni cause précise -- describe_exit() continue de
// distinguer "signal X (N)" (mort réellement par signal, cas d'un build
// Release sans sanitizer pour intercepter) de "code de sortie N" (sortie
// explicite, sanitizer ou non) sans que l'appelant ait besoin de deviner
// laquelle des deux s'est produite.
namespace {

// Lit SSHOS_TEST_TIMEOUT_MS. 0 (ou negatif) desactive explicitement le
// garde-temps -- utile pour deboguer un blocage sous gdb sans que le
// harnais l'interrompe entre-temps (et sans qu'il fasse tourner les cas
// dans un processus different de celui que gdb a attache). Absente/
// invalide : valeur par defaut, tres au-dessus des attentes bornees les
// plus longues connues cote daemonize (jusqu'a 8 s sur un chemin d'echec),
// pour qu'un reglage trop serre ne fasse pas plus de mal qu'aucun
// garde-temps.
long resolve_timeout_ms() {
  constexpr long kDefaultMs = 30000;
  const char* raw = std::getenv("SSHOS_TEST_TIMEOUT_MS");
  if (raw == nullptr || *raw == '\0') return kDefaultMs;

  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(raw, &end, 10);
  if (errno != 0 || end == raw || *end != '\0') {
    std::fprintf(stderr,
                 "sshos_tests: SSHOS_TEST_TIMEOUT_MS='%s' invalide, valeur "
                 "par defaut (%ld ms) conservee\n",
                 raw, kDefaultMs);
    return kDefaultMs;
  }
  return v;
}

// Etat partage entre superviseur et ouvrier -- voir la note de conception
// en tete de fichier pour le protocole exact des deux semaphores.
struct SharedState {
  sem_t case_done;      // poste par l'ouvrier : ce cas est termine
  sem_t case_ack;        // poste par le superviseur : champs consommes
  char case_name[128];   // nom du cas en cours, ecrit par l'ouvrier AVANT
                          // de l'executer (donc toujours fiable, meme si ce
                          // cas precis ne finit jamais -- voir plus bas)
  int case_new_failures;  // delta d'assertions echouees pour CE seul cas
};

SharedState* make_shared_state() {
  void* p = ::mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    std::fprintf(stderr,
                 "sshos_tests: mmap a echoue (%s) ; garde-temps par cas "
                 "desactive pour cette execution\n",
                 std::strerror(errno));
    return nullptr;
  }
  return static_cast<SharedState*>(p);
}

// Corps original, partage par le chemin sans garde (SSHOS_TEST_TIMEOUT_MS=0
// ou repli sur echec de fork()/mmap) et par l'ouvrier du chemin garde.
//
// Un test qui lève au lieu d'échouer proprement ne doit pas emporter le
// reste de la suite : une exception non interceptee saute par-dessus tout
// ce qui suit dans la boucle qui l'appelle (registre entier ou lot restant
// de l'ouvrier).
void run_case_inline(const th::Case& c) {
  try {
    c.fn();
  } catch (const std::exception& e) {
    th::fail_uncaught(c.name, e.what());
  } catch (...) {
    th::fail_uncaught(c.name, "type inconnu (n'herite pas de std::exception)");
  }
}

// Decrit un statut waitpid() en distinguant explicitement mort par SIGNAL et
// sortie par CODE -- les deux causes existent reellement et ne doivent pas
// etre confondues : un SIGSEGV non intercepte tue par signal (WIFSIGNALED),
// mais sous ASan/UBSan le sanitizer intercepte souvent le signal lui-meme,
// imprime son rapport, puis termine par un _exit(code) explicite -- ce que
// waitpid() rapporte alors comme une sortie par CODE, pas par signal, meme
// si la cause racine etait bien un crash. Ce n'est pas ce site d'appel qui
// peut lever cette ambiguite (le rapport du sanitizer, lui, est deja sur
// stderr juste au-dessus) ; le but ici est seulement de ne jamais AFFIRMER
// la mauvaise des deux causes -- voir fail_worker_died() dans harness.hpp
// pour la raison de ne pas non plus l'appeler une exception C++.
std::string describe_exit(int status) {
  if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    const char* name = ::strsignal(sig);
    return "signal " + std::string(name != nullptr ? name : "?") + " (" +
           std::to_string(sig) + ")";
  }
  return "code de sortie " + std::to_string(WEXITSTATUS(status));
}

// Echeance absolue (horloge CLOCK_REALTIME, celle qu'utilise sem_timedwait)
// pour l'attente du PROCHAIN cas. Calculee une seule fois au moment ou le
// superviseur commence a attendre un cas donne, jamais recalculee sur un
// retry EINTR pour ce meme cas -- sinon une suite de signaux etrangers
// pourrait repousser indefiniment l'echeance et annuler le garde-temps.
struct timespec compute_absolute_deadline(long timeout_ms) {
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec += 1;
  }
  return ts;
}

bool same_instant(const struct timespec& a, const struct timespec& b) {
  return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

// Echeance de la PROCHAINE tranche de sondage : au plus tot entre
// `case_deadline` (fixe, calculee une seule fois par compute_absolute_
// deadline pour tout le cas) et maintenant + poll_ms. Ne modifie jamais
// `case_deadline` -- c'est ce qui garantit que boucler en tranches ne change
// PAS le delai total avant un vrai timeout par rapport a un unique
// sem_timedwait(case_deadline) : la derniere tranche, quand la marge
// restante est inferieure a poll_ms, est exactement case_deadline lui-meme.
struct timespec next_poll_slice(const struct timespec& case_deadline,
                                 long poll_ms) {
  struct timespec slice {};
  ::clock_gettime(CLOCK_REALTIME, &slice);
  slice.tv_sec += poll_ms / 1000;
  slice.tv_nsec += (poll_ms % 1000) * 1000000L;
  if (slice.tv_nsec >= 1000000000L) {
    slice.tv_nsec -= 1000000000L;
    slice.tv_sec += 1;
  }
  if (slice.tv_sec > case_deadline.tv_sec ||
      (slice.tv_sec == case_deadline.tv_sec &&
       slice.tv_nsec > case_deadline.tv_nsec)) {
    return case_deadline;
  }
  return slice;
}

// Granularite de sondage entre deux verifications waitpid(WNOHANG) pendant
// l'attente d'un cas -- voir wait_for_case() pour le detail du cout. 50 ms
// est largement "une fraction de seconde" pour la detection d'un ouvrier
// mort, tout en restant assez large pour qu'un cas legitime plus lent qu'une
// tranche (les attentes bornees de daemonize, jusqu'a plusieurs secondes) ne
// paie qu'une poignee de sondages par seconde plutot qu'un flot continu.
constexpr long kDeathPollMs = 50;

// Issue de l'attente d'un seul cas cote superviseur -- voir wait_for_case().
enum class CaseWait {
  kFinished,      // case_done poste : le cas a rendu la main a temps
  kWorkerDead,    // ouvrier trouve mort par un sondage, avant l'echeance
  kWaitpidError,  // waitpid() a echoue de facon inattendue pendant un sondage
  kTimedOut,      // echeance globale du cas atteinte, ouvrier toujours vivant
};

// Attend le prochain case_done jusqu'a l'echeance absolue `case_deadline`,
// mais en sondant l'etat de l'ouvrier par tranches de `poll_ms` plutot qu'en
// un seul sem_timedwait(case_deadline) direct : un ouvrier mort (crash,
// signal, abort sanitizer) est ainsi constate a la PROCHAINE TRANCHE plutot
// qu'a l'echeance complete du cas (jusqu'a 30 s par defaut) -- c'etait le
// trou signale en suivi de ce round : un crash sous ASan en cours de
// developpement faisait payer l'integralite de timeout_ms a chaque
// execution avant de rapporter quoi que ce soit.
//
// Cout nominal : NUL tant qu'un cas rend la main avant la fin de sa propre
// premiere tranche -- ce qui couvre tous les cas de la suite aujourd'hui
// (micro- a milli-secondes chacun). sem_timedwait() est reveille par le
// sem_post() de l'ouvrier bien avant sa propre echeance de tranche, la
// boucle ci-dessous ne boucle alors jamais et ne fait qu'UN SEUL appel
// systeme, exactement comme avant ce round. Le waitpid(WNOHANG)
// supplementaire n'est paye QUE par un cas qui dure deja plus longtemps
// qu'une tranche (poll_ms) -- negligeable face a sa propre duree (les
// attentes bornees de daemonize, jusqu'a plusieurs secondes, paient au plus
// quelques dizaines de sondages a quelques microsecondes chacun).
CaseWait wait_for_case(SharedState* shared, pid_t worker,
                        const struct timespec& case_deadline, long poll_ms,
                        int* death_status, int* waitpid_errno) {
  for (;;) {
    const struct timespec slice = next_poll_slice(case_deadline, poll_ms);
    const int rc = ::sem_timedwait(&shared->case_done, &slice);
    if (rc == 0) return CaseWait::kFinished;
    if (errno == EINTR) continue;  // signal etranger : reboucle, meme echeance globale
    if (errno != ETIMEDOUT) {
      // Erreur inattendue (EINVAL...) : aucun cas connu aujourd'hui, mais
      // aussi grave qu'un vrai timeout -- ne pas boucler indefiniment
      // dessus.
      return CaseWait::kTimedOut;
    }

    // Cette tranche a expire : l'ouvrier est-il deja mort ? C'est le chemin
    // rapide qui remplace l'attente complete de timeout_ms sur un crash.
    int wstatus = 0;
    const pid_t reaped = ::waitpid(worker, &wstatus, WNOHANG);
    if (reaped > 0) {
      *death_status = wstatus;
      return CaseWait::kWorkerDead;
    }
    if (reaped < 0) {
      *waitpid_errno = errno;
      return CaseWait::kWaitpidError;
    }
    // reaped == 0 : toujours vivant. Si cette tranche etait deja
    // l'echeance globale du cas (voir next_poll_slice), c'est un vrai
    // depassement de delai ; sinon ce n'etait qu'un sondage intermediaire,
    // reboucle.
    if (same_instant(slice, case_deadline)) return CaseWait::kTimedOut;
  }
}

// Tourne dans l'OUVRIER : execute sequentiellement cases[start..end) et
// coordonne chaque achevement avec le superviseur via le protocole a deux
// semaphores decrit en tete de fichier. Ne revient jamais avant la fin du
// lot (ou avant d'etre tue) -- c'est ce qui ramene le nombre de fork() du
// chemin nominal a UN SEUL pour toute la suite au lieu d'un par cas.
void run_worker(const std::vector<const th::Case*>& cases, std::size_t start,
                 SharedState* shared) {
  for (std::size_t i = start; i < cases.size(); ++i) {
    const th::Case& c = *cases[i];
    std::snprintf(shared->case_name, sizeof shared->case_name, "%s", c.name);
    std::printf("- %s\n", c.name);
    // Vide immediatement : si CE cas bloque, cette ligne doit malgre tout
    // apparaitre dans la sortie. Sans ce fflush, elle resterait dans le
    // tampon stdio de l'ouvrier (stdout non-tty => tamponne en bloc, cas
    // typique d'une sortie redirigee/pipee en CI) et disparaitrait
    // purement et simplement quand SIGKILL emporte l'ouvrier plus tard.
    std::fflush(stdout);

    const int before = th::failures();
    run_case_inline(c);
    shared->case_new_failures = th::failures() - before;

    // Poste APRES avoir ecrit les deux champs ci-dessus : c'est le signal
    // qui dit au superviseur qu'ils sont valides a lire.
    ::sem_post(&shared->case_done);
    // Attend que le superviseur ait fini de les lire avant de reutiliser
    // ces memes champs pour le cas suivant -- protocole strictement
    // sequentiel, voir la note de conception en tete de fichier.
    ::sem_wait(&shared->case_ack);
  }
}

// Boucle occupee volontaire, sans point d'annulation ni appel systeme
// bloquant : le pire cas pour un mecanisme fonde sur les points
// d'annulation (ex. pthread_cancel differe), et la preuve que ce
// garde-temps n'en a pas besoin puisqu'il agit depuis un AUTRE processus.
// N'est jamais executee par la suite normale -- seulement si
// SSHOS_TEST_SELFTEST_HANG est positionnee. Voir le rapport de cette tache
// pour la commande de preuve complete.
void selftest_hang() {
  for (;;) {
    // Barriere memoire vide : empeche le compilateur de prouver l'absence
    // d'effets observables et de supprimer la boucle a l'optimisation.
    // Preferee a un compteur `volatile` incremente, dont le C++20
    // deprecie l'operateur ++ (-Werror=volatile).
    asm volatile("" ::: "memory");
  }
}

// Construit la liste des cas a executer : soit le cas synthetique de
// demonstration (SSHOS_TEST_SELFTEST_HANG positionnee -- n'ajoute rien a
// th::registry(), le compte de la suite normale reste donc a 166), soit le
// registre normal filtre par `filter` comme avant ce round.
std::vector<const th::Case*> build_cases(const char* filter) {
  std::vector<const th::Case*> cases;
  if (std::getenv("SSHOS_TEST_SELFTEST_HANG") != nullptr) {
    static const th::Case selftest{"selftest_timeout_guard_hangs_on_purpose",
                                    &selftest_hang};
    if (filter == nullptr || std::strstr(selftest.name, filter) != nullptr) {
      cases.push_back(&selftest);
    }
    return cases;
  }
  for (const auto& c : th::registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    cases.push_back(&c);
  }
  return cases;
}

// Execute un cas dans le processus courant (pas de garde-temps) et met a
// jour les compteurs de la boucle appelante. Utilise a la fois quand le
// garde-temps est desactive et comme repli ponctuel si fork() echoue.
void run_case_and_track(const th::Case& c, int& ran, int& failed_cases) {
  std::printf("- %s\n", c.name);
  std::fflush(stdout);
  const int before = th::failures();
  run_case_inline(c);
  ++ran;
  if (th::failures() > before) ++failed_cases;
}

}  // namespace

#include <memory>

#include "daemon/session.hpp"
#include "fake_apps.hpp"
#include "shell/sysinfo.hpp"

// UNE SEULE FOIS, pour toute la suite : la fenetre amorcee est le double
// factice, pas un Terminal. Un Terminal lancerait un vrai shell dans
// chaque cas de session -- lent, salissant -- et surtout, la moitie de ces
// cas lisent a l'ecran l'etat de ce double (son compteur de clics, la
// taille qu'on lui annonce), ce qu'aucune vraie application n'imite.
namespace {
std::unique_ptr<sshos::App> make_seed_double() {
  return std::make_unique<sshos::Bloc>();
}
struct SeedOnce {
  SeedOnce() {
    sshos::Session::set_seed_factory_for_tests(&make_seed_double);
    // Le moniteur de fond lit la vraie machine : gele, il ne rend aucune
    // reference de rendu instable. Son dessin est couvert par
    // test_sysinfo.cpp, qui lui donne des chiffres choisis.
    sshos::SysInfo::freeze_for_tests();
  }
} g_seed_once;
}  // namespace

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  const std::vector<const th::Case*> cases = build_cases(filter);

  const long timeout_ms = resolve_timeout_ms();
  SharedState* shared = timeout_ms > 0 ? make_shared_state() : nullptr;
  const bool guard_enabled = timeout_ms > 0 && shared != nullptr;

  int ran = 0;
  int failed_cases = 0;

  if (!guard_enabled) {
    // SSHOS_TEST_TIMEOUT_MS=0, ou repli sur echec de mmap : aucun fork,
    // execution directe dans ce processus -- comportement d'origine,
    // pratique pour deboguer sous gdb sans second processus dans le jeu.
    for (const auto* c : cases) run_case_and_track(*c, ran, failed_cases);
  } else {
    std::size_t next = 0;
    bool first_worker = true;
    while (next < cases.size()) {
      // Aucun processus n'est bloque sur ces semaphores a cet instant :
      // soit c'est la toute premiere iteration, soit l'ouvrier precedent
      // est deja mort et recolte (voir plus bas) -- (re)initialiser est
      // donc sans risque. sem_destroy() sur un semaphore jamais initialise
      // est UB, d'ou le garde sur `first_worker`.
      if (!first_worker) {
        ::sem_destroy(&shared->case_done);
        ::sem_destroy(&shared->case_ack);
      }
      ::sem_init(&shared->case_done, 1, 0);
      ::sem_init(&shared->case_ack, 1, 0);
      first_worker = false;

      // Toutes les sorties non encore ecrites doivent l'etre AVANT le
      // fork : sinon l'ouvrier en herite une copie et la re-ecrit lui-meme
      // a sa sortie (exit() vide les tampons stdio), doublant tout ce que
      // le superviseur avait deja tamponne mais pas encore vide.
      std::fflush(nullptr);

      const pid_t worker = ::fork();
      if (worker < 0) {
        // Ne devrait arriver que sous pression memoire/pid extreme ; pas
        // pire que d'executer ce cas en ligne, donc repli ponctuel plutot
        // qu'abandon de la suite entiere. Retente fork() au cas suivant.
        std::fprintf(stderr,
                     "  ATTENTION %s : fork() a echoue (%s), execution "
                     "sans garde-temps pour ce cas\n",
                     cases[next]->name, std::strerror(errno));
        run_case_and_track(*cases[next], ran, failed_cases);
        ++next;
        continue;
      }

      if (worker == 0) {
        // Nouveau groupe de processus : le superviseur pourra tuer d'un
        // coup ce lot ET les enfants directs qu'il aurait lui-meme forkes
        // sans changer de groupe, sans jamais toucher au binaire de test
        // (qui reste dans son groupe d'origine).
        ::setpgid(0, 0);
        run_worker(cases, next, shared);
        // exit() et non _exit() : LeakSanitizer accroche sa verification
        // aux gestionnaires atexit, que seul exit() declenche. _exit() la
        // sauterait silencieusement pour tout ce lot -- un trou de ce
        // genre est precisement ce que ce round corrige, pas quelque
        // chose a en rouvrir un autre.
        std::exit(0);
      }

      // Superviseur. Idiome standard : setpgid() appele ici ET cote
      // enfant juste apres le fork() ; dans cette conception precise, le
      // superviseur n'attend sur les semaphores qu'apres son propre
      // appel, donc lui seul suffit deja a eliminer la course -- celui-ci
      // est la redondance habituelle, pas une necessite stricte ici.
      ::setpgid(worker, worker);

      bool worker_lost = false;
      for (std::size_t i = next; i < cases.size(); ++i) {
        const struct timespec case_deadline =
            compute_absolute_deadline(timeout_ms);
        int death_status = 0;
        int waitpid_errno = 0;
        const CaseWait outcome =
            wait_for_case(shared, worker, case_deadline, kDeathPollMs,
                          &death_status, &waitpid_errno);
        ++ran;

        if (outcome == CaseWait::kFinished) {
          th::failures() += shared->case_new_failures;
          if (shared->case_new_failures > 0) ++failed_cases;
          // Libere l'ouvrier pour le cas suivant -- voir le protocole en
          // tete de fichier.
          ::sem_post(&shared->case_ack);
          continue;
        }

        // Ce cas n'a pas termine a temps : accroche ou ouvrier deja mort
        // (crash). shared->case_name a deja ete ecrit par l'ouvrier avant
        // qu'il ne commence a executer ce cas (voir run_worker) et ne
        // peut pas avoir change depuis -- le protocole a deux semaphores
        // garantit que l'ouvrier n'a pas pu avancer au-dela de ce cas
        // sans que le superviseur l'y autorise, ce qu'il n'a justement
        // pas encore fait ici.
        if (outcome == CaseWait::kWorkerDead) {
          // Crash/signal, constate en au plus kDeathPollMs plutot qu'a
          // l'echeance complete -- voir wait_for_case(). fail_worker_died,
          // pas fail_uncaught : rien n'a ete "lance", le processus entier a
          // disparu (voir harness.hpp pour la distinction).
          th::fail_worker_died(shared->case_name, describe_exit(death_status));
        } else if (outcome == CaseWait::kWaitpidError) {
          th::fail_worker_died(shared->case_name,
                                std::string("waitpid a echoue de facon "
                                            "inattendue : ") +
                                    std::strerror(waitpid_errno));
        } else {
          // kTimedOut : l'ouvrier est toujours vivant a l'echeance globale
          // du cas -- vrai depassement de delai. SIGKILL est imparable
          // mais ne deroule aucun destructeur -- voir la note de
          // conception en tete de fichier. Vise le groupe entier
          // (-worker), pas seulement `worker`.
          ::kill(-worker, SIGKILL);

          // Recolte bornee : SIGKILL est normalement instantane, mais un
          // processus en etat D (E/S noyau non interruptible) peut
          // l'ignorer indefiniment -- aucun mecanisme utilisateur ne peut
          // forcer ca. Mieux vaut abandonner apres 1 s et continuer la
          // suite que de reproduire le blocage qu'on corrige.
          int status = 0;
          bool reaped = false;
          for (int attempt = 0; attempt < 100; ++attempt) {
            if (::waitpid(worker, &status, WNOHANG) > 0) {
              reaped = true;
              break;
            }
            ::usleep(10 * 1000);
          }
          if (!reaped) {
            std::fprintf(stderr,
                         "  ATTENTION %s : processus %d ne repond pas a "
                         "SIGKILL (etat D probable) ; abandonne, la suite "
                         "continue\n",
                         shared->case_name, static_cast<int>(worker));
          }
          th::fail_timeout(shared->case_name, timeout_ms);
        }
        ++failed_cases;
        worker_lost = true;
        next = i + 1;
        break;
      }

      if (!worker_lost) {
        // Lot termine avec succes : l'ouvrier vient de poster le dernier
        // case_done puis d'entrer dans son std::exit(0). Recolte normale,
        // sans borne -- il ne lui reste que sa propre finalisation de
        // runtime a executer, pas un cas de test qui pourrait bloquer.
        int status = 0;
        ::waitpid(worker, &status, 0);
        next = cases.size();
      }
    }
  }

  std::printf("\n%d cas, %d en echec, %d assertions echouees\n", ran,
              failed_cases, th::failures());
  return th::failures() == 0 ? 0 : 1;
}
