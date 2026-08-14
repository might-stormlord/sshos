#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.hpp"
#include "pty/env.hpp"
#include "pty/pty.hpp"

using sshos::Pty;
using sshos::PtySpawn;

namespace {

// Une seconde de patience : ces cas lancent de vrais processus, et la
// machine peut être chargée. Sans borne, un défaut se manifesterait par une
// suite qui ne rend jamais la main plutôt que par un échec.
constexpr int kDeadlineMs = 4000;

void nap_ms(int ms) {
  timespec ts{ms / 1000, static_cast<long>(ms % 1000) * 1000000L};
  ::nanosleep(&ts, nullptr);
}

// Lit le maître jusqu'à voir `needle`, ou jusqu'à la limite. Rend tout ce
// qui a été lu, pour que l'échec montre ce qui est arrivé au lieu de
// « faux ».
std::string read_until(Pty& p, const std::string& needle, int deadline_ms = kDeadlineMs) {
  std::string got;
  for (int waited = 0; waited < deadline_ms; waited += 10) {
    char buf[4096];
    const ssize_t n = p.read(buf, sizeof buf);
    if (n > 0) {
      got.append(buf, static_cast<size_t>(n));
      if (!needle.empty() && got.find(needle) != std::string::npos) return got;
      continue;  // il y avait de quoi lire : peut-être encore
    }
    nap_ms(10);
  }
  return got;
}

PtySpawn shell_running(const std::string& script) {
  PtySpawn s;
  s.path = "/bin/sh";
  s.argv = {"/bin/sh", "-c", script};
  s.env = sshos::child_env({"PATH=/usr/local/bin:/usr/bin:/bin"}, {});
  s.cols = 80;
  s.rows = 24;
  return s;
}

// SANS SHELL AU MILIEU. `dash` remet lui-même le masque de signaux à vide
// avant d'exécuter quoi que ce soit : un test qui interroge l'enfant à
// travers `sh -c` mesure donc le shell et non notre code, et passe même
// quand l'assainissement est retiré. Trouvé par mutation, pas à la lecture.
//
// Construit à la main plutôt que par copie d'un shell_running() modifié :
// g++ 15 émet un faux -Wfree-nonheap-object en inlinant la chaîne
// initializer_list -> vector -> copie, et -Werror en fait une erreur.
PtySpawn program_running(std::vector<std::string> argv) {
  PtySpawn s;
  s.path = argv.empty() ? std::string() : argv.front();
  s.argv = std::move(argv);
  s.env = sshos::child_env({"PATH=/usr/local/bin:/usr/bin:/bin"}, {});
  s.cols = 80;
  s.rows = 24;
  return s;
}

// Attend la mort de l'enfant sans jamais bloquer indéfiniment.
bool reaped_within(Pty& p, int deadline_ms = kDeadlineMs) {
  for (int waited = 0; waited < deadline_ms; waited += 10) {
    if (p.try_reap()) return true;
    nap_ms(10);
  }
  return false;
}

// La valeur hexadécimale d'un champ de /proc/self/status (SigBlk, SigIgn).
unsigned long long field_of(const std::string& text, const std::string& name) {
  const size_t at = text.find(name + ":");
  if (at == std::string::npos) return ~0ULL;  // absent : l'échec sera visible
  size_t start = text.find_first_not_of(" \t", at + name.size() + 1);
  if (start == std::string::npos) return ~0ULL;
  const size_t end = text.find_first_of(" \t\r\n", start);
  return std::strtoull(text.substr(start, end - start).c_str(), nullptr, 16);
}

}  // namespace

TEST(pty_runs_a_process_and_reads_what_it_writes) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("printf 'bonjour'")), std::string());
  CHECK(p.master() >= 0);
  CHECK(p.pid() > 0);
  const std::string out = read_until(p, "bonjour");
  CHECK(out.find("bonjour") != std::string::npos);
  CHECK(reaped_within(p));
}

// Le maître est non bloquant : la boucle du démon est mono-thread, et un
// read() bloquant sur un terminal muet gèlerait TOUTES les sessions.
TEST(pty_master_never_blocks_on_a_silent_child) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("read ignore")), std::string());
  char buf[64];
  const ssize_t n = p.read(buf, sizeof buf);
  CHECK(n < 0);  // rien à lire, et on rend la main tout de suite
  p.hangup();
  CHECK(reaped_within(p));
}

