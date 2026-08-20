// Le lancement d'un démon et l'attente qu'il écoute.
//
// CE QUE CE FICHIER EXISTE POUR ATTRAPER. Le 19 août 2026, un redémarrage de
// bureau après mise à jour s'est perdu : le client accordait au démon neuf
// 50 tentatives à 20 ms, soit une seconde pile, puis rendait la main au
// shell. Le démon, lui, est bien reparti — treize secondes plus tard. Le
// journal ne portait qu'un trou entre les deux, parce que le geste vivait
// dans src/main.cpp, que CMakeLists retire de sshos_core : aucun test ne
// pouvait l'atteindre. Il vit désormais dans src/client/launch.cpp, et ce
// fichier est la raison pour laquelle il y a été déplacé.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

#include "client/launch.hpp"
#include "common/net.hpp"
#include "daemon/daemon.hpp"
#include "harness.hpp"

namespace {

using namespace std::chrono_literals;

// Même raison que dans test_net.cpp : l'espace de noms des sockets
// abstraites est global au réseau, pas au pid.
std::string nom_unique() {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "sshos-launch/" << ::getpid() << '-' << std::hex << dist(rng);
  return os.str();
}

// UN TEST DOIT RÉCOLTER SES ENFANTS. Tous les cas tournent dans le même
// processus et ceux de test_daemon.cpp appellent reap_children(), qui fait
// waitpid(-1) : un zombie oublié ici est ramassé là-bas, et le try_reap()
// qui l'attendait reçoit ECHILD pour toujours. Le garde vaut aussi sur le
// chemin d'échec d'un REQUIRE, qui sort par un `return` nu.
class Enfant {
 public:
  explicit Enfant(pid_t pid) : pid_(pid) {}
  ~Enfant() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGKILL);
    int st = 0;
    ::waitpid(pid_, &st, 0);
  }
  Enfant(const Enfant&) = delete;
  Enfant& operator=(const Enfant&) = delete;
  bool valid() const { return pid_ > 0; }

 private:
  pid_t pid_;
};

// Un démon de pacotille : il ne fait qu'attendre `delai`, se mettre à
// écouter, et rester là. C'est tout ce dont l'attente a besoin pour être
// éprouvée, et ça évite de lancer un vrai bureau.
pid_t forker_un_ecouteur(const std::string& nom, std::chrono::milliseconds delai) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  ::usleep(static_cast<useconds_t>(delai.count()) * 1000);
  try {
    sshos::Fd l = sshos::bind_abstract(nom);
    for (;;) ::pause();
  } catch (const std::exception&) {
  }
  ::_exit(0);
}

// Le lanceur injecté : il rend le pid d'un intermédiaire qui meurt aussitôt,
// exactement comme spawn_detached. Le vrai écouteur est forké par le test
// lui-même, pour que le test le possède et puisse le récolter.
pid_t lanceur_trivial(const std::vector<std::string>&) {
  const pid_t pid = ::fork();
  if (pid == 0) ::_exit(0);
  return pid;
}

pid_t lanceur_qui_echoue(const std::vector<std::string>&) { return -1; }

}  // namespace

// LE CAS QUI A COÛTÉ UN BUREAU. 1,3 s, c'est au-delà de l'ancien budget
// d'une seconde et bien en deçà de ce qu'une machine qui vient de compiler
// le projet et de passer 1277 tests peut mettre à relancer un démon.
TEST(launch_waits_for_a_daemon_slower_than_the_old_one_second_budget) {
  const std::string nom = nom_unique();
  Enfant ecouteur(forker_un_ecouteur(nom, 1300ms));
  REQUIRE(ecouteur.valid());

  const sshos::DaemonLaunch r =
      sshos::launch_daemon(nom, "/aucun-binaire", {}, sshos::LaunchBudget{},
                           lanceur_trivial);
  CHECK(r == sshos::DaemonLaunch::Connected);
}

// L'ATTENTE DOIT SE DIRE. Un abandon muet est ce qui a fait croire que le
// bureau ne reviendrait pas ; une attente annoncée se supporte.
TEST(launch_says_once_that_the_daemon_is_taking_its_time) {
  const std::string nom = nom_unique();
  Enfant ecouteur(forker_un_ecouteur(nom, 400ms));
  REQUIRE(ecouteur.valid());

  int dits = 0;
  sshos::LaunchBudget budget;
  budget.patience = 100ms;
  const sshos::DaemonLaunch r = sshos::launch_daemon(
      nom, "/aucun-binaire", [&dits] { ++dits; }, budget, lanceur_trivial);

  CHECK(r == sshos::DaemonLaunch::Connected);
  CHECK_EQ(dits, 1);
}

