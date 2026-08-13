#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <chrono>
#include <string>
#include <ctime>
#include <vector>

#include <memory>

#include "app/app.hpp"
#include "common/platform.hpp"
#include "daemon/reap.hpp"
#include "pty/env.hpp"
#include "pty/pty.hpp"
#include "daemon/session.hpp"
#include "render/surface.hpp"
#include "harness.hpp"

using sshos::ChildSink;
using sshos::reap_children;

namespace {

// Ce que la récolte rapporte, enregistré sans rien exécuter.
struct Recorder : ChildSink {
  std::vector<pid_t> pids;
  std::vector<int> statuses;
  void on_child_exit(pid_t pid, int status) override {
    pids.push_back(pid);
    statuses.push_back(status);
  }
};

// Un enfant qui meurt tout de suite, avec le code demandé.
pid_t spawn_dying_child(int code) {
  const pid_t pid = ::fork();
  if (pid == 0) ::_exit(code);
  return pid;
}

void sleep_ms(int ms) {
  timespec ts{ms / 1000, static_cast<long>(ms % 1000) * 1000000L};
  ::nanosleep(&ts, nullptr);
}

// L'état d'un processus, lu dans /proc : `Z` veut dire mort mais pas
// encore récolté. C'est le seul moyen d'attendre que les trois soient
// morts SANS les récolter -- et sans cela, un test qui récolte en boucle
// ne peut pas distinguer une boucle de récolte d'un appel unique.
char proc_state(pid_t pid) {
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  FILE* f = ::fopen(path.c_str(), "re");
  if (f == nullptr) return '?';
  char buf[512] = {0};
  const size_t n = ::fread(buf, 1, sizeof buf - 1, f);
  ::fclose(f);
  if (n == 0) return '?';
  // Le nom de la commande est entre parenthèses et peut contenir des
  // espaces : l'état est le premier caractère non blanc APRÈS la dernière
  // parenthèse fermante.
  const std::string line(buf, n);
  const size_t close = line.rfind(')');
  if (close == std::string::npos || close + 2 >= line.size()) return '?';
  return line[close + 2];
}

bool wait_until_zombie(pid_t pid, int budget_ms) {
  for (int waited = 0; waited <= budget_ms; waited += 5) {
    if (proc_state(pid) == 'Z') return true;
    sleep_ms(5);
  }
  return false;
}

// Récolte jusqu'à en avoir `want`, ou jusqu'à épuisement du budget. Les
// enfants meurent tout de suite, mais « tout de suite » n'est pas
// « avant que le parent ne reprenne la main ».
int reap_until(ChildSink& sink, int want, int budget_ms) {
  int total = 0;
  for (int waited = 0; waited <= budget_ms; waited += 5) {
    total += reap_children(sink);
    if (total >= want) return total;
    sleep_ms(5);
  }
  return total;
}

}  // namespace

// LE cas qui justifie la boucle. Les signaux standards ne sont pas mis en
// file : trois enfants morts entre deux lectures du `signalfd` ne
// produisent qu'UN enregistrement. Récolter le seul pid qu'il nomme
// laisserait les deux autres en zombies, et leurs maîtres de
// pseudo-terminaux jamais fermés -- jusqu'à épuiser `kernel.pty.max`.
TEST(reap_collects_three_children_that_died_at_once) {
  Recorder rec;
  std::vector<pid_t> spawned;
  for (int i = 0; i < 3; ++i) spawned.push_back(spawn_dying_child(0));
  for (pid_t p : spawned) REQUIRE(p > 0);
  // Les trois sont morts AVANT le premier appel : la récolte doit donc les
  // ramener tous EN UNE FOIS. Récolter en boucle depuis le test ne
  // prouverait rien -- c'est la boucle du code qu'on mesure.
  for (pid_t p : spawned) REQUIRE(wait_until_zombie(p, 3000));

  const int reaped = reap_children(rec);
  CHECK_EQ(reaped, 3);
  REQUIRE_EQ(rec.pids.size(), size_t{3});

  // Aucun zombie ne subsiste : un enfant déjà récolté n'existe plus, et
  // `waitpid` le dit en échouant.
  for (pid_t p : spawned) {
    int st = 0;
    CHECK_EQ(::waitpid(p, &st, WNOHANG), -1);
  }
}

TEST(reap_reports_the_exit_status_it_collected) {
  Recorder rec;
  const pid_t pid = spawn_dying_child(7);
  REQUIRE(pid > 0);

  REQUIRE_EQ(reap_until(rec, 1, 3000), 1);
  REQUIRE_EQ(rec.pids.size(), size_t{1});
  CHECK_EQ(rec.pids[0], pid);
  CHECK(WIFEXITED(rec.statuses[0]));
  CHECK_EQ(WEXITSTATUS(rec.statuses[0]), 7);
}

// Rien de mort : la boucle rend zéro et ne bloque pas. C'est le cas de
// TOUS les réveils qui ne sont pas des morts d'enfant.
TEST(reap_collects_nothing_when_no_child_died) {
  Recorder rec;
  CHECK_EQ(reap_children(rec), 0);
  CHECK(rec.pids.empty());
}

// ------------------------------------------------- l'acheminement au bureau

// La récolte est GLOBALE au processus : le démon apprend qu'un pid est
// mort sans savoir à qui il était. C'est la table de la session qui le
// retrouve -- et le test suivant vérifie qu'elle le retrouve BIEN, parce
// qu'un enfant livré à la mauvaise fenêtre est pire qu'un enfant perdu.
namespace {

struct CountingApp : sshos::App {
  int exits = 0;
  int last_status = 0;
  void render(sshos::View) override {}
  void on_child_exit(int status) override {
    ++exits;
    last_status = status;
  }
};

sshos::NullFdRegistrar g_fds;

// Horloge figée : la session en veut une, et un test de récolte n'a rien
// à dire sur l'heure.
struct StillPlatform : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(1786370700));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return {};
  }
  std::string read_file(std::string_view) const override { return {}; }
};

}  // namespace