// PREMIER des trois héritages qui traversent execve. Le démon bloque
// SIGCHLD pour le recevoir par signalfd ; un enfant qui hérite de ce
// blocage casse « make -j8 », qui n'apprend jamais que ses compilateurs
// sont morts. Rien dans le code de l'enfant ne le laisse voir.
TEST(pty_hands_the_child_an_empty_signal_mask) {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGCHLD);
  sigaddset(&block, SIGUSR1);
  sigset_t saved;
  ::sigprocmask(SIG_BLOCK, &block, &saved);

  Pty p;
  const std::string err =
      p.spawn(program_running({"/bin/grep", "SigBlk", "/proc/self/status"}));
  const std::string out = read_until(p, "SigBlk");
  ::sigprocmask(SIG_SETMASK, &saved, nullptr);

  REQUIRE_EQ(err, std::string());
  CHECK_EQ(field_of(out, "SigBlk"), 0ULL);
  CHECK(reaped_within(p));
}

// DEUXIÈME héritage. Le démon met SIGPIPE à SIG_IGN ; un enfant qui en
// hérite ne s'arrête plus jamais sur un tuyau fermé, et « yes | head -1 »
// tourne indéfiniment. On interroge le masque plutôt que le symptôme :
// certaines versions de `yes` sortent d'elles-mêmes sur EPIPE, ce qui
// ferait passer le test alors que la disposition serait fausse.
TEST(pty_hands_the_child_default_dispositions) {
  struct sigaction ign {};
  ign.sa_handler = SIG_IGN;
  struct sigaction saved_pipe {};
  struct sigaction saved_hup {};
  ::sigaction(SIGPIPE, &ign, &saved_pipe);
  ::sigaction(SIGHUP, &ign, &saved_hup);

  Pty p;
  const std::string err =
      p.spawn(program_running({"/bin/grep", "SigIgn", "/proc/self/status"}));
  const std::string out = read_until(p, "SigIgn");
  ::sigaction(SIGPIPE, &saved_pipe, nullptr);
  ::sigaction(SIGHUP, &saved_hup, nullptr);

  REQUIRE_EQ(err, std::string());
  const unsigned long long ignored = field_of(out, "SigIgn");
  // Bit n-1 pour le signal n : SIGPIPE vaut 13, SIGHUP vaut 1.
  CHECK((ignored & (1ULL << (SIGPIPE - 1))) == 0);
  CHECK((ignored & (1ULL << (SIGHUP - 1))) == 0);
  CHECK(reaped_within(p));
}

// TROISIÈME héritage. Un maître fuité laisse le shell de la fenêtre A lire
// la sortie de la fenêtre B, et empêche à jamais la libération du PTY de A.
// Sans shell au milieu, pour la même raison qu'au-dessus : on veut mesurer
// ce que NOUS laissons passer.
TEST(pty_never_leaks_another_windows_master) {
  Pty a;
  REQUIRE_EQ(a.spawn(shell_running("read ignore")), std::string());
  const int leaked = a.master();
  REQUIRE(leaked >= 0);

  Pty b;
  REQUIRE_EQ(b.spawn(program_running({"/bin/ls", "-l", "/proc/self/fd"})),
             std::string());
  const std::string out = read_until(b, "");

  // On regarde les CIBLES, pas les numéros. Le numéro seul ne prouve rien :
  // dans l'enfant de B, `ls` ouvre lui-même /proc/self/fd et reçoit le 3 --
  // exactement le numéro qu'a le maître de A dans le parent. Un maître
  // fuité, lui, pointe sur /dev/ptmx, ce que rien d'autre ne fait.
  if (out.find("ptmx") != std::string::npos) {
    th::fail(__FILE__, __LINE__,
             "un maitre de PTY est visible dans l'enfant d'une autre "
             "fenetre : " + out);
  }
  // Et le test n'est pas creux : on a bien lu la liste d'un enfant de PTY.
  CHECK(out.find("/dev/pts/") != std::string::npos);

  a.hangup();
  b.hangup();
  CHECK(reaped_within(a));
  CHECK(reaped_within(b));
}

