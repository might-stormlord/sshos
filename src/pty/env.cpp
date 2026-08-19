#include "pty/env.hpp"

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <vector>

extern char** environ;

namespace sshos {
namespace {

// Fermée par sécurité autant que par correction : voir env.hpp.
constexpr std::string_view kSessionVars[] = {
    "SSH_AUTH_SOCK", "SSH_CONNECTION", "SSH_CLIENT",
    "SSH_TTY",       "DISPLAY",        "XDG_SESSION_ID",
};

// Ce que nous imposons, quoi que disent la base et le client.
constexpr std::pair<std::string_view, std::string_view> kForced[] = {
    // TERM décrit NOTRE émulateur, jamais celui du client : entre l'invité
    // et le terminal du client il y a notre parseur, et c'est lui que
    // l'invité a en face de lui.
    {"TERM", "xterm-256color"},
    {"COLORTERM", "truecolor"},
    // Pour que les scripts sachent où ils tournent.
    {"SSHOS", "1"},
};

// La taille faisant autorité est celle du PTY, posée par TIOCSWINSZ ; le
// noyau envoie SIGWINCH tout seul au groupe au premier plan. Un LINES ou
// COLUMNS hérité serait une SECONDE vérité, figée à la taille qu'avait la
// fenêtre au lancement de l'enfant.
// SSHOS_BOOT_ID et SSHOS_EXE sont l'identité du bureau lui-même : le nom de
// son socket et le chemin de son binaire. Un enfant qui en hérite peut
// s'attacher au bureau qui l'a lancé, ou le tuer -- et c'est précisément ce
// qui arrive quand on travaille sur le projet depuis un terminal du bureau
// installé.
constexpr std::string_view kBanned[] = {"LINES", "COLUMNS", "SSHOS_BOOT_ID",
                                        "SSHOS_EXE"};

bool in(const auto& table, std::string_view key) {
  return std::find(std::begin(table), std::end(table), key) != std::end(table);
}

// L'ENTREE DE L'UTILISATEUR DANS LA BASE DE COMPTES, lue une bonne fois.
// getpwuid_r plutôt que getpwuid : le démon est mono-thread, mais rendre un
// pointeur vers un tampon statique qu'un appel suivant écrasera est
// exactement le genre de piège qu'on ne veut pas laisser derrière soi.
//
// `retenu` porte la mémoire des chaînes : `pw` ne fait que pointer dedans.
bool account_entry(passwd& pw, std::vector<char>& retenu) {
  const uid_t uid = ::getuid();
  retenu.assign(4096, '\0');
  passwd* found = nullptr;
  for (int tries = 0; tries < 4; ++tries) {
    const int rc = ::getpwuid_r(uid, &pw, retenu.data(), retenu.size(), &found);
    if (rc == 0) return found != nullptr;
    if (rc != ERANGE) return false;
    retenu.resize(retenu.size() * 2);
  }
  return false;
}

}  // namespace

bool is_session_variable(std::string_view key) { return in(kSessionVars, key); }

std::string login_shell() {
  passwd pw{};
  std::vector<char> retenu;
  if (!account_entry(pw, retenu)) return "/bin/sh";
  if (pw.pw_shell == nullptr || pw.pw_shell[0] != '/') return "/bin/sh";
  return pw.pw_shell;
}

std::string home_dir() {
  passwd pw{};
  std::vector<char> retenu;
  if (!account_entry(pw, retenu)) return "/";
  // UN CHEMIN ABSOLU OU RIEN. Un « home » relatif -- ça existe dans des
  // bases de comptes malmenées -- ferait démarrer le shell à un endroit qui
  // dépend du répertoire courant du démon, c'est-à-dire nulle part de sûr.
  if (pw.pw_dir == nullptr || pw.pw_dir[0] != '/') return "/";
  return pw.pw_dir;
}

std::vector<std::string> child_env(const std::vector<std::string>& base,
                                   const EnvDelta& delta) {
  // Une table ordonnée, et pas une table de hachage : le résultat doit être
  // le même d'un appel à l'autre, sans quoi les tests de l'enfant seraient
  // à la merci d'un ordre de parcours.
  std::map<std::string, std::string> env;

  for (const std::string& entry : base) {
    const size_t eq = entry.find('=');
    // « = » sans clé et une entrée sans « = » existent dans la nature ; on
    // hérite de ce qu'on nous a donné, on ne le valide pas.
    if (eq == std::string::npos || eq == 0) continue;
    const std::string key = entry.substr(0, eq);
    // Le fossile disparaît même si le client n'en offre pas de version
    // fraîche : un SSH_AUTH_SOCK mort est PIRE que pas d'agent du tout,
    // parce qu'il fait échouer git après un délai d'attente au lieu de
    // demander la passphrase tout de suite.
    if (is_session_variable(key)) continue;
    if (in(kBanned, key)) continue;
    env[key] = entry.substr(eq + 1);
  }

  for (const auto& [key, value] : delta) {
    if (!is_session_variable(key)) continue;  // liste fermée : voir env.hpp
    env[key] = value;
  }

  for (const auto& [key, value] : kForced) {
    env[std::string(key)] = std::string(value);
  }

  std::vector<std::string> out;
  out.reserve(env.size());
  for (const auto& [key, value] : env) out.push_back(key + "=" + value);
  return out;
}

std::vector<std::string> daemon_env() {
  std::vector<std::string> out;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) out.emplace_back(*e);
  return out;
}

}  // namespace sshos
