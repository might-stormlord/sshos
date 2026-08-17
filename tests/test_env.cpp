#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "harness.hpp"
#include "pty/env.hpp"

using sshos::child_env;
using sshos::EnvDelta;
using sshos::login_shell;

namespace {

// Cherche « CLE=… » et rend la valeur, ou une sentinelle si la clé est
// absente. L'absence et la chaîne vide sont deux choses différentes ici :
// un SSH_AUTH_SOCK vide serait un agent cassé, pas un agent absent.
std::string value_of(const std::vector<std::string>& env, const std::string& key) {
  const std::string prefix = key + "=";
  for (const std::string& e : env) {
    if (e.rfind(prefix, 0) == 0) return e.substr(prefix.size());
  }
  return "<absente>";
}

}  // namespace

// L'environnement du démon est un fossile de la PREMIÈRE session SSH. Suivre
// son $SHELL revient à obéir à ce que le tout premier client avait ce
// jour-là ; getpwuid dit ce que l'utilisateur a vraiment choisi.
TEST(login_shell_ignores_the_shell_variable_of_the_daemon) {
  const char* before = ::getenv("SHELL");
  const std::string saved = before ? before : "";
  ::setenv("SHELL", "/nexiste/pas/du/tout", 1);

  const std::string sh = login_shell();

  if (saved.empty()) {
    ::unsetenv("SHELL");
  } else {
    ::setenv("SHELL", saved.c_str(), 1);
  }

  CHECK(sh != "/nexiste/pas/du/tout");
  CHECK(!sh.empty());
  CHECK(sh[0] == '/');
}

// Le cœur du problème : SSH_AUTH_SOCK pointe vers un agent mort depuis la
// première déconnexion, donc « git push » réclamerait une passphrase dans
// toutes les fenêtres, pour toute la durée de vie du démon.
TEST(child_env_lets_the_handshake_delta_win_over_the_fossil) {
  const std::vector<std::string> base = {"PATH=/usr/bin",
                                         "SSH_AUTH_SOCK=/tmp/agent-mort",
                                         "SSH_CONNECTION=1.2.3.4 22 5.6.7.8 22"};
  const EnvDelta delta = {{"SSH_AUTH_SOCK", "/tmp/agent-vivant"},
                          {"SSH_CONNECTION", "9.9.9.9 22 8.8.8.8 22"}};

  const std::vector<std::string> env = child_env(base, delta);
  CHECK_EQ(value_of(env, "SSH_AUTH_SOCK"), std::string("/tmp/agent-vivant"));
  CHECK_EQ(value_of(env, "SSH_CONNECTION"), std::string("9.9.9.9 22 8.8.8.8 22"));
  CHECK_EQ(value_of(env, "PATH"), std::string("/usr/bin"));  // le reste survit
}

// Un client qui n'offre pas d'agent doit faire DISPARAÎTRE le fossile, pas
// le laisser en place. Un agent mort est pire que pas d'agent du tout :
// l'un fait échouer git après un délai d'attente, l'autre demande la
// passphrase tout de suite.
TEST(child_env_drops_a_fossil_the_client_did_not_renew) {
  const std::vector<std::string> base = {"PATH=/usr/bin",
                                         "SSH_AUTH_SOCK=/tmp/agent-mort",
                                         "DISPLAY=:0"};
  const std::vector<std::string> env = child_env(base, EnvDelta{});
  CHECK_EQ(value_of(env, "SSH_AUTH_SOCK"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "DISPLAY"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "PATH"), std::string("/usr/bin"));
}

// TERM décrit NOTRE émulateur, jamais celui du client. Un client en
// « vt100 » ne doit pas faire croire au shell qu'il n'a que seize couleurs :
// entre les deux il y a notre parseur, qui en gère seize millions.
TEST(child_env_fixes_the_terminal_it_actually_provides) {
  const std::vector<std::string> base = {"TERM=vt100", "COLORTERM=", "PATH=/bin"};
  const std::vector<std::string> env = child_env(base, EnvDelta{});
  CHECK_EQ(value_of(env, "TERM"), std::string("xterm-256color"));
  CHECK_EQ(value_of(env, "COLORTERM"), std::string("truecolor"));
  CHECK_EQ(value_of(env, "SSHOS"), std::string("1"));
}