// setsid() + TIOCSCTTY, sans quoi Ctrl+C ne génère aucun signal et le
// contrôle de tâches du shell ne fonctionne pas. On teste l'effet, pas
// l'appel : un \003 doit tuer l'enfant.
TEST(pty_gives_the_child_a_controlling_terminal) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("printf 'pret'; sleep 30")), std::string());
  REQUIRE(read_until(p, "pret").find("pret") != std::string::npos);

  const char intr = '\003';
  CHECK(p.write(&intr, 1) == 1);
  CHECK(reaped_within(p));
}

// La taille faisant autorité est celle du PTY. `stty size` rend « lignes
// colonnes ».
TEST(pty_sets_the_window_size_the_child_sees) {
  PtySpawn s = shell_running("stty size");
  s.cols = 132;
  s.rows = 43;
  Pty p;
  REQUIRE_EQ(p.spawn(s), std::string());
  const std::string out = read_until(p, "43");
  CHECK(out.find("43 132") != std::string::npos);
  CHECK(reaped_within(p));
}

// Le redimensionnement atteint un enfant DÉJÀ lancé. L'enfant attend une
// ligne avant de mesurer : le test ne dépend donc d'aucun délai.
TEST(pty_resize_reaches_a_child_already_running) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("read ignore; stty size")), std::string());
  p.resize(100, 30);
  const char nl = '\n';
  CHECK(p.write(&nl, 1) == 1);
  const std::string out = read_until(p, "30");
  CHECK(out.find("30 100") != std::string::npos);
  CHECK(reaped_within(p));
}

// Un exec raté doit se dire, et ne pas laisser un enfant fantôme derrière
// lui. Sans le tuyau de rapport, la fenêtre resterait vide sans que
// personne sache pourquoi.
TEST(pty_reports_a_failed_exec_instead_of_leaving_a_ghost) {
  PtySpawn s = shell_running("");
  s.path = "/nexiste/pas/du/tout";
  s.argv = {"/nexiste/pas/du/tout"};
  Pty p;
  const std::string err = p.spawn(s);
  CHECK(!err.empty());
  CHECK(err.find("nexiste") != std::string::npos);
  CHECK(p.pid() <= 0);      // rien à récolter
  CHECK(p.master() < 0);    // et rien à fermer
}

// Les deux drapeaux de fin, INDÉPENDANTS DANS LES DEUX SENS : un
// « nohup … & » garde l'esclave ouvert après la mort du shell, et un enfant
// qui se démonise ferme l'esclave avant sa propre mort.
TEST(pty_keeps_the_two_deaths_apart) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("printf 'la'; exit 3")), std::string());
  REQUIRE(read_until(p, "la").find("la") != std::string::npos);
  CHECK(reaped_within(p));
  CHECK(p.exited());
  CHECK_EQ(p.exit_code(), 3);
  // La mort du processus ne ferme pas le maître : la sortie encore en
  // tampon dans la discipline de ligne serait jetée avec lui.
  CHECK(p.master() >= 0);
}

// On draine jusqu'à EIO ; on ne ferme jamais le maître sur simple réception
// de SIGCHLD. Les noyaux récents livrent d'abord les données restantes.
TEST(pty_delivers_the_last_bytes_written_before_the_child_died) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("printf 'dernier mot'; exit 0")), std::string());
  const std::string out = read_until(p, "dernier mot");
  CHECK(out.find("dernier mot") != std::string::npos);
  CHECK(reaped_within(p));
}

// Fermer deux fois, récolter deux fois, redimensionner un PTY mort : rien
// de tout ça ne doit tomber. Le liant appellera ces méthodes dans un ordre
// que la mort de l'invité rend imprévisible.
TEST(pty_tolerates_being_taken_down_twice) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("exit 0")), std::string());
  CHECK(reaped_within(p));
  CHECK(!p.try_reap());  // déjà récolté : plus rien à dire
  p.close_master();
  CHECK_EQ(p.master(), -1);  // le numéro périmé ne doit pas rester en main
  p.close_master();
  CHECK_EQ(p.master(), -1);
  p.resize(10, 10);
  p.hangup();
  char buf[8];
  CHECK(p.read(buf, sizeof buf) < 0);
  CHECK(p.write("x", 1) < 0);
}