TEST(session_delivers_a_child_death_to_the_window_that_asked) {
  StillPlatform plat;
  sshos::Session sess(plat, g_fds, 40, 12);
  sshos::Surface s(40, 12);
  sess.render(s);  // amorce la première fenêtre

  const sshos::WindowId id = sess.open_from_catalog("bloc");
  REQUIRE(id != 0);
  sshos::Window* w = sess.window_for_tests(id);
  REQUIRE(w != nullptr);

  auto app = std::make_unique<CountingApp>();
  CountingApp* raw = app.get();
  w->app = std::move(app);
  w->host->watch_child(4242);

  REQUIRE_EQ(sess.watched_children_for_tests(), size_t{1});
  sess.take_dirty();  // on part d'une ardoise propre

  sess.on_child_exit(4242, 0x0700);
  CHECK_EQ(raw->exits, 1);
  CHECK_EQ(raw->last_status, 0x0700);
  // L'entrée ne sert plus à rien : la garder ferait livrer une seconde
  // fois si le noyau réattribuait le numéro.
  CHECK_EQ(sess.watched_children_for_tests(), size_t{0});
  // Et ce que l'application vient d'apprendre change ce qu'elle affiche.
  CHECK(sess.take_dirty());
}

// Un pid qui n'est pas dans la table -- enfant d'une fenêtre déjà fermée,
// ou processus qui ne nous appartient pas -- ne doit RIEN déclencher. Ce
// n'est pas une anomalie : c'est le cas normal d'un démon qui récolte tout
// ce qui meurt sous lui.
TEST(session_ignores_a_child_it_never_watched) {
  StillPlatform plat;
  sshos::Session sess(plat, g_fds, 40, 12);
  sshos::Surface s(40, 12);
  sess.render(s);

  const sshos::WindowId id = sess.open_from_catalog("bloc");
  sshos::Window* w = sess.window_for_tests(id);
  REQUIRE(w != nullptr);
  auto app = std::make_unique<CountingApp>();
  CountingApp* raw = app.get();
  w->app = std::move(app);
  w->host->watch_child(4242);

  sess.on_child_exit(9999, 0);
  CHECK_EQ(raw->exits, 0);
}

// Une fenêtre fermée emporte ses enfants surveillés. Les laisser dans la
// table ferait livrer leur mort à une fenêtre disparue -- ou pire, à celle
// qui reprendrait son numéro.
TEST(session_forgets_the_children_of_a_window_it_closes) {
  StillPlatform plat;
  sshos::Session sess(plat, g_fds, 40, 12);
  sshos::Surface s(40, 12);
  sess.render(s);

  const sshos::WindowId id = sess.open_from_catalog("bloc");
  sshos::Window* w = sess.window_for_tests(id);
  REQUIRE(w != nullptr);
  w->host->watch_child(4242);
  CHECK_EQ(sess.watched_children_for_tests(), size_t{1});

  sess.close_window_for_tests(id);
  CHECK_EQ(sess.watched_children_for_tests(), size_t{0});
}

// ------------------------------------- la sortie survit a la mort de l'enfant

// On ne ferme JAMAIS le maître sur simple réception de `SIGCHLD` : cela
// jetterait la sortie encore en tampon dans la discipline de ligne. Le
// test le prouve dans le pire ordre possible -- l'enfant écrit, meurt, est
// RÉCOLTÉ, et c'est seulement après qu'on lit.
TEST(reap_does_not_swallow_what_the_child_wrote_before_dying) {
  sshos::Pty p;
  sshos::PtySpawn spec;
  spec.path = "/bin/sh";
  spec.argv = {"/bin/sh", "-c", "printf 'dernier mot'"};
  spec.env = sshos::child_env({"PATH=/usr/local/bin:/usr/bin:/bin"}, {});
  spec.cols = 80;
  spec.rows = 24;
  REQUIRE_EQ(p.spawn(spec), std::string());

  Recorder rec;
  const pid_t child = p.pid();
  REQUIRE(reap_until(rec, 1, 3000) >= 1);
  REQUIRE_EQ(rec.pids.size(), size_t{1});
  REQUIRE_EQ(rec.pids[0], child);

  // La récolte est passée. Les octets doivent toujours être là.
  std::string got;
  for (int waited = 0; waited < 2000 && got.find("dernier mot") == std::string::npos;
       waited += 10) {
    char buf[4096];
    const ssize_t n = p.read(buf, sizeof buf);
    if (n > 0) {
      got.append(buf, static_cast<size_t>(n));
      continue;
    }
    sleep_ms(10);
  }
  CHECK(got.find("dernier mot") != std::string::npos);
}

// La récolte NE BLOQUE PAS. Un enfant bien vivant ne doit pas immobiliser
// la boucle du démon -- qui est mono-thread, et où l'attendre gèlerait
// toutes les fenêtres et tous les clients.
TEST(reap_does_not_wait_for_a_child_that_is_still_alive) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::pause();  // vivant, et sans rien faire
    ::_exit(0);
  }
  REQUIRE(pid > 0);

  Recorder rec;
  CHECK_EQ(reap_children(rec), 0);
  CHECK(rec.pids.empty());

  ::kill(pid, SIGKILL);
  REQUIRE(wait_until_zombie(pid, 3000));
  CHECK_EQ(reap_children(rec), 1);
}