// Même un client qui envoie TERM dans son delta ne l'impose pas : la
// variable décrit ce que l'invité a en face de lui, et en face il a nous.
TEST(child_env_refuses_a_terminal_named_by_the_client) {
  const EnvDelta delta = {{"TERM", "dumb"}, {"COLORTERM", "non"}};
  const std::vector<std::string> env = child_env({}, delta);
  CHECK_EQ(value_of(env, "TERM"), std::string("xterm-256color"));
  CHECK_EQ(value_of(env, "COLORTERM"), std::string("truecolor"));
}

// La taille faisant autorité est celle du PTY, posée par TIOCSWINSZ ; le
// noyau envoie SIGWINCH tout seul. Un LINES/COLUMNS hérité serait une
// SECONDE vérité, figée à la taille qu'avait la fenêtre au lancement.
TEST(child_env_drops_lines_and_columns) {
  const std::vector<std::string> base = {"LINES=24", "COLUMNS=80", "HOME=/root"};
  const std::vector<std::string> env = child_env(base, EnvDelta{});
  CHECK_EQ(value_of(env, "LINES"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "COLUMNS"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "HOME"), std::string("/root"));
}

// Une entrée du delta qui ne fait pas partie des variables de session est
// ignorée : le handshake ne doit pas être un canal pour injecter LD_PRELOAD
// ou PATH dans tous les processus du démon.
TEST(child_env_only_honours_the_session_variables) {
  const EnvDelta delta = {{"LD_PRELOAD", "/tmp/mechant.so"},
                          {"PATH", "/tmp/mechant/bin"},
                          {"SSH_TTY", "/dev/pts/9"}};
  const std::vector<std::string> env = child_env({"PATH=/usr/bin"}, delta);
  CHECK_EQ(value_of(env, "LD_PRELOAD"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "PATH"), std::string("/usr/bin"));
  CHECK_EQ(value_of(env, "SSH_TTY"), std::string("/dev/pts/9"));
}

// Une base malformée ne fait pas tomber la construction : le démon hérite
// de ce qu'on lui a donné, et « = » sans clé existe dans la nature.
TEST(child_env_survives_a_malformed_base) {
  const std::vector<std::string> base = {"=vide", "SANSEGAL", "OK=1", ""};
  const std::vector<std::string> env = child_env(base, EnvDelta{});
  CHECK_EQ(value_of(env, "OK"), std::string("1"));
  for (const std::string& e : env) {
    CHECK(!e.empty());
    CHECK(e.find('=') != std::string::npos);
    CHECK(e[0] != '=');
  }
}

// Deux appels identiques rendent exactement la même chose, dans le même
// ordre : sans ça les tests de la tâche suivante seraient à la merci de
// l'ordre de parcours d'une table de hachage.
TEST(child_env_is_deterministic) {
  const std::vector<std::string> base = {"B=2", "A=1", "C=3"};
  const EnvDelta delta = {{"SSH_TTY", "/dev/pts/1"}};
  CHECK(child_env(base, delta) == child_env(base, delta));
  const std::vector<std::string> env = child_env(base, delta);
  CHECK(std::is_sorted(env.begin(), env.end()));
}

// L'IDENTITÉ DU BUREAU NE DESCEND PAS DANS SES PROPRES SHELLS. Un shell
// ouvert dans le bureau installé qui hérite de SSHOS_BOOT_ID calcule le
// même nom de socket que lui : « sshos --kill » tapé là tue la session de
// travail, et « sshos » s'y rattache au lieu d'ouvrir une instance neuve.
// C'est exactement le piège que l'installation isolée existe pour fermer.
//
// SSHOS_EXE part pour la même raison : c'est le chemin de relance du
// bureau, pas une information dont un invité a besoin.
TEST(child_env_never_leaks_the_desktop_identity_to_a_shell) {
  const std::vector<std::string> base = {
      "PATH=/usr/bin",
      "SSHOS_BOOT_ID=local",
      "SSHOS_EXE=/home/u/.local/libexec/sshos",
  };

  const std::vector<std::string> env = child_env(base, {});

  CHECK_EQ(value_of(env, "SSHOS_BOOT_ID"), std::string("<absente>"));
  CHECK_EQ(value_of(env, "SSHOS_EXE"), std::string("<absente>"));

  // On bannit deux variables, pas l'environnement : le reste passe.
  CHECK_EQ(value_of(env, "PATH"), std::string("/usr/bin"));
}
