#include "client/tty_guard.hpp"

#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>

namespace sshos {
namespace {

// Descripteur du terminal du client courant, lu par le gestionnaire de
// signal fatal. `volatile sig_atomic_t` et non `int` : cette variable est
// écrite par du code normal (TtyGuard::TtyGuard / ~TtyGuard) et lue par un
// gestionnaire de signal, qui peut s'exécuter à tout moment sur le même
// thread. Sans `volatile` le compilateur pourrait garder une copie en
// registre côté code normal et ne jamais revoir la valeur posée par le
// gestionnaire ; `sig_atomic_t` est le seul type dont la lecture/écriture
// est garantie exempte de déchirure (torn read/write) face à un signal.
volatile sig_atomic_t g_crash_fd = -1;

// Copie du termios d'origine, à restaurer par le gestionnaire de plantage.
// Écrite une seule fois dans TtyGuard::TtyGuard, avant que g_crash_fd ne
// soit rendu visible au gestionnaire (l'ordre des deux affectations
// ci-dessous est significatif) : au moment où le gestionnaire peut la lire,
// elle est déjà stable et ne bouge plus jusqu'au destructeur.
termios g_crash_saved{};

// Littéral de restauration, écrit directement par le gestionnaire de signal
// fatal SANS passer par tty_restore_sequence(). tty_restore_sequence()
// renvoie une std::string *par valeur* : elle alloue, et malloc() n'est pas
// dans la liste des fonctions sûres en contexte de signal (man 7
// signal-safety). La cause la plus commune d'un SIGABRT est précisément le
// détecteur de corruption de tas de glibc qui appelle abort() depuis
// l'intérieur de malloc(), verrou de tas tenu : un gestionnaire qui alloue à
// cet instant précis se bloque pour toujours, et le terminal n'est jamais
// restauré. Un tableau `constexpr` de portée fichier n'alloue rien.
// crash_restore_literal_for_tests() l'expose pour qu'un test vérifie qu'il
// reste identique, octet pour octet, à tty_restore_sequence() -- sans ce
// test les deux pourraient dériver en silence l'un de l'autre au premier
// changement de l'un sans l'autre.
constexpr char kCrashRestoreLiteral[] =
    "\033[?25h\033[?7h\033[?1004l\033[?2004l\033[?1006l\033[?1002l\033[?1049l";

void write_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

// Version sans allocation, pour le gestionnaire de signal : `len` est connu
// à la compilation (sizeof du littéral moins le terminateur), aucune
// std::string n'est construite.
void write_literal(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, data + off, len - off);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

// Logique de restauration partagée par le vrai gestionnaire de signal et par
// run_crash_restore_for_tests() : écrire le littéral (voir kCrashRestoreLiteral
// ci-dessus, pas d'allocation) puis remettre le termios sauvegardé.
// tcsetattr() figure dans la liste POSIX des fonctions async-signal-safe
// (confirmé sur ce système via `man 7 signal-safety`), donc l'appeler ici
// est possible et nécessaire : le gestionnaire de référence du plan ne fait
// que les séquences d'échappement, ce qui laisse le terminal en mode brut
// après tout plantage -- ni écho, ni édition de ligne, un état dont
// l'utilisateur ne peut sortir qu'en connaissant `stty sane`.
void crash_restore(int fd) {
  if (fd < 0) return;
  write_literal(fd, kCrashRestoreLiteral, sizeof(kCrashRestoreLiteral) - 1);
  ::tcsetattr(fd, TCSANOW, &g_crash_saved);
}

extern "C" void on_fatal(int sig) {
  crash_restore(g_crash_fd);
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

}  // namespace

std::string tty_setup_sequence() {
  return "\033[?1049h\033[?1002h\033[?1006h\033[?2004h\033[?1004h\033[?7l";
}

std::string tty_restore_sequence() {
  return "\033[?25h\033[?7h\033[?1004l\033[?2004l\033[?1006l\033[?1002l\033[?1049l";
}

const char* crash_restore_literal_for_tests() { return kCrashRestoreLiteral; }

void run_crash_restore_for_tests() { crash_restore(g_crash_fd); }

std::vector<std::pair<std::string, std::string>> collect_env_delta() {
  static constexpr std::array<const char*, 6> kKeys{
      "SSH_AUTH_SOCK", "SSH_CONNECTION", "SSH_CLIENT",
      "SSH_TTY",       "DISPLAY",        "XDG_SESSION_ID"};
  std::vector<std::pair<std::string, std::string>> out;
  for (const char* k : kKeys) {
    if (const char* v = std::getenv(k); v != nullptr) out.emplace_back(k, v);
  }
  return out;
}

TtyGuard::TtyGuard(int fd) : fd_(fd) {
  if (::tcgetattr(fd_, &saved_) != 0) return;
  termios raw = saved_;
  ::cfmakeraw(&raw);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (::tcsetattr(fd_, TCSANOW, &raw) != 0) return;
  armed_ = true;
  // g_crash_saved avant g_crash_fd : le gestionnaire de signal ne doit
  // jamais voir un descripteur "armé" pointant vers un termios pas encore
  // écrit.
  g_crash_saved = saved_;
  g_crash_fd = fd_;
  write_all(fd_, tty_setup_sequence());
}

TtyGuard::~TtyGuard() {
  if (!armed_) return;
  // Désarmer le gestionnaire de signal avant de commencer notre propre
  // restauration : un plantage survenant pendant cette restauration ne doit
  // pas faire écrire le gestionnaire par-dessus une séquence déjà à moitié
  // envoyée.
  g_crash_fd = -1;
  write_all(fd_, tty_restore_sequence());
  ::tcsetattr(fd_, TCSANOW, &saved_);
}

void TtyGuard::install_crash_handlers() {
  for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTERM, SIGINT}) {
    ::signal(sig, on_fatal);
  }
}

}  // namespace sshos
