#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
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

// CAP_SYS_RESOURCE EST-ELLE DANS LE JEU EFFECTIF ? Lue dans
// /proc/self/status, sans libcap -- le projet n'a aucune dependance
// externe.
//
// CE N'EST PAS LA MEME CHOSE QU'ETRE ROOT, et c'est tout l'interet de cette
// fonction : un conteneur Docker tourne en root et retire pourtant cette
// capacite de son jeu par defaut. La CI de ce depot est exactement ce
// cas-la, et une version anterieure de ce fichier -- qui interrogeait
// geteuid() -- y a echoue alors qu'elle passait sur la machine de
// developpement.
bool peut_baisser_le_score() {
  const int fd = ::open("/proc/self/status", O_RDONLY);
  if (fd < 0) return false;
  std::string tout;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) tout.append(buf, static_cast<size_t>(n));
  ::close(fd);

  const size_t at = tout.find("CapEff:");
  if (at == std::string::npos) return false;
  size_t i = at + 7;
  while (i < tout.size() && (tout[i] == ' ' || tout[i] == '\t')) ++i;
  unsigned long long masque = 0;
  for (; i < tout.size() && std::isxdigit(static_cast<unsigned char>(tout[i])); ++i) {
    masque = masque * 16 + static_cast<unsigned long long>(
                              std::isdigit(static_cast<unsigned char>(tout[i]))
                                  ? tout[i] - '0'
                                  : std::tolower(tout[i]) - 'a' + 10);
  }
  // CAP_SYS_RESOURCE vaut 24 (linux/capability.h).
  return (masque >> 24 & 1ULL) != 0;
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
  // LES DEUX SENS SONT AFFIRMES, sans echappatoire : avec la capacite la
  // protection DOIT reussir -- sans quoi une protection qui n'ecrirait plus
  // rien passerait ce cas en se faisant passer pour un manque de privilege
  // -- et sans elle elle DOIT echouer en laissant le reglage intact plutot
  // qu'en le degradant.
  if (peut_baisser_le_score()) {
    CHECK_EQ(vu, std::string("oui -1000"));
  } else {
    CHECK_EQ(vu, std::string("non 0"));
  }
}

// ET IL LA REND. Le reglage survit a fork() ET a execve() : un enfant qui
// garderait l'immunite ferait tuer PostgreSQL a la place d'un `make -j12`
// lance dans une fenetre du bureau.
TEST(oom_protection_is_dropped_on_demand) {
  const std::string vu = dans_un_enfant([](int fd) {
    if (!sshos::protect_from_oom()) {
      dis(fd, "sans la capacite");
      return;
    }
    sshos::drop_oom_protection();
    dis(fd, reglage());
  });

  REQUIRE(!vu.empty());
  if (vu == "sans la capacite") return;
  CHECK_EQ(vu, std::string("0"));
}

// REMONTER EST TOUJOURS PERMIS. C'est ce qui rend l'abandon possible sans
// aucune capacite dans l'enfant, alors que la protection, elle, demande
// CAP_SYS_RESOURCE : le noyau ne garde que la DESCENTE.
TEST(oom_protection_can_be_dropped_by_an_unprivileged_child) {
  const std::string vu = dans_un_enfant([](int fd) {
    if (!sshos::protect_from_oom()) {
      dis(fd, "sans la capacite");
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
  if (vu == "sans la capacite") return;
  CHECK_EQ(vu, std::string("petit-fils 0"));
}