// La sortie d'ERREUR de l'invité doit atterrir dans la fenêtre. Sans le
// troisième dup2, elle part sur la sortie d'erreur du démon -- c'est-à-dire
// nulle part de visible -- et un « command not found » disparaît en
// silence, ce qui est exactement le message qu'on avait besoin de lire.
TEST(pty_puts_the_guests_error_output_in_the_window) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running("printf 'sur la sortie d erreur' >&2")),
             std::string());
  const std::string out = read_until(p, "erreur");
  CHECK(out.find("sur la sortie d erreur") != std::string::npos);
  CHECK(reaped_within(p));
}

// SIGHUP part au GROUPE, pas au seul enfant : un shell a des
// petits-enfants, et ne prévenir que lui laisse la compilation tourner
// jusqu'au bout, orpheline et invisible.
//
// Le piège, trouvé par mutation : tuer le seul shell suffit APPAREMMENT,
// parce que le noyau envoie de lui-même SIGHUP au groupe de premier plan
// quand le chef de session meurt. Un test qui laisse le shell mourir ne
// distingue donc rien. On garde ici le shell EN VIE -- il attrape le
// signal -- pour que seul notre envoi puisse atteindre le pipeline.
//
// `trap` avec une commande, et non `trap ""` : un signal IGNORÉ à l'exec
// reste ignoré chez les enfants, ce qui protégerait aussi le pipeline et
// rendrait le test creux une seconde fois.
//
// Le marqueur d'amorçage passe PAR LE TUBE, et c'est tout le sujet. Émis
// avant le pipeline (`printf pret; sleep 5 | cat`), il ne prouve rien : on
// raccrochait alors que le pipeline n'était pas encore né, le shell
// attrapait le HUP tout seul, puis lançait tranquillement ses cinq
// secondes de `sleep` -- échec sept fois sur dix. Émis À GAUCHE et relayé
// par `cat`, il ne peut arriver que si les DEUX membres tournent : le
// gauche pour l'écrire, le droit pour le rendre. La garantie est causale
// et non plus un pari sur l'ordre des fork.
TEST(pty_hangs_up_the_whole_process_group) {
  Pty p;
  REQUIRE_EQ(p.spawn(shell_running(
                 "trap 'printf RACCROCHE' HUP; "
                 "{ printf 'pret\\n'; sleep 5; } | cat; printf 'FINI'")),
             std::string());
  REQUIRE(read_until(p, "pret").find("pret") != std::string::npos);

  p.hangup();

  // Si le pipeline a reçu le signal, il meurt tout de suite et le shell
  // reprend la main. Sinon il faut attendre les cinq secondes de `sleep`.
  const std::string out = read_until(p, "FINI", 2000);
  if (out.find("FINI") == std::string::npos) {
    p.kill_now();  // on ne laisse rien traîner derrière un échec
    reaped_within(p, 1000);
    th::fail(__FILE__, __LINE__,
             "le pipeline a survecu au raccrochage : le signal n'est pas "
             "parti au groupe. Lu : " + out);
  }
  p.kill_now();
  reaped_within(p, 2000);
}


// L'état d'un processus tel que le noyau le voit : `R`/`S` vivant, `Z`
// zombie non récolté, `.` parti pour de bon. `kill(pid, 0)` ne suffit pas
// -- il réussit sur un zombie, et ferait passer un shell bel et bien mort
// pour un shell qui a survécu.
char proc_state(pid_t pid) {
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  FILE* f = ::fopen(path.c_str(), "re");
  if (f == nullptr) return '.';
  char buf[512] = {0};
  const size_t n = ::fread(buf, 1, sizeof buf - 1, f);
  ::fclose(f);
  const std::string line(buf, n);
  const size_t close = line.rfind(')');
  if (close == std::string::npos || close + 2 >= line.size()) return '?';
  return line[close + 2];
}

bool gone_or_dead(pid_t pid, int deadline_ms = kDeadlineMs) {
  for (int waited = 0; waited < deadline_ms; waited += 10) {
    const char st = proc_state(pid);
    if (st == '.' || st == 'Z') return true;
    nap_ms(10);
  }
  return false;
}

// FERMER LA FENÊTRE EMPORTE LE SHELL. Mesuré : la fermeture du maître
// suffit dans ce cas -- le noyau envoie SIGHUP au groupe au premier plan du
// terminal -- et le cas est là pour que ça reste vrai.
TEST(pty_destructor_takes_an_ordinary_shell_with_it) {
  pid_t shell = 0;
  {
    Pty p;
    REQUIRE_EQ(p.spawn(shell_running("printf 'pret\\n'; sleep 30")),
               std::string());
    shell = p.pid();
    REQUIRE(read_until(p, "pret").find("pret") != std::string::npos);
  }
  CHECK(gone_or_dead(shell));
}