// Rien ne se dit tant que l'attente reste courte : le cas nominal est
// silencieux, et c'est ce qui rend le message utile quand il tombe.
TEST(launch_stays_silent_when_the_daemon_answers_at_once) {
  const std::string nom = nom_unique();
  Enfant ecouteur(forker_un_ecouteur(nom, 0ms));
  REQUIRE(ecouteur.valid());

  int dits = 0;
  const sshos::DaemonLaunch r = sshos::launch_daemon(
      nom, "/aucun-binaire", [&dits] { ++dits; }, sshos::LaunchBudget{},
      lanceur_trivial);

  CHECK(r == sshos::DaemonLaunch::Connected);
  CHECK_EQ(dits, 0);
}

// Le budget reste un budget : personne n'écoute jamais, on finit par le
// dire. Court exprès -- la suite ne doit pas attendre trente secondes pour
// éprouver un abandon.
TEST(launch_gives_up_when_nobody_ever_listens) {
  sshos::LaunchBudget budget;
  budget.total = 200ms;
  budget.patience = 1000ms;
  const sshos::DaemonLaunch r = sshos::launch_daemon(
      nom_unique(), "/aucun-binaire", {}, budget, lanceur_trivial);
  CHECK(r == sshos::DaemonLaunch::TimedOut);
}

// Un fork qui échoue n'est pas une attente qui expire : les deux se
// distinguent, parce que le message à l'utilisateur n'est pas le même.
TEST(launch_tells_a_failed_spawn_apart_from_an_expired_wait) {
  const sshos::DaemonLaunch r = sshos::launch_daemon(
      nom_unique(), "/aucun-binaire", {}, sshos::LaunchBudget{},
      lanceur_qui_echoue);
  CHECK(r == sshos::DaemonLaunch::SpawnFailed);
}

// --- ce que le demon dit quand il n'arrive PAS a demarrer ----------------
//
// Le trou de treize secondes du 19 aout n'etait pas une absence de defaut :
// c'etait une absence de trace. `spawn_detached` redirige 0/1/2 du demon
// vers /dev/null avant l'execv, et le journal n'etait ouvert qu'APRES un
// bind reussi -- un demon qui renonce sortait donc en silence, avec le code
// 0 pour l'adresse deja prise. Une vie qui ne commence pas doit se lire
// quelque part.

namespace {

std::string lire_fichier(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

// Un repertoire a soi. /var/tmp et non /tmp : ce dernier est un tmpfs de
// 2,7 Go sur cette machine, et l'y remplir a deja coute une compilation.
class BacJournal {
 public:
  BacJournal() {
    char tpl[] = "/var/tmp/sshos-launch-XXXXXX";
    const char* fait = ::mkdtemp(tpl);
    if (fait != nullptr) dir_ = fait;
  }
  ~BacJournal() {
    if (dir_.empty()) return;
    ::unlink(fichier().c_str());
    ::rmdir(dir_.c_str());
  }
  BacJournal(const BacJournal&) = delete;
  BacJournal& operator=(const BacJournal&) = delete;
  bool valid() const { return !dir_.empty(); }
  std::string fichier() const { return dir_ + "/journal.log"; }

 private:
  std::string dir_;
};

}  // namespace

TEST(daemon_notes_in_its_journal_that_a_start_was_refused) {
  BacJournal bac;
  REQUIRE(bac.valid());
  const std::string nom = nom_unique();

  // L'adresse est deja a nous : le demon qu'on lance ne pourra pas l'avoir.
  sshos::Fd occupant = sshos::bind_abstract(nom);

  // Ce n'est pas une erreur -- un demon tourne deja -- mais ca doit se lire.
  CHECK_EQ(sshos::run_daemon(nom, bac.fichier()), 0);

  const std::string texte = lire_fichier(bac.fichier());
  CHECK(texte.find("demarrage refuse") != std::string::npos);
  CHECK(texte.find("adresse deja prise") != std::string::npos);
}
