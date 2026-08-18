#include "daemon/journal.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "common/paths.hpp"

namespace sshos {
namespace {

// Le chemin retenu pour le gestionnaire de plantage. Un handler ne peut ni
// allouer ni lire un std::string : la copie est faite ICI, a l'installation,
// dans un tampon de taille fixe.
constexpr size_t kPathMax = 512;
char g_crash_path[kPathMax] = {};

void write_all(int fd, const char* text, size_t n) {
  size_t done = 0;
  while (done < n) {
    const ssize_t w = ::write(fd, text + done, n - done);
    if (w > 0) {
      done += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    return;
  }
}

// Ouvre le journal en ajout, en creant le repertoire s'il manque. Rend -1
// sans rien dire quand ce n'est pas possible.
int open_appending(const char* path) {
  int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd >= 0) return fd;
  if (errno != ENOENT) return -1;
  // Le repertoire de donnees peut ne pas exister encore : une installation
  // neuve n'a jamais rien ecrit. On cree CHAQUE segment -- sur une machine
  // ou personne n'a jamais rien pose sous le home, c'est `.local` et
  // `.local/share` qui manquent avant `sshos`, et un mkdir() du seul
  // dernier segment echouerait sur ENOENT sans rien dire.
  const char* slash = std::strrchr(path, '/');
  if (slash == nullptr || slash == path) return -1;
  const std::string dir(path, static_cast<size_t>(slash - path));
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i < dir.size() && dir[i] != '/') continue;
    ::mkdir(dir.substr(0, i).c_str(), 0700);  // EEXIST est le cas normal
  }
  return ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

// Le numero de signal en decimal, sans allocation : `snprintf` n'est pas
// sur vis-a-vis des signaux.
size_t decimal(int value, char* out, size_t cap) {
  if (cap == 0) return 0;
  char tmp[16];
  size_t n = 0;
  if (value <= 0) {
    tmp[n++] = '0';
  } else {
    while (value > 0 && n < sizeof tmp) {
      tmp[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
  }
  size_t written = 0;
  while (n > 0 && written < cap) out[written++] = tmp[--n];
  return written;
}

extern "C" void crash_note(int sig) {
  if (g_crash_path[0] != '\0') {
    const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
      // PAS D'HORODATAGE : localtime_r n'est pas sur vis-a-vis des signaux,
      // et une ligne sans date vaut mieux qu'un blocage dans un
      // gestionnaire. La ligne « demarrage » qui precede porte la date.
      static const char tete[] = "(arret brutal : signal ";
      write_all(fd, tete, sizeof tete - 1);
      char num[16];
      const size_t n = decimal(sig, num, sizeof num);
      write_all(fd, num, n);
      static const char queue[] = ")\n";
      write_all(fd, queue, sizeof queue - 1);
      ::close(fd);
    }
  }
  // On relance le signal a nu : image memoire, code de sortie, tout ce que
  // le systeme aurait fait sans nous reste vrai.
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

}  // namespace

Journal::Journal(const Platform& plat, std::string path, size_t max_bytes)
    : plat_(&plat), path_(std::move(path)), max_bytes_(max_bytes) {}

void Journal::note(std::string_view evenement) {
  if (path_.empty()) return;
  const int fd = open_appending(path_.c_str());
  if (fd < 0) return;

  // BORNE HAUTE, sinon un demon qui redemarre en boucle ferait grossir ce
  // fichier sans fin. On repart de zero plutot que de faire tourner : ce
  // journal se lit apres un incident, et ce qui vient de se passer compte
  // plus que le mois dernier.
  const off_t taille = ::lseek(fd, 0, SEEK_END);
  if (taille > 0 && static_cast<size_t>(taille) > max_bytes_) {
    if (::ftruncate(fd, 0) == 0) {
      static const char remis[] = "(journal remis a zero)\n";
      write_all(fd, remis, sizeof remis - 1);
    }
  }

  const std::time_t t = std::chrono::system_clock::to_time_t(plat_->now());
  std::tm tm{};
  char ligne[64];
  int tete = 0;
  if (::localtime_r(&t, &tm) != nullptr) {
    tete = std::snprintf(ligne, sizeof ligne, "%04d-%02d-%02d %02d:%02d:%02d ",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                         tm.tm_min, tm.tm_sec);
  }
  if (tete > 0) write_all(fd, ligne, static_cast<size_t>(tete));
  write_all(fd, evenement.data(), evenement.size());
  write_all(fd, "\n", 1);
  ::close(fd);
}

std::string daemon_journal_path() {
  const std::string dir = user_data_dir();
  if (dir.empty()) return {};
  return dir + "/journal.log";
}

void arm_crash_note(const std::string& path) {
  if (path.empty() || path.size() >= kPathMax) return;
  std::memcpy(g_crash_path, path.c_str(), path.size() + 1);

  const int signaux[] = {SIGSEGV, SIGBUS, SIGFPE, SIGABRT};
  for (int sig : signaux) {
    struct sigaction actuel {};
    if (::sigaction(sig, nullptr, &actuel) != 0) continue;
    // DEJA PRIS : ASan et UBSan installent le leur, et leur rapport vaut
    // infiniment mieux que notre ligne. On ne s'impose pas.
    if (actuel.sa_handler != SIG_DFL) continue;
    struct sigaction nouveau {};
    nouveau.sa_handler = &crash_note;
    sigemptyset(&nouveau.sa_mask);
    // Pas de SA_RESTART : rien a reprendre, on ne revient pas.
    nouveau.sa_flags = 0;
    ::sigaction(sig, &nouveau, nullptr);
  }
}

}  // namespace sshos
