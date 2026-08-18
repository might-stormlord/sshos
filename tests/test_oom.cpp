#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "common/oom.hpp"
#include "harness.hpp"

namespace {

// Le reglage du processus courant, lu a la main.
std::string reglage() {
  const int fd = ::open("/proc/self/oom_score_adj", O_RDONLY);
  if (fd < 0) return "<illisible>";
  char buf[64];
  const ssize_t n = ::read(fd, buf, sizeof buf - 1);
  ::close(fd);
  if (n <= 0) return "<vide>";
  std::string s(buf, static_cast<size_t>(n));
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

// DANS UN ENFANT, TOUJOURS. Le reglage du tueur de memoire s'herite : le
// poser dans le processus de test contaminerait tous les cas suivants -- ils
// tournent tous dans le MEME ouvrier (tests/main.cpp) -- et jusqu'aux vrais
// shells que lancent les cas du terminal.
std::string dans_un_enfant(void (*corps)(int)) {
  int tube[2];
  if (::pipe(tube) != 0) return "<pipe a echoue>";
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(tube[0]);
    ::close(tube[1]);
    return "<fork a echoue>";
  }
  if (pid == 0) {
    ::close(tube[0]);
    corps(tube[1]);
    ::_exit(0);
  }
  ::close(tube[1]);
  std::string out;
  char buf[256];
  ssize_t n = 0;
  while ((n = ::read(tube[0], buf, sizeof buf)) > 0) {
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(tube[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);  // rien ne traine derriere un cas
  return out;
}

void dis(int fd, const std::string& s) {
  const ssize_t ignored = ::write(fd, s.data(), s.size());
  (void)ignored;
}

}  // namespace

// LE DEMON SE MET HORS D'ATTEINTE. Il pese quelques megaoctets et porte
// toute la session : le noyau ne doit jamais le choisir pour recuperer de
// la memoire.
TEST(oom_protection_puts_the_process_out_of_reach) {
  const std::string vu = dans_un_enfant([](int fd) {
    const bool ok = sshos::protect_from_oom();
    dis(fd, (ok ? std::string("oui ") : std::string("non ")) + reglage());
  });

  REQUIRE(!vu.empty());
  if (vu.rfind("non ", 0) == 0) {
    // Toute valeur negative demande CAP_SYS_RESOURCE. Sans privilege il n'y
    // a rien a proteger -- et surtout, l'echec doit avoir laisse le reglage
    // INTACT plutot que de le degrader.
    //
    // SOUS ROOT, EN REVANCHE, IL N'Y A PAS D'EXCUSE : sans cette garde, une
    // protection qui n'ecrirait plus rien passerait ce cas en se faisant
    // passer pour un manque de privilege.
    CHECK(::geteuid() != 0);
    CHECK_EQ(vu, std::string("non 0"));
    return;
  }
  CHECK_EQ(vu, std::string("oui -1000"));
}

// ET IL LA REND. Le reglage survit a fork() ET a execve() : un enfant qui
// garderait l'immunite ferait tuer PostgreSQL a la place d'un `make -j12`
// lance dans une fenetre du bureau.
TEST(oom_protection_is_dropped_on_demand) {
  const std::string vu = dans_un_enfant([](int fd) {
    if (!sshos::protect_from_oom()) {
      dis(fd, "sans privilege");
      return;
    }
    sshos::drop_oom_protection();
    dis(fd, reglage());
  });

  REQUIRE(!vu.empty());
  if (vu == "sans privilege") return;
  CHECK_EQ(vu, std::string("0"));
}

// REMONTER EST TOUJOURS PERMIS. C'est ce qui rend l'abandon possible sans
// privilege dans l'enfant, alors que la protection, elle, en demande un :
// le noyau ne garde que la DESCENTE.
TEST(oom_protection_can_be_dropped_by_an_unprivileged_child) {
  const std::string vu = dans_un_enfant([](int fd) {
    if (!sshos::protect_from_oom()) {
      dis(fd, "sans privilege");
      return;
    }
    // Un petit-fils, comme un shell d'invite : il herite de -1000 puis le
    // rend, sans jamais rien demander au noyau qui ne soit une remontee.
    const pid_t pid = ::fork();
    if (pid == 0) {
      sshos::drop_oom_protection();
      dis(fd, "petit-fils " + reglage());
      ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
  });

  REQUIRE(!vu.empty());
  if (vu == "sans privilege") return;
  CHECK_EQ(vu, std::string("petit-fils 0"));
}