// ET IL EMPORTE AUSSI CELUI QUI A REFUSÉ LE RACCROCHAGE. Un `trap '' HUP`
// ignore le SIGHUP du noyau comme le nôtre : mesuré, un tel shell survivait
// à la fermeture de sa fenêtre et tournait pour TOUTE la vie du démon --
// sans pseudo-terminal, sans fenêtre, sans aucun moyen de le revoir. La
// session survit à la déconnexion par construction : cette vie-là se compte
// en semaines.
TEST(pty_destructor_takes_a_shell_that_refused_the_hangup) {
  pid_t shell = 0;
  {
    Pty p;
    REQUIRE_EQ(
        p.spawn(shell_running("trap '' HUP; printf 'pret\\n'; sleep 30")),
        std::string());
    shell = p.pid();
    REQUIRE(read_until(p, "pret").find("pret") != std::string::npos);
  }
  CHECK(gone_or_dead(shell));
}

// MAIS PAS CELUI QUI A QUITTÉ LA SESSION. `setsid` -- ce que font `nohup` et
// `disown` -- sort l'enfant du groupe, et rien de ce qu'on envoie au groupe
// ne l'atteint plus. C'est la porte de sortie, et elle est VOULUE : sans
// elle, il n'y aurait aucun moyen de faire survivre un travail à sa fenêtre.
TEST(pty_destructor_leaves_a_child_that_left_the_session) {
  pid_t escaped = 0;
  {
    Pty p;
    REQUIRE_EQ(p.spawn(shell_running(
                   "setsid sleep 30 & printf '%d\\n' \"$!\"; sleep 30")),
               std::string());
    const std::string out = read_until(p, "\n", 2000);
    escaped = static_cast<pid_t>(std::atoi(out.c_str()));
    REQUIRE(escaped > 0);
  }
  nap_ms(300);
  const char st = proc_state(escaped);
  ::kill(escaped, SIGKILL);  // on ne laisse rien tourner derriere un cas
  CHECK(st != '.');
}

// LE MAÎTRE EST REFERMÉ. SIGKILL emporte le shell, donc aucun cas de
// comportement ne verrait la différence -- mais le descripteur, lui, reste
// ouvert. Une session qui ouvre et ferme des fenêtres pendant des semaines
// finirait à court de descripteurs, et le démon ne pourrait plus accepter
// un seul client.
TEST(pty_destructor_gives_the_master_descriptor_back) {
  const auto open_fds = []() {
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n;
  };

  // Un premier tour à part : il paie les allocations et les tampons que la
  // bibliothèque garde, et que le compte suivant prendrait pour une fuite.
  {
    Pty warm;
    REQUIRE_EQ(warm.spawn(shell_running("sleep 30")), std::string());
  }
  const int before = open_fds();
  REQUIRE(before > 0);

  for (int i = 0; i < 8; ++i) {
    Pty p;
    REQUIRE_EQ(p.spawn(shell_running("sleep 30")), std::string());
  }

  CHECK_EQ(open_fds(), before);
}

// LE SIGKILL PART AU GROUPE, pas au seul enfant. Un `trap '' HUP` se
// transmet à ses enfants -- SIG_IGN survit au fork comme à l'exec -- donc
// une tâche de fond lancée sous ce shell ignore le raccrochage tout autant.
// Ne tuer que le shell la laisserait tourner, orpheline, sans terminal.
TEST(pty_destructor_takes_the_whole_group_of_a_shell_that_refused_the_hangup) {
  pid_t background = 0;
  {
    Pty p;
    REQUIRE_EQ(p.spawn(shell_running("trap '' HUP; sleep 30 & "
                                     "printf '%d\\n' \"$!\"; sleep 30")),
               std::string());
    const std::string out = read_until(p, "\n", 2000);
    background = static_cast<pid_t>(std::atoi(out.c_str()));
    REQUIRE(background > 0);
  }
  const bool dead = gone_or_dead(background, 2000);
  if (!dead) ::kill(background, SIGKILL);  // rien ne traîne derrière un echec
  CHECK(dead);
}
