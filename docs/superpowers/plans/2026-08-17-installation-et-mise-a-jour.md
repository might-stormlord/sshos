# Installation locale et mise à jour depuis le bureau — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Installer `ssh_os` dans le home de l'utilisateur, isolé de l'arbre de développement, et lui donner une mise à jour vérifiée une fois par jour, déclenchée par une entrée de menu et une pastille cliquable.

**Architecture:** Le C++ ne fait que **lire** un fichier d'état clé=valeur et **superviser un enfant** ; `git`, `cmake`, le réseau et la compilation vivent dans `tools/update.sh`, lancé par un `fork()`/`execv()` simple dont le pid est inscrit auprès du récolteur unique du démon. L'isolation entre instances passe par `SSHOS_BOOT_ID`, qui compose le nom du socket abstrait, et par un lanceur shell qui pose aussi `SSHOS_EXE` — le chemin de relance, car `/proc/self/exe` désigne une inode et relancerait l'ancien binaire après une mise à jour.

**Tech Stack:** C++20 (bibliothèque standard + POSIX/Linux uniquement), CMake ≥ 3.20, harnais de test maison (`tests/harness.hpp`), `sh` POSIX pour l'outillage, GitHub Actions pour la publication.

**Spec:** `docs/superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md`

## Global Constraints

Ces règles valent pour **toutes** les tâches. Elles viennent du §4 de `docs/REPRISE.md` et du §2 de la spec.

- **`CMakeLists.txt` ne doit pas être modifié.** Aucune cible `install()`, aucun `configure_file`, aucun `-D`. Les nouveaux fichiers sont pris par les globs `CONFIGURE_DEPENDS`.
- **Un fichier de test doit être à plat dans `tests/`**, nommé `test_*.cpp`. `CMakeLists.txt:26` utilise `file(GLOB …)` **non récursif** : `tests/shell/test_x.cpp` serait silencieusement ignoré, la suite resterait verte, et le test n'existerait pas.
- **Zéro dépendance externe.** Ni gtest, ni ncurses, ni fmt, ni bibliothèque JSON. Bibliothèque standard et API POSIX/Linux uniquement.
- **Un thread, un `epoll`, aucun mutex.** Le démon ne bloque jamais : ni `git`, ni `cmake`, ni lecture réseau dans son fil.
- **Les commentaires sont en français, avec les accents.** Le code et les identifiants restent en anglais.
- **`\033`, jamais `\e`.** `\e` est une extension GCC ; avec `-Wpedantic -Werror`, une seule occurrence casse la compilation du projet entier.
- **Les libellés de menu et de barre sont sans accents**, comme `Ranger les fenetres` (`menu.cpp:46`) et `Fermer la session` (`menu.cpp:52`).
- **Les messages de commit sont en français sans accents.**
- **Aucun code de compatibilité** avec des systèmes plus anciens : pas de `#ifdef` pour de vieilles glibc ou de vieux noyaux.
- **Aucun compte de tests n'est écrit nulle part** dans le code, les scripts ou le workflow. Le critère est « zéro échec ». Le total (`1146` au 17 août 2026) périme à chaque tâche de ce plan.
- **Commandes de vérification :**
  - `cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests`
  - filtre : `./build-release/sshos_tests <sous-chaine>`
  - attendu : `0 en echec, 0 assertions echouees`, zéro avertissement.
- **Jamais de commande détachée** (`&`, `nohup`, `disown`) dans le travail : deux agents s'y sont bloqués définitivement sur ce projet.

---

# Structure des fichiers

| Fichier | Responsabilité | Tâche |
|---|---|---|
| `src/pty/env.cpp` *(modif)* | Bannir l'identité du bureau de l'environnement des shells enfants | 1 |
| `tests/test_golden.cpp` *(modif)* | Lire les références depuis `SSHOS_GOLDEN_DIR` avant `__FILE__` | 2 |
| `src/daemon/daemonize.{hpp,cpp}` *(modif)* | `daemon_exe_path()` — le chemin de relance, testable | 3 |
| `src/main.cpp` *(modif)* | Utiliser `daemon_exe_path()` au lieu de `/proc/self/exe` | 3 |
| `tools/install.sh` *(créer)* | Installation interactive, lanceur, échelle d'acquisition | 4 |
| `src/shell/update_state.{hpp,cpp}` *(créer)* | **Analyseur pur** du fichier d'état : schéma, plafonds, bornage | 5 |
| `src/shell/update_service.{hpp,cpp}` *(créer)* | Machine à sept états, horloge, lanceur injecté | 6 |
| `src/shell/menu.{hpp,cpp}` *(modif)* | `MenuItem::enabled`, `Menu::set_extra_items()` | 7 |
| `src/shell/panel.{hpp,cpp}` *(modif)* | `PanelHit::Update`, `Panel::set_update_badge()` | 8 |
| `src/daemon/session.{hpp,cpp}` *(modif)* | Câblage : réveil, routage `update:*`, lanceur réel, récolte | 9 |
| `src/daemon/daemon.cpp` *(modif)* | Délai sans garde `if (client)`, `wants_quit()` en fin de boucle | 10 |
| `src/common/proto.hpp` *(modif)*, `src/client/client.cpp` *(modif)* | `kDetachReasonUpdate`, boucle de reconnexion | 11 |
| `tools/update.sh` *(créer)* | `--check`, `--apply`, `--rollback` : tout ce qui touche git et le réseau | 12 |
| `tools/sonde_update.py` *(créer)* | Sonde bout-en-bout sur un faux dépôt, `HOME` temporaire | 13 |
| `.github/workflows/release.yml` *(créer)* | Publication du binaire, des tests et des références | 14 |

**Phase 1 = tâches 1 à 4.** Elle livre un logiciel utilisable seul : une instance installée, isolée de l'arbre de développement, sans mécanisme de mise à jour. **Phase 2 = tâches 5 à 14.**

---

# PHASE 1 — l'installation isolée

## Task 1: Bannir l'identité du bureau de l'environnement des enfants

**Contexte.** `daemon_env()` recopie tout `environ` ; `child_env()` n'en retire que `kSessionVars` et `kBanned`. `SSHOS_BOOT_ID` n'y est pas, donc chaque shell du bureau installé en hérite — et `./build-release/sshos --kill` tapé dans ce shell **tue le bureau installé**. C'est le §3.2 de la spec.

**Files:**
- Modify: `src/pty/env.cpp:36`
- Test: `tests/test_env.cpp`

**Interfaces:**
- Consumes: `sshos::child_env(const std::vector<std::string>& base, const EnvDelta& delta)` — existante, `src/pty/env.hpp:31`.
- Produces: rien de neuf. Change un comportement.

- [ ] **Step 1: Écrire le test qui échoue**

Ajouter à la fin de `tests/test_env.cpp` :

```cpp
// L'IDENTITÉ DU BUREAU NE DESCEND PAS DANS SES PROPRES SHELLS. Un shell
// ouvert dans le bureau installé qui hérite de SSHOS_BOOT_ID calcule le
// même nom de socket que lui : `sshos --kill` tapé là tue la session de
// travail. SSHOS_EXE part pour la même raison -- c'est le chemin de
// relance du bureau, pas une information dont un invité a besoin.
TEST(child_env_never_leaks_the_desktop_identity_to_a_shell) {
  const std::vector<std::string> base = {
      "PATH=/usr/bin",
      "SSHOS_BOOT_ID=local",
      "SSHOS_EXE=/home/u/.local/libexec/sshos",
  };

  const std::vector<std::string> env = child_env(base, {});

  for (const std::string& e : env) {
    CHECK(e.rfind("SSHOS_BOOT_ID=", 0) != 0);
    CHECK(e.rfind("SSHOS_EXE=", 0) != 0);
  }

  // On bannit deux variables, pas l'environnement : le reste passe.
  bool has_path = false;
  for (const std::string& e : env) {
    if (e == "PATH=/usr/bin") has_path = true;
  }
  CHECK(has_path);
}
```

- [ ] **Step 2: Lancer le test et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" && \
  ./build-release/sshos_tests child_env_never_leaks
```

Attendu : **échec**, sur l'une des deux `CHECK` de la boucle — les deux variables sont aujourd'hui transmises telles quelles.

- [ ] **Step 3: Implémenter**

Dans `src/pty/env.cpp`, remplacer la ligne 36 :

```cpp
// Ce qui ne descend JAMAIS dans un enfant. LINES/COLUMNS parce que le
// shell les recalcule et qu'une valeur fossile ment sur la taille réelle.
// SSHOS_BOOT_ID et SSHOS_EXE parce qu'ils sont l'identité du bureau
// lui-même : un enfant qui en hérite peut s'attacher au bureau qui l'a
// lancé, ou le tuer. C'est exactement le piège que l'installation isolée
// existe pour fermer.
constexpr std::string_view kBanned[] = {"LINES", "COLUMNS", "SSHOS_BOOT_ID",
                                        "SSHOS_EXE"};
```

- [ ] **Step 4: Lancer le test et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
```

Attendu : `0 en echec, 0 assertions echouees` sur la **suite entière** — pas seulement sur le filtre. Une variable bannie peut casser un test de terminal.

- [ ] **Step 5: Commiter**

```bash
git add src/pty/env.cpp tests/test_env.cpp
git commit -m "fix(env): l'identite du bureau ne descend plus dans ses shells

Un shell ouvert dans le bureau installe heritait de SSHOS_BOOT_ID, donc
calculait le meme nom de socket que le bureau qui l'avait lance. sshos
--kill tape dans ce shell tuait la session de travail -- precisement le
piege que l'installation isolee existe pour fermer.

SSHOS_EXE part avec, pour la meme raison : c'est le chemin de relance du
bureau, pas une information dont un invite a besoin.

Consequence assumee : sshos tape dans une fenetre du bureau ne se
rattache plus a ce bureau, il vise le nom par defaut."
```

---

## Task 2: Rendre les références des tests relogeables

**Contexte.** `tests/test_golden.cpp:42-46` déduit le répertoire des références de `__FILE__`, gravé à la compilation. Un `sshos_tests` téléchargé depuis une release chercherait donc un chemin du conteneur de CI, et les 9 cas `golden_*` échoueraient par construction — l'échelon 2 raterait systématiquement son propre garde-fou (spec §5.2).

**Files:**
- Modify: `tests/test_golden.cpp:38-46`
- Test: `tests/test_golden.cpp` (même fichier)

**Interfaces:**
- Produces: la variable d'environnement **`SSHOS_GOLDEN_DIR`**, consommée par les tâches 4 et 14.

- [ ] **Step 1: Écrire le test qui échoue**

Ajouter dans l'espace anonyme de `tests/test_golden.cpp`, après `golden_dir()`, une copie locale du garde d'environnement — il en existe déjà deux dans le projet (`tests/test_net.cpp:80`, `tests/test_tty.cpp:35`), une troisième reste cohérente :

```cpp
class EnvVarGuard {
 public:
  explicit EnvVarGuard(const char* name) : name_(name) {
    if (const char* v = std::getenv(name)) {
      had_value_ = true;
      value_ = v;
    }
  }
  ~EnvVarGuard() {
    if (had_value_) {
      ::setenv(name_, value_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  EnvVarGuard(const EnvVarGuard&) = delete;
  EnvVarGuard& operator=(const EnvVarGuard&) = delete;

 private:
  const char* name_;
  bool had_value_ = false;
  std::string value_;
};
```

Puis le cas, hors espace anonyme :

```cpp
// LES RÉFÉRENCES DOIVENT POUVOIR DÉMÉNAGER. __FILE__ est gravé à la
// compilation : un binaire de test publié dans une release chercherait le
// répertoire de la machine qui l'a compilé, qui n'existe pas chez celui
// qui le télécharge. La variable est donc consultée d'abord, et le chemin
// compilé n'est plus qu'un repli pour le développement.
TEST(golden_dir_prefers_the_environment_over_the_compiled_path) {
  EnvVarGuard guard("SSHOS_GOLDEN_DIR");

  ::setenv("SSHOS_GOLDEN_DIR", "/tmp/refs", 1);
  CHECK_EQ(golden_dir(), std::string("/tmp/refs/"));

  // La barre oblique finale est ajoutée si elle manque, jamais doublée.
  ::setenv("SSHOS_GOLDEN_DIR", "/tmp/refs/", 1);
  CHECK_EQ(golden_dir(), std::string("/tmp/refs/"));

  // Vide = absente : on ne cherche pas les références à la racine.
  ::setenv("SSHOS_GOLDEN_DIR", "", 1);
  CHECK(golden_dir().find("tests/golden/") != std::string::npos);

  ::unsetenv("SSHOS_GOLDEN_DIR");
  CHECK(golden_dir().find("tests/golden/") != std::string::npos);
}
```

Ajouter `#include <cstdlib>` en tête du fichier s'il n'y est pas.

- [ ] **Step 2: Lancer le test et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" && \
  ./build-release/sshos_tests golden_dir_prefers
```

Attendu : **échec** au premier `CHECK_EQ` — `golden_dir()` ignore aujourd'hui l'environnement.

- [ ] **Step 3: Implémenter**

Remplacer `golden_dir()` dans `tests/test_golden.cpp` :

```cpp
// SSHOS_GOLDEN_DIR d'abord, __FILE__ ensuite. Le chemin compilé est absolu
// (CMake passe des chemins absolus) et convient parfaitement au
// développement, mais il désigne la machine qui a COMPILÉ le binaire. Un
// sshos_tests publié dans une release et exécuté ailleurs a besoin qu'on
// lui dise où sont les références.
std::string golden_dir() {
  if (const char* d = std::getenv("SSHOS_GOLDEN_DIR")) {
    std::string p = d;
    if (!p.empty()) {
      if (p.back() != '/') p.push_back('/');
      return p;
    }
  }
  std::string p = __FILE__;
  p.resize(p.rfind('/') + 1);
  return p + "golden/";
}
```

- [ ] **Step 4: Lancer les tests et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
```

Attendu : suite entière au vert. Vérifier ensuite que le déménagement marche vraiment :

```bash
cp -r tests/golden /tmp/golden-copie
SSHOS_GOLDEN_DIR=/tmp/golden-copie ./build-release/sshos_tests golden_
rm -rf /tmp/golden-copie
```

Attendu : les cas `golden_*` passent avec les références déplacées.

- [ ] **Step 5: Commiter**

```bash
git add tests/test_golden.cpp
git commit -m "test(golden): les references peuvent demenager

__FILE__ est grave a la compilation : un sshos_tests publie dans une
release chercherait le repertoire de la machine qui l'a compile. Les neuf
cas golden echouaient donc par construction chez celui qui le telecharge,
et l'echelon binaire de l'installeur ratait systematiquement son propre
garde-fou.

SSHOS_GOLDEN_DIR est consultee d'abord ; le chemin compile reste le repli
du developpement."
```

---

## Task 3: Le chemin de relance du démon, testable

**Contexte.** `main.cpp:40` relance par `spawn_detached({"/proc/self/exe", "--daemon"})`. `/proc/self/exe` désigne l'**inode** exécutée, pas un chemin : après une mise à jour, celle du client est l'ancienne version, et le redémarrage relancerait l'ancien binaire en silence (spec §7.4a). Il faut résoudre un chemin — et le faire **hors de `main.cpp`**, que `CMakeLists.txt:18` retire de `sshos_core` et qui est donc intestable.

**Files:**
- Modify: `src/daemon/daemonize.hpp`, `src/daemon/daemonize.cpp`
- Modify: `src/main.cpp:20-58`
- Test: `tests/test_daemonize.cpp`

**Interfaces:**
- Produces: `std::string sshos::daemon_exe_path();` — consommée par la tâche 4 (le lanceur pose `SSHOS_EXE`) et la tâche 11 (le redémarrage).

- [ ] **Step 1: Écrire le test qui échoue**

Ajouter à `tests/test_daemonize.cpp` (avec une copie locale d'`EnvVarGuard`, comme au Task 2, si le fichier n'en a pas déjà une) :

```cpp
// LE CHEMIN L'EMPORTE SUR L'INODE. /proc/self/exe designe l'inode en cours
// d'exécution : après une mise à jour, celle d'un client déjà lancé est
// l'ANCIENNE version, et relancer par là ferait repartir le démon sur le
// binaire qu'on vient de remplacer -- sans que rien ne le signale. Le
// lanceur installé pose donc SSHOS_EXE, qui fait autorité.
TEST(daemon_exe_path_prefers_the_installed_path_over_the_running_inode) {
  EnvVarGuard guard("SSHOS_EXE");

  ::setenv("SSHOS_EXE", "/home/u/.local/libexec/sshos", 1);
  CHECK_EQ(sshos::daemon_exe_path(), std::string("/home/u/.local/libexec/sshos"));

  // Vide = absente. Un lanceur mal écrit ne doit pas rendre le démon
  // inlançable ; on retombe sur le comportement de l'arbre de dev.
  ::setenv("SSHOS_EXE", "", 1);
  CHECK_EQ(sshos::daemon_exe_path(), std::string("/proc/self/exe"));

  ::unsetenv("SSHOS_EXE");
  CHECK_EQ(sshos::daemon_exe_path(), std::string("/proc/self/exe"));
}
```

- [ ] **Step 2: Lancer le test et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

Attendu : **échec de compilation**, `daemon_exe_path` non déclarée.

- [ ] **Step 3: Implémenter**

Dans `src/daemon/daemonize.hpp`, avant la fermeture du `namespace` :

```cpp
// Le chemin du binaire à relancer pour obtenir un démon.
//
// `/proc/self/exe` désigne l'INODE en cours d'exécution, pas un chemin :
// elle reste vivante même après que le fichier a été remplacé ou délié.
// C'est exactement ce qu'il faut dans l'arbre de développement, et
// exactement ce qu'il ne faut pas après une mise à jour -- un client lancé
// avant celle-ci relancerait l'ancienne version, en silence, et le
// contrôle de compatibilité du protocole ne se déclencherait même pas
// puisque les deux binaires seraient les mêmes.
//
// Le lanceur installé pose `SSHOS_EXE` ; quand elle est définie et non
// vide, elle fait autorité. Sinon on retombe sur l'inode, ce qui laisse le
// développement inchangé.
std::string daemon_exe_path();
```

Ajouter `#include <string>` en tête si absent. Dans `src/daemon/daemonize.cpp` :

```cpp
std::string daemon_exe_path() {
  if (const char* p = std::getenv("SSHOS_EXE")) {
    if (*p != '\0') return p;
  }
  return "/proc/self/exe";
}
```

Ajouter `#include <cstdlib>` et `#include <string>` si absents. Puis, dans `src/main.cpp`, remplacer la ligne 40 :

```cpp
  const pid_t mid = sshos::spawn_detached({sshos::daemon_exe_path(), "--daemon"});
```

- [ ] **Step 4: Lancer les tests et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
```

Attendu : suite entière au vert. Puis vérifier que le démon démarre toujours réellement :

```bash
./build-release/sshos --kill 2>/dev/null; ./build-release/sshos --daemon && \
  ./build-release/sshos --status && ./build-release/sshos --kill
```

Attendu : `demon actif (pid N)` puis `demon N arrete`.

- [ ] **Step 5: Commiter**

```bash
git add src/daemon/daemonize.hpp src/daemon/daemonize.cpp src/main.cpp tests/test_daemonize.cpp
git commit -m "feat(daemonize): relancer le demon par chemin, pas par inode

/proc/self/exe designe l'inode en cours d'execution. Apres une mise a
jour, celle d'un client deja lance est l'ancienne version : relancer par
la ferait repartir le demon sur le binaire qu'on vient de remplacer, sans
que rien ne le signale -- et le controle de compatibilite du protocole ne
se declencherait meme pas, les deux binaires etant les memes.

SSHOS_EXE, posee par le lanceur installe, fait desormais autorite. Le
repli sur l'inode laisse l'arbre de developpement inchange.

La fonction vit dans daemonize plutot que dans main.cpp, que CMakeLists
retire de sshos_core et qui est donc intestable."
```

---

## Task 4: `tools/install.sh`

**Contexte.** Spec §5 (échelle d'acquisition), §9 (l'installeur). C'est la tâche qui rend la phase 1 utilisable.

**Files:**
- Create: `tools/install.sh` (mode 0755)

**Interfaces:**
- Consumes: `SSHOS_GOLDEN_DIR` (tâche 2), `SSHOS_EXE` (tâche 3).
- Produces: la disposition du §9.2 de la spec, et le fichier d'état initial que la tâche 5 sait lire.

- [ ] **Step 1: Écrire le script**

Créer `tools/install.sh`. Le squelette complet, à respecter dans l'ordre :

```sh
#!/bin/sh
# Installation de ssh_os dans le home de l'utilisateur.
#
# Ce script ne decide rien a la place de l'utilisateur et ne modifie aucun
# fichier de configuration sans un oui explicite.
set -eu

REPO_URL="https://github.com/might-stormlord/sshos.git"
API="https://api.github.com/repos/might-stormlord/sshos"
CURL_OPTS="--proto =https --proto-redir =https --tlsv1.2 -fsSL"

# --- etat des lieux -------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

HAVE_GIT=no;  have git  && HAVE_GIT=yes
HAVE_NET=no;  have curl && HAVE_NET=curl
[ "$HAVE_NET" = no ] && have wget && HAVE_NET=wget
HAVE_TAR=no;  have tar  && HAVE_TAR=yes
HAVE_CC=no;   have c++  && HAVE_CC=yes
HAVE_CMAKE=no; have cmake && HAVE_CMAKE=yes

# --- questions ------------------------------------------------------------
# 1. prefixe (defaut ~/.local)
# 2. PATH : verifier, donner la ligne exacte, NE PAS toucher au profil
# 3. loginctl enable-linger : proposer, expliquer, poser seulement sur oui
# 4. nom d'instance (defaut local)

# --- echelle d'acquisition ------------------------------------------------
# echelon 1 git / 2 release / 3 archive / 4 local, dans cet ordre

# --- pose ----------------------------------------------------------------
# cp exe -> exe.previous ; cp neuf -> exe.new ; chmod 0755 ; mv -f exe.new exe
```

Les points **obligatoires**, chacun tiré d'un défaut identifié en relecture :

1. **Le verrou, dès le début** (spec §6.6) :
   ```sh
   mkdir -p "$SHARE"
   exec 9>"$SHARE/lock"
   flock 9 || { echo "sshos: une autre installation est en cours" >&2; exit 1; }
   ```
2. **Le lanceur** (spec §3.1), écrit dans `$PREFIX/bin/sshos`, mode 0755 :
   ```sh
   #!/bin/sh
   SSHOS_BOOT_ID="${SSHOS_BOOT_ID:-local}"
   SSHOS_EXE="$HOME/.local/libexec/sshos"
   export SSHOS_BOOT_ID SSHOS_EXE
   exec "$SSHOS_EXE" "$@"
   ```
   `$HOME/.local` est remplacé par le préfixe choisi au moment de l'écriture.
3. **La sonde du binaire téléchargé** (spec §5.2) — avec `SSHOS_BOOT_ID`, sans quoi un binaire sain rend 1 :
   ```sh
   out=$(SSHOS_BOOT_ID=probe "$candidate" --probe-unknown-flag 2>&1) || rc=$?
   rc=${rc:-0}
   case "$rc" in
     2)   echo "$out" | grep -q '^usage:' && ok=yes ;;
     1)   ok=yes ;;              # charge, environnement incomplet
     126|127) ok=no ;;           # l'editeur de liens l'a refuse
     *)   ok=no ;;
   esac
   ```
4. **La pose du binaire** (spec §8.2) — `cp` puis `rename`, jamais d'écriture en place, sinon **ETXTBSY** garanti quand un démon tourne :
   ```sh
   [ -f "$EXE" ] && cp "$EXE" "$EXE.previous"
   cp "$built" "$EXE.new"
   chmod 0755 "$EXE.new"
   mv -f "$EXE.new" "$EXE"
   ```
5. **L'échelon 3 résout le sha d'abord** (spec §5.3), jamais `refs/heads/main` :
   ```sh
   sha=$(curl $CURL_OPTS "$API/commits/main" | sed -n 's/.*"sha": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1)
   curl $CURL_OPTS -o "$tmp/src.tar.gz" \
     "https://codeload.github.com/might-stormlord/sshos/tar.gz/$sha"
   ```
6. **L'arbre de compilation est validé avant réemploi** (spec §5.7) :
   ```sh
   if [ -d "$SRC/.git" ] && [ "$(git -C "$SRC" remote get-url origin 2>/dev/null)" = "$REPO_URL" ]; then
     git -C "$SRC" fetch --quiet origin main
   else
     rm -rf "$SRC" && git clone --quiet "$REPO_URL" "$SRC"
   fi
   ```
7. **Un préfixe système désactive la mise à jour** (spec §9.1) : si `$PREFIX` n'est pas sous `$HOME`, écrire `status=updates-disabled` et `message=prefixe systeme`.
8. **L'état initial** (spec §9.2) : `status=up-to-date`, `checked_at=$(date +%s)`, `prefix=$PREFIX`, `source=` l'échelon retenu, `installed_commit=` le sha ou `unknown`.
9. **La suite complète tourne avant la pose**, avec `SSHOS_GOLDEN_DIR` pointé sur les références installées. **Le critère est le code de retour, jamais un compte de cas.**

- [ ] **Step 2: Vérifier la syntaxe et l'analyse statique**

```bash
sh -n tools/install.sh && echo "syntaxe OK"
command -v shellcheck >/dev/null && shellcheck tools/install.sh || echo "shellcheck absent, ignore"
```

Attendu : `syntaxe OK`, et aucune erreur de niveau `error` si `shellcheck` est présent.

- [ ] **Step 3: Installer pour de vrai, dans un HOME jetable**

C'est le seul essai qui prouve quelque chose, et il ne touche pas votre vrai `~/.local` :

```bash
rm -rf /tmp/sshos-essai && mkdir -p /tmp/sshos-essai
HOME=/tmp/sshos-essai sh tools/install.sh
```

Attendu : les quatre questions, puis la disposition du §9.2 sous `/tmp/sshos-essai/.local/`.

- [ ] **Step 4: Vérifier l'isolation, qui est tout l'objet de la phase 1**

```bash
# Le bureau installe tourne sous son propre nom de socket.
HOME=/tmp/sshos-essai /tmp/sshos-essai/.local/bin/sshos --daemon
HOME=/tmp/sshos-essai /tmp/sshos-essai/.local/bin/sshos --status

# Le binaire de dev ne le voit pas, et ne peut pas le tuer.
./build-release/sshos --status
./build-release/sshos --kill

# Il est toujours la.
HOME=/tmp/sshos-essai /tmp/sshos-essai/.local/bin/sshos --status
HOME=/tmp/sshos-essai /tmp/sshos-essai/.local/bin/sshos --kill
```

Attendu, dans l'ordre : `demon actif (pid N)` — `aucun demon` — `aucun demon` — **`demon actif (pid N)`** — `demon N arrete`.

> Le quatrième résultat est **le test de la phase 1 entière**. S'il rend `aucun demon`, l'isolation ne marche pas et il ne faut pas continuer.

- [ ] **Step 5: Nettoyer et commiter**

```bash
rm -rf /tmp/sshos-essai
git add tools/install.sh
git commit -m "feat(outils): installeur local, isole de l'arbre de developpement

Quatre questions, aucune decision prise a la place de l'utilisateur, et
aucun fichier de configuration touche sans un oui explicite.

L'echelle d'acquisition descend git, binaire publie, archive, arbre
local. Le binaire telecharge est EPROUVE avant d'etre pose, avec
SSHOS_BOOT_ID dans son environnement : sans elle, main() sort en 1 avant
meme de lire argv et un binaire sain serait classe casse.

La pose se fait par copie puis rename. Une ecriture en place sur un
binaire en cours d'execution rend ETXTBSY a tous les coups, c'est-a-dire
exactement dans le cas ou l'on met a jour."
```

---

# PHASE 2 — la mise à jour depuis le bureau

## Task 5: L'analyseur du fichier d'état

**Contexte.** Spec §4. Fichier clé=valeur lu par le démon, écrit par les scripts. Analyseur **pur** : ni système de fichiers, ni horloge — c'est ce qui le rend intégralement testable.

**Files:**
- Create: `src/shell/update_state.hpp`, `src/shell/update_state.cpp`
- Test: `tests/test_update_state.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class UpdateStatus { Idle, Checking, Applying, UpToDate, Available,
                            RestartPending, CheckFailed, ApplyFailed,
                            HistoryRewritten, UpdatesDisabled };
  struct UpdateState {
    UpdateStatus status = UpdateStatus::Idle;
    std::string prefix, source, installed_commit, previous_commit, remote_commit;
    std::int64_t checked_at = 0;
    pid_t pid = -1;
    std::string message;
  };
  UpdateState parse_update_state(std::string_view raw, std::int64_t now_epoch);
  inline constexpr std::size_t kMaxStateBytes = 4096;
  inline constexpr std::size_t kMaxMessageBytes = 200;
  ```

- [ ] **Step 1: Écrire les tests qui échouent**

Créer `tests/test_update_state.cpp` :

```cpp
#include "shell/update_state.hpp"

#include <string>

#include "harness.hpp"

using sshos::parse_update_state;
using sshos::UpdateStatus;

namespace {
constexpr std::int64_t kNow = 1755400000;
}  // namespace

TEST(update_state_reads_a_well_formed_file) {
  const std::string raw =
      "schema=1\n"
      "prefix=/home/u/.local\n"
      "source=git\n"
      "installed_commit=aaaa\n"
      "previous_commit=bbbb\n"
      "remote_commit=cccc\n"
      "checked_at=1755300000\n"
      "status=available\n"
      "pid=\n"
      "message=une ligne\n";

  const sshos::UpdateState s = parse_update_state(raw, kNow);

  CHECK(s.status == UpdateStatus::Available);
  CHECK_EQ(s.source, std::string("git"));
  CHECK_EQ(s.installed_commit, std::string("aaaa"));
  CHECK_EQ(s.previous_commit, std::string("bbbb"));
  CHECK_EQ(s.checked_at, static_cast<std::int64_t>(1755300000));
  CHECK_EQ(s.message, std::string("une ligne"));
}

// UN SCHEMA INCONNU EST UN FICHIER ABSENT. Interpreter au hasard un format
// qu'on ne connait pas est pire que ne rien afficher.
TEST(update_state_treats_an_unknown_schema_as_absent) {
  const sshos::UpdateState s = parse_update_state("schema=2\nstatus=available\n", kNow);
  CHECK(s.status == UpdateStatus::Idle);
  CHECK(s.message.empty());
}

TEST(update_state_treats_an_absent_or_empty_file_as_idle) {
  CHECK(parse_update_state("", kNow).status == UpdateStatus::Idle);
  CHECK(parse_update_state("\n\n", kNow).status == UpdateStatus::Idle);
}

// AU-DELA DU PLAFOND, C'EST UN FICHIER ABSENT. Il est lu dans le fil unique
// du demon ; un resume de compilation de plusieurs megaoctets y ferait une
// allocation et une analyse synchrones a chaque relecture.
TEST(update_state_refuses_a_file_over_the_cap) {
  std::string raw = "schema=1\nstatus=available\nmessage=";
  raw.append(sshos::kMaxStateBytes, 'x');
  CHECK(parse_update_state(raw, kNow).status == UpdateStatus::Idle);
}

// LA PREMIERE OCCURRENCE GAGNE. Une ligne ajoutee apres coup -- par un
// message d'erreur mal assaini qui contiendrait un retour a la ligne -- ne
// doit pas pouvoir ecraser une valeur deja lue.
TEST(update_state_lets_the_first_occurrence_of_a_key_win) {
  const sshos::UpdateState s =
      parse_update_state("schema=1\nstatus=apply-failed\nstatus=up-to-date\n", kNow);
  CHECK(s.status == UpdateStatus::ApplyFailed);
}

TEST(update_state_ignores_a_line_without_a_separator) {
  const sshos::UpdateState s =
      parse_update_state("schema=1\nn importe quoi\nstatus=available\n", kNow);
  CHECK(s.status == UpdateStatus::Available);
}

TEST(update_state_splits_on_the_first_equals_sign) {
  const sshos::UpdateState s =
      parse_update_state("schema=1\nmessage=a=b=c\n", kNow);
  CHECK_EQ(s.message, std::string("a=b=c"));
}

TEST(update_state_ignores_an_unknown_key) {
  const sshos::UpdateState s =
      parse_update_state("schema=1\ninvente=oui\nstatus=available\n", kNow);
  CHECK(s.status == UpdateStatus::Available);
}

// UNE HORLOGE QUI A RECULE NE DOIT PAS FAIRE ATTENDRE UN JOUR DE PLUS, ET
// UNE VALEUR ABSURDE NE DOIT PAS DEBORDER : checked_at + 86400 sur
// INT64_MAX est un debordement signe. Les deux se traitent d'un seul geste.
TEST(update_state_clamps_checked_at_outside_the_plausible_range) {
  CHECK_EQ(parse_update_state("schema=1\nchecked_at=9223372036854775807\n", kNow).checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse_update_state("schema=1\nchecked_at=-5\n", kNow).checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse_update_state("schema=1\nchecked_at=pas un nombre\n", kNow).checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse_update_state("schema=1\nchecked_at=1755300000\n", kNow).checked_at,
           static_cast<std::int64_t>(1755300000));
}

TEST(update_state_truncates_an_oversized_message) {
  std::string raw = "schema=1\nmessage=";
  raw.append(sshos::kMaxMessageBytes + 50, 'z');
  raw.push_back('\n');
  CHECK_EQ(parse_update_state(raw, kNow).message.size(), sshos::kMaxMessageBytes);
}

TEST(update_state_reads_every_status_value) {
  struct Row { const char* text; UpdateStatus want; };
  const Row rows[] = {
      {"idle", UpdateStatus::Idle},
      {"checking", UpdateStatus::Checking},
      {"applying", UpdateStatus::Applying},
      {"up-to-date", UpdateStatus::UpToDate},
      {"available", UpdateStatus::Available},
      {"restart-pending", UpdateStatus::RestartPending},
      {"check-failed", UpdateStatus::CheckFailed},
      {"apply-failed", UpdateStatus::ApplyFailed},
      {"history-rewritten", UpdateStatus::HistoryRewritten},
      {"updates-disabled", UpdateStatus::UpdatesDisabled},
  };
  for (const Row& r : rows) {
    const std::string raw = std::string("schema=1\nstatus=") + r.text + "\n";
    CHECK(parse_update_state(raw, kNow).status == r.want);
  }
  // Une valeur inconnue ne devine pas : elle retombe sur Idle.
  CHECK(parse_update_state("schema=1\nstatus=invente\n", kNow).status == UpdateStatus::Idle);
}

TEST(update_state_reads_a_pid_only_when_it_is_a_number) {
  CHECK_EQ(parse_update_state("schema=1\npid=1234\n", kNow).pid, 1234);
  CHECK_EQ(parse_update_state("schema=1\npid=\n", kNow).pid, -1);
  CHECK_EQ(parse_update_state("schema=1\npid=abc\n", kNow).pid, -1);
}
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

Attendu : **échec de compilation**, `shell/update_state.hpp` introuvable.

- [ ] **Step 3: Implémenter**

Créer `src/shell/update_state.hpp` :

```cpp
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sshos {

// L'état que les scripts écrivent et que le démon lit. Le C++ ne l'écrit
// JAMAIS : c'est ce qui garde git, cmake et le réseau hors du démon.
enum class UpdateStatus {
  Idle,
  Checking,
  Applying,
  UpToDate,
  Available,
  RestartPending,
  CheckFailed,
  ApplyFailed,
  HistoryRewritten,
  UpdatesDisabled,
};

struct UpdateState {
  UpdateStatus status = UpdateStatus::Idle;
  std::string prefix;
  std::string source;
  std::string installed_commit;
  std::string previous_commit;
  std::string remote_commit;
  std::int64_t checked_at = 0;
  pid_t pid = -1;
  std::string message;
};

// Le fichier est lu dans le fil UNIQUE du démon. Un résumé de compilation
// de plusieurs mégaoctets y ferait une lecture, une allocation et une
// analyse synchrones dans la boucle d'affichage, à chaque relecture.
inline constexpr std::size_t kMaxStateBytes = 4096;
inline constexpr std::size_t kMaxMessageBytes = 200;

// Analyse PURE : ni disque, ni horloge. `now_epoch` sert uniquement à
// borner `checked_at`, et il est passé plutôt que lu pour que le cas d'une
// horloge reculée soit reproductible en test.
//
// Tolérante, jamais devineresse : clé inconnue ignorée, ligne sans `=`
// ignorée, valeur illisible ramenée au défaut, fichier absent / vide /
// de schéma inconnu / trop grand rendu comme un état vierge. Aucun cas ne
// lève. La PREMIÈRE occurrence d'une clé gagne, pour qu'une ligne ajoutée
// après coup ne puisse pas écraser une valeur déjà lue.
UpdateState parse_update_state(std::string_view raw, std::int64_t now_epoch);

}  // namespace sshos
```

Créer `src/shell/update_state.cpp` avec l'analyseur correspondant : découpe par `\n`, découpe au **premier** `=`, table de correspondance `status`, insertion seulement si la clé n'a pas déjà été vue, bornage de `checked_at` à `[0, now_epoch]` (hors bornes → `0`), troncature de `message` à `kMaxMessageBytes`, et sortie anticipée si `raw.size() > kMaxStateBytes` ou si `schema` ≠ `1`.

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests update_state
```

Attendu : les 12 cas au vert. Puis la suite entière.

- [ ] **Step 5: Campagne de mutation, puis commit**

```bash
git add src/shell/update_state.hpp src/shell/update_state.cpp tests/test_update_state.cpp
git commit -m "wip(update): analyseur du fichier d'etat avant campagne de mutation"
python3 tools/mutation.py --files src/shell/update_state.cpp
```

Pour chaque survivante : un cas de plus, ou une équivalence déclarée en commentaire. Puis :

```bash
git add -A
git commit -m "feat(update): analyseur pur du fichier d'etat

Le C++ lit cet etat, il ne l'ecrit jamais : c'est ce qui garde git, cmake
et le reseau hors du demon.

Trois regles valent la peine d'etre dites. La premiere occurrence d'une
cle gagne, pour qu'une ligne ajoutee apres coup par un message mal
assaini ne puisse pas ecraser une valeur deja lue. checked_at hors de
[0, maintenant] est ramene a zero, ce qui traite d'un seul geste
l'horloge reculee et le debordement de checked_at + 86400 sur INT64_MAX.
Et au-dela de 4 Kio le fichier est traite comme absent : il est lu dans
le fil unique du demon.

L'analyse est pure -- ni disque ni horloge -- donc integralement
testable."
```

---

## Task 6: `UpdateService`, la machine à sept états

**Contexte.** Spec §6. Trois coutures d'injection : chemin du fichier d'état, lanceur d'enfant, `const Platform&`.

**Files:**
- Create: `src/shell/update_service.hpp`, `src/shell/update_service.cpp`
- Test: `tests/test_update_service.cpp`

**Interfaces:**
- Consumes: `parse_update_state`, `UpdateState`, `UpdateStatus` (tâche 5) ; `sshos::Platform` (`src/common/platform.hpp:12`).
- Produces:
  ```cpp
  struct UpdateEntry { std::string label; bool enabled = true; std::string id; };
  class UpdateService {
   public:
    using Launcher = std::function<pid_t(const std::vector<std::string>& argv)>;
    UpdateService(const Platform& plat, std::string state_path, Launcher launch);
    void tick();                       // appelée par la session
    int delay_ms() const;              // -1 = rien à attendre
    UpdateEntry entry() const;         // libellé + enabled + id
    bool badge() const;
    void run(std::string_view id);     // update:check / update:apply / update:restart
    void on_child_exit(pid_t pid, int status);
    bool wants_restart() const;
    std::string message() const;
  };
  ```

- [ ] **Step 1: Écrire les tests qui échouent**

Créer `tests/test_update_service.cpp`. Le faux `Platform` et le faux lanceur, dans l'espace anonyme :

```cpp
namespace {

struct FakePlatform : sshos::Platform {
  std::int64_t wall = 1755400000;
  std::chrono::steady_clock::duration mono{};

  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(wall));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point(mono);
  }
  std::string read_file(std::string_view) const override { return {}; }

  void advance(std::chrono::seconds s) { mono += s; wall += s.count(); }
};

// Écrit un fichier d'état jetable et rend son chemin. Sous /tmp, jamais
// dans le vrai ~/.local : un test ne touche pas à l'installation réelle.
std::string write_state(const std::string& body) { /* ... */ }

}  // namespace
```

Les cas obligatoires, un par ligne du §11.1 de la spec :

```cpp
// LE LIBELLE ET L'INERTIE POUR CHACUN DES SEPT ETATS. MenuItem n'a pas de
// champ pour dire « inerte » ; c'est le service qui le porte, et c'est ce
// qui rend le cas testable ici plutot que seulement dans la session.
TEST(update_service_labels_every_state) {
  FakePlatform plat;
  struct Row { const char* status; const char* label; bool enabled; bool badge; };
  const Row rows[] = {
      {"idle",             "Verifier les mises a jour", true,  false},
      {"checking",         "Verification en cours...",  false, false},
      {"up-to-date",       "Verifier les mises a jour", true,  false},
      {"available",        "Mettre a jour",             true,  true},
      {"applying",         "Mise a jour en cours...",   false, false},
      {"restart-pending",  "Redemarrer pour terminer",  true,  true},
      {"check-failed",     "Verifier les mises a jour", true,  false},
  };
  for (const Row& r : rows) {
    const std::string path =
        write_state(std::string("schema=1\nstatus=") + r.status + "\n");
    sshos::UpdateService svc(plat, path, [](const std::vector<std::string>&) {
      return static_cast<pid_t>(-1);
    });
    svc.tick();
    CHECK_EQ(svc.entry().label, std::string(r.label));
    CHECK_EQ(svc.entry().enabled, r.enabled);
    CHECK_EQ(svc.badge(), r.badge);
  }
}

// UN COMMIT INCONNU NE SE COMPARE A RIEN, ET ON NE PRETEND PAS LE
// CONTRAIRE : l'entree propose de reinstaller, pas de mettre a jour.
TEST(update_service_offers_a_reinstall_when_the_installed_commit_is_unknown) { /* ... */ }

TEST(update_service_says_so_when_updates_are_disabled) { /* ... */ }

// UN TRAVAIL DONT LE PID EST MORT EST UN ECHEC, PAS UN TRAVAIL EN COURS.
// C'est le cas du demon redemarre pendant une application : sans cette
// regle, l'entree resterait inerte a vie.
TEST(update_service_treats_a_dead_worker_pid_as_a_failure) { /* ... */ }

// LE RELIQUAT EST BORNE A [0, 24 h]. checked_at est une heure MURALE
// ecrite par un script ; l'echeance, elle, court sur steady_now(), parce
// que platform.hpp:16 dit que tout ce qui expire dans ce projet s'y
// mesure. Une horloge qui a recule ne doit pas faire attendre deux jours.
TEST(update_service_clamps_the_remaining_delay_to_a_day) { /* ... */ }

// PAS DE VERIFICATION DANS LES TRENTE PREMIERES SECONDES. Ouvrir le bureau
// ne doit pas declencher un acces reseau.
TEST(update_service_waits_thirty_seconds_before_its_first_check) { /* ... */ }

// UN CLIC SUR UNE ENTREE INERTE NE LANCE RIEN. La garde ne repose pas sur
// le libelle : le service revoit son etat avant d'agir.
TEST(update_service_refuses_to_run_while_a_child_is_alive) { /* ... */ }

// L'ENFANT EST MORT SANS RIEN CHANGER : c'est un echec, pas un succes
// silencieux. Sans cette regle, rien ne distingue « rien fait » de
// « fait ».
TEST(update_service_fails_when_the_child_dies_without_touching_the_state) { /* ... */ }

TEST(update_service_ignores_a_child_exit_for_a_pid_it_did_not_launch) { /* ... */ }
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

Attendu : **échec de compilation**, `shell/update_service.hpp` introuvable.

- [ ] **Step 3: Implémenter**

Créer les deux fichiers. Points non négociables :

- `tick()` relit le fichier par `std::ifstream` sur `state_path_`, borne à `kMaxStateBytes`, passe le résultat à `parse_update_state(raw, wall_now)`.
- `Checking`/`Applying` avec un `pid` dont `::kill(pid, 0)` rend `-1` et `errno == ESRCH` → `Failed`, message `"interrompu"`.
- `delay_ms()` rend `-1` si aucune échéance ; sinon les millisecondes restantes, calculées sur `steady_now()`.
- Au premier `tick()`, l'échéance est `max(30 s, min(24 h, 24 h − (wall_now − checked_at)))`, le tout borné à `[30 s, 24 h]`.
- `run(id)` refuse si `entry().enabled == false`, sinon appelle `launch_({exe, "--check"})` ou `{exe, "--apply"}` et retient le pid.
- `on_child_exit(pid, status)` ignore un pid inconnu, sinon relit l'état ; si l'état n'a pas bougé et que `status != 0`, force `Failed`.

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests update_service
```

- [ ] **Step 5: Campagne de mutation, puis commit**

```bash
git add -A && git commit -m "wip(update): machine a etats avant campagne de mutation"
python3 tools/mutation.py --files src/shell/update_service.cpp
# un cas par survivante, ou une equivalence declaree sur place
git add -A
git commit -m "feat(update): la machine a sept etats du service de mise a jour

Trois coutures d'injection, chacune avec un precedent dans le projet : le
chemin du fichier d'etat (comme read_boot_id(boot_id_path)), le lanceur
d'enfant, et un const Platform& (comme Session). Aucun test ne touche au
reseau, ne forke, ni ne lit le vrai ~/.local.

checked_at est une heure murale ecrite par un script ; elle ne sert qu'une
fois, au demarrage, pour calculer un reliquat borne a [0, 24 h].
L'echeance elle-meme court sur steady_now(), parce que platform.hpp dit
que tout ce qui expire dans ce projet s'y mesure.

RestartPending existe pour une raison precise : apres une installation
sans redemarrage, le binaire pose n'est pas celui qui tourne. Sans cet
etat, la pastille s'eteindrait et l'utilisateur continuerait sur
l'ancienne version sans aucune indication."
```

---

## Task 7: `MenuItem::enabled` et `Menu::set_extra_items()`

**Contexte.** Spec §6.2 et §7.1. `Menu::open()` (`menu.cpp:32-54`) construit `all_` **lui-même**, sans paramètre, et `MenuItem` n'a que `{id, label}` — donc « inerte » n'a aucun observable et il n'existe aucun chemin pour un libellé dynamique.

**Files:**
- Modify: `src/shell/menu.hpp:11-14` et `:26-51`, `src/shell/menu.cpp:32-54`
- Test: `tests/test_menu.cpp`

**Interfaces:**
- Produces: `MenuItem{ id, label, enabled }` et `void Menu::set_extra_items(std::vector<MenuItem> items);` — consommés par la tâche 9.

- [ ] **Step 1: Écrire les tests qui échouent**

```cpp
// LE MENU NE SAIT RIEN DU SERVICE DE MISE A JOUR. Il recoit des entrees
// supplementaires et les rend comme les siennes : c'est ce qui evite que
// shell/ depende de l'etat du demon.
TEST(menu_appends_the_extra_items_after_its_own) {
  sshos::Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open();

  bool found = false;
  for (const sshos::MenuItem& it : m.visible()) {
    if (it.id == "update:check") {
      found = true;
      CHECK_EQ(it.label, std::string("Verifier les mises a jour"));
      CHECK(it.enabled);
    }
  }
  CHECK(found);
}

// UNE ENTREE INERTE RESTE VISIBLE : elle dit ce qui se passe. C'est la
// session qui refuse d'agir, pas le menu qui cache.
TEST(menu_keeps_a_disabled_item_visible) {
  sshos::Menu m;
  m.set_extra_items({{"update:apply", "Mise a jour en cours...", false}});
  m.open();

  bool found = false;
  for (const sshos::MenuItem& it : m.visible()) {
    if (it.id == "update:apply") { found = true; CHECK(!it.enabled); }
  }
  CHECK(found);
}

// LE CLIC DROIT PASSE PAR LE MEME CHEMIN. open_at() appelle open(), donc
// une entree ajoutee apparait aux TROIS points d'ouverture sans que
// personne ait a y penser -- l'invariant que menu.cpp:56-59 decrit deja.
TEST(menu_shows_the_extra_items_when_opened_at_the_cursor) {
  sshos::Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open_at(4, 4);

  bool found = false;
  for (const sshos::MenuItem& it : m.visible()) {
    if (it.id == "update:check") found = true;
  }
  CHECK(found);
}

// REPOSER LES ENTREES NE LES EMPILE PAS. Le libelle change a chaque
// changement d'etat ; deux ouvertures ne doivent pas donner deux lignes.
TEST(menu_replaces_the_extra_items_instead_of_accumulating_them) {
  sshos::Menu m;
  m.set_extra_items({{"update:check", "Verifier les mises a jour", true}});
  m.open();
  m.close();
  m.set_extra_items({{"update:apply", "Mettre a jour", true}});
  m.open();

  int count = 0;
  for (const sshos::MenuItem& it : m.visible()) {
    if (it.id.rfind("update:", 0) == 0) ++count;
  }
  CHECK_EQ(count, 1);
}
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

Attendu : **échec de compilation**, `set_extra_items` non déclarée.

- [ ] **Step 3: Implémenter**

`src/shell/menu.hpp` :

```cpp
struct MenuItem {
  std::string id;
  std::string label;
  // Une entrée peut être visible et sans effet : « Mise a jour en cours »
  // dit ce qui se passe, mais ne relance rien. Sans ce champ, l'inertie
  // n'aurait aucun observable et ne serait testable nulle part.
  bool enabled = true;
};
```

et, dans la partie publique de `Menu` :

```cpp
  // Des entrées que le menu ne sait pas fabriquer lui-même, posées par la
  // session avant chaque ouverture. Le menu ne connaît ni leur origine ni
  // leur sens -- il les rend comme les siennes, et la session interprète
  // l'identifiant. Reposer la liste la REMPLACE.
  void set_extra_items(std::vector<MenuItem> items) { extra_ = std::move(items); }
```

avec `std::vector<MenuItem> extra_;` en membre privé, et dans `Menu::open()`, après les entrées fixes :

```cpp
  for (const MenuItem& e : extra_) all_.push_back(e);
```

Vérifier que `refilter()` et `contains_fold()` fonctionnent sur les nouveaux libellés sans changement.

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests menu
```

Puis la suite entière — `MenuItem` gagne un champ, et les initialisations par accolades ailleurs dans le projet doivent encore compiler.

- [ ] **Step 5: Commiter**

```bash
git add src/shell/menu.hpp src/shell/menu.cpp tests/test_menu.cpp
git commit -m "feat(menu): entrees supplementaires et entrees inertes

Menu::open() construisait sa table seul, sans aucun parametre : il
n'existait aucun chemin par lequel la session aurait pu lui donner un
libelle qui change. set_extra_items() en ouvre un, et open_at() en
beneficie sans rien changer puisqu'il passe par open().

MenuItem gagne enabled. Sans ce champ, une entree « en cours » n'etait
qu'un libelle : ouvrir le menu et appuyer sur Entree la declenchait
quand meme, et l'inertie n'avait aucun observable a tester."
```

---

## Task 8: La pastille cliquable

**Contexte.** Spec §7.2. `PanelHit` (`panel.hpp:13`) n'a aucune valeur pour la pastille. Une pastille peinte et non cliquable viole *« La souris d'abord »*, règle que `REPRISE` §10 note avoir **déjà dû être redite**. Le précédent est `session.cpp:768` : *« Le rappel dit quelle touche ouvre l'aide ; le cliquer l'ouvre aussi. »*

**Files:**
- Modify: `src/shell/panel.hpp:13`, `src/shell/panel.cpp`
- Test: `tests/test_panel.cpp`

**Interfaces:**
- Produces: `PanelHit::Update` et `void Panel::set_update_badge(bool on);` — consommés par la tâche 9.

- [ ] **Step 1: Écrire les tests qui échouent**

```cpp
// LA PASTILLE SE CLIQUE. Une pastille qui annonce une mise a jour sans
// etre cliquable est le contre-exemple exact de la regle du projet.
TEST(panel_hits_the_update_badge_where_it_draws_it) {
  sshos::WindowManager wm;
  sshos::Panel p;
  p.set_update_badge(true);
  p.layout(wm, 80, 24, /*utf8=*/true);

  const sshos::Rect r = p.rect(80, 24);
  bool found = false;
  for (int x = 0; x < 80 && !found; ++x) {
    if (p.hit(x, r.y).what == sshos::PanelHit::Update) found = true;
  }
  CHECK(found);
}

TEST(panel_has_no_update_badge_when_it_is_off) {
  sshos::WindowManager wm;
  sshos::Panel p;
  p.set_update_badge(false);
  p.layout(wm, 80, 24, /*utf8=*/true);

  const sshos::Rect r = p.rect(80, 24);
  for (int x = 0; x < 80; ++x) {
    CHECK(p.hit(x, r.y).what != sshos::PanelHit::Update);
  }
}

// LE GLYPHE SUIT LE TERMINAL. Le panneau gere deja les deux modes ; une
// pastille en UTF-8 sur un terminal ASCII ferait un remplacement laid a
// l'endroit le plus visible du bureau.
TEST(panel_falls_back_to_ascii_for_the_update_badge) {
  sshos::WindowManager wm;
  sshos::Panel p;
  p.set_update_badge(true);
  p.layout(wm, 80, 24, /*utf8=*/false);

  sshos::Surface s(80, 24);
  p.draw(s.view(p.rect(80, 24)), sshos::Theme{}, "14:32");
  CHECK(sshos::surface_contains(s, "*"));
}
```

> Si `surface_contains` n'existe pas dans `tests/test_panel.cpp`, utiliser l'utilitaire d'inspection déjà employé par les autres cas du fichier — le lire avant d'écrire ce cas.

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

Attendu : **échec de compilation**, `PanelHit::Update` et `set_update_badge` inconnus.

- [ ] **Step 3: Implémenter**

`src/shell/panel.hpp:13` :

```cpp
enum class PanelHit { None, Body, MenuButton, Pinned, Task, Overflow, Clock, Hint, Update };
```

et, en public :

```cpp
  // La pastille de mise à jour, posée AVANT layout() : la discipline du
  // panneau veut que layout() calcule une fois ce que draw() et hit()
  // relisent, sinon ce qu'on clique n'est pas ce qu'on voit.
  //
  // Elle cède la place après le rappel et avant l'horloge : une à deux
  // colonnes, donc le cas est rare, mais il vaut mieux le trancher que le
  // découvrir sur un bureau chargé.
  void set_update_badge(bool on) { update_badge_ = on; }
```

Dans `panel.cpp`, `layout()` insère un `Item{PanelHit::Update, …}` d'une colonne (`●` en UTF-8, `*` sinon) juste avant l'horloge quand `update_badge_` est vrai, et `draw()` peint le même glyphe à la même position. `hit()` n'a rien à changer : il parcourt déjà la liste posée par `layout()`.

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests panel
```

Puis la suite entière : `PanelHit` gagne une valeur, et tout `switch` exhaustif dessus doit encore compiler sous `-Wswitch -Werror`.

- [ ] **Step 5: Commiter**

```bash
git add src/shell/panel.hpp src/shell/panel.cpp tests/test_panel.cpp
git commit -m "feat(panneau): la pastille de mise a jour, et elle se clique

La souris d'abord est la regle du projet, et le dossier de reprise note
qu'elle a deja du etre redite une fois. Une pastille peinte et non
cliquable en serait le contre-exemple exact -- le panneau a deja le
precedent avec le rappel de la touche leader, que cliquer ouvre l'aide.

PanelHit gagne Update, et la pastille est posee avant layout() pour que
draw() et hit() lisent la meme liste."
```

---

## Task 9: Câbler le service dans la session

**Contexte.** Spec §6.4, §6.5, §7.1, §7.3. C'est la tâche qui relie tout, et celle où vit le défaut signature du projet : *« un test qui appelle la méthode lui-même ne teste jamais son appelant »* (`REPRISE` §9 bis n° 10) — né sans appelant **quatorze fois**.

**Files:**
- Modify: `src/daemon/session.hpp`, `src/daemon/session.cpp:141-196` (routage), `:249, :766, :828` (ouvertures du menu), `:502-508` (réveil), `:538-553` (récolte)
- Test: `tests/test_session.cpp`

**Interfaces:**
- Consumes: `UpdateService` (tâche 6), `Menu::set_extra_items` (tâche 7), `Panel::set_update_badge` (tâche 8), `daemon_exe_path()` (tâche 3).
- Produces: `int Session::update_delay_ms() const;` et `void Session::tick_update();` — consommés par la tâche 10.

- [ ] **Step 1: Écrire les tests qui échouent**

```cpp
// LE RECOLTEUR APPELLE BIEN LE SERVICE. C'est le cablage qui est ne sans
// appelant quatorze fois dans ce projet, et celui dont depend toute la
// sortie de Applying : sans lui, l'entree reste inerte a vie.
TEST(session_routes_a_service_child_exit_to_the_update_service) { /* ... */ }

// UN ENFANT DE SERVICE N'A PAS DE FENETRE. Le routage passe par
// ChildWatch{pid, win} et ignore SILENCIEUSEMENT un pid absent de la
// table : sans aiguillage explicite avant cette recherche, la mort de
// l'enfant de mise a jour disparaitrait sans trace.
TEST(session_does_not_lose_a_service_child_in_the_window_lookup) { /* ... */ }

TEST(session_puts_the_update_entry_in_the_menu) { /* ... */ }

// LES TROIS PORTES DU MENU. Clavier, bouton du panneau, clic droit sur le
// vide : l'entree doit apparaitre par les trois.
TEST(session_shows_the_update_entry_from_all_three_menu_openings) { /* ... */ }

// UN CLIC SUR UNE ENTREE INERTE NE LANCE RIEN, meme si l'identifiant
// arrive jusqu'au routage. La garde ne repose pas sur l'interface.
TEST(session_refuses_a_disabled_update_command) { /* ... */ }

// LA CONFIRMATION DE REDEMARRAGE COMPTE CE QUI VA MOURIR, et elle le
// compte avec ce que la session sait reellement : des processus en cours,
// pas des « shells » -- elle ne connait pas les types d'applications.
TEST(session_counts_windows_and_running_children_before_restarting) { /* ... */ }

// ANNULER RESTE LA REPONSE PAR DEFAUT. modal.hpp:26 porte cet invariant
// pour les questions destructrices ; celle-ci en est une.
TEST(session_defaults_the_restart_confirmation_to_cancel) { /* ... */ }

TEST(session_clicking_the_panel_badge_opens_the_same_confirmation) { /* ... */ }
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

- [ ] **Step 3: Implémenter**

- `Session` gagne un `UpdateService update_;`, construit avec `*plat_`, le chemin `$HOME/.local/share/sshos/state`, et un lanceur qui **forke simplement** (voir ci-dessous).
- Avant chaque `menu_.open()` / `menu_.open_at()` (les trois sites `:249`, `:766`, `:828`) : `menu_.set_extra_items({update_.entry()})`.
- `run_menu()` reconnaît `update:check`, `update:apply`, `update:restart` **et revérifie `update_.entry().enabled`** avant d'agir.
- `PanelHit::Update` dans le `switch` de `:764` → même action que l'entrée courante.
- `mark_refresh_due()` appelle `update_.tick()` en plus de sa boucle sur les fenêtres.
- `on_child_exit(pid, status)` : **avant** la recherche dans `children_`, `if (update_.owns(pid)) { update_.on_child_exit(pid, status); return; }`.
- Le lanceur réel, avec les trois obligations du §6.5 de la spec :

```cpp
  // fork() SIMPLE, jamais spawn_detached : celui-ci fait un double fork et
  // rend le pid de l'intermédiaire, qui meurt en quelques millisecondes --
  // le vrai travail serait réparenté à init et ne pourrait plus jamais
  // être récolté. C'est le motif de Pty (pty.cpp:81) qu'il faut ici.
  const pid_t pid = ::fork();
  if (pid == 0) {
    // Le démon bloque SIGTERM/SIGINT/SIGCHLD pour son signalfd, et le
    // masque SURVIT à execve : sans ce rétablissement, git, make et le
    // binaire de tests hériteraient de SIGCHLD bloqué.
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (int s : {SIGPIPE, SIGHUP, SIGTERM, SIGINT, SIGCHLD}) ::sigaction(s, &dfl, nullptr);

    // La sortie ne doit ni se perdre ni atteindre le terminal.
    const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); ::close(fd); }
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) { ::dup2(devnull, 0); ::close(devnull); }

    ::execv(argv0.c_str(), raw.data());
    ::_exit(127);
  }
  return pid;
```

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests session
```

Puis la suite entière, y compris en Debug — ASan et UBSan trouvent ici ce que Release cache :

```bash
cmake --build build-debug -j"$(nproc)" && ./build-debug/sshos_tests
```

- [ ] **Step 5: Campagne de mutation, puis commit**

```bash
git add -A && git commit -m "wip(session): cablage du service avant campagne de mutation"
python3 tools/mutation.py --files src/daemon/session.cpp
git add -A
git commit -m "feat(session): cabler le service de mise a jour

Le lanceur fait un fork() SIMPLE. spawn_detached ferait un double fork et
rendrait le pid de l'intermediaire, qui meurt en quelques millisecondes :
le vrai travail serait reparente a init et ne pourrait plus jamais etre
recolte, donc Applying retomberait ~1 ms apres le clic sur un fichier
d'etat inchange. C'est le motif de Pty qu'il faut ici, masque de signaux
retabli compris -- il survit a execve.

La mort d'un enfant de service est aiguillee AVANT la recherche dans
children_, qui passe par une fenetre et ignore silencieusement un pid
inconnu.

Et le test verifie que le RECOLTEUR appelle le service, pas seulement que
le service reagit quand on l'appelle : c'est le defaut qui est revenu
quatorze fois dans ce projet."
```

---

## Task 10: Réveiller le démon sans client attaché

**Contexte.** Spec §6.4. `daemon.cpp:298-300` ne pose le plancher d'une seconde que `if (client)`, et `:319` lit `refresh_delay_ms()` sous la même garde — le commentaire l'assume : *« sans client il n'y a aucun plancher du tout : le démon au repos continue de bloquer indéfiniment »*. Un bureau détaché ne verrait donc jamais échoir ses 24 h. Le précédent exact existe : `help_delay_ms()` à `:311-314` n'a **pas** cette garde.

Second point : `wants_quit()` n'est lu qu'à `:541`, **dans la branche `EPOLLIN` du client** — une sortie décidée ailleurs n'aurait effet qu'à la prochaine frappe.

**Files:**
- Modify: `src/daemon/daemon.cpp:311-320`, `:541`
- Test: `tests/test_daemon.cpp`

**Interfaces:**
- Consumes: `Session::update_delay_ms()`, `Session::tick_update()` (tâche 9).

- [ ] **Step 1: Écrire le test qui échoue**

```cpp
// LE DEMON DETACHE GARDE UNE HORLOGE. C'est l'etat NORMAL de ce projet :
// une verification quotidienne qui ne bat que client attache ne bat
// presque jamais. help_delay_ms() est le precedent -- il n'a pas la garde
// if (client), et pour la meme raison.
TEST(daemon_wakes_for_the_update_deadline_without_a_client) { /* ... */ }

// SORTIR SANS QU'UNE TOUCHE SOIT TAPEE. wants_quit() n'etait lu que dans
// la branche EPOLLIN du client : un redemarrage decide a la fin d'une mise
// a jour aurait attendu la frappe suivante.
TEST(daemon_quits_when_the_session_asks_outside_of_a_key_event) { /* ... */ }
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests daemon_wakes daemon_quits
```

- [ ] **Step 3: Implémenter**

Après le bloc `help_delay` de `daemon.cpp:311-314`, sur le **même** modèle et **sans** garde `if (client)` :

```cpp
    // La verification quotidienne. PAS de garde `if (client)` : le bureau
    // detache est l'etat normal de ce projet, et une echeance qui ne bat
    // que client attache ne bat presque jamais. Rien ne s'affiche sans
    // client -- le service ne fait qu'ecrire un fichier d'etat.
    const int update_delay = session.update_delay_ms();
    if (update_delay >= 0) {
      timeout = (timeout < 0) ? update_delay : std::min(timeout, update_delay);
      if (update_delay == 0) session.tick_update();
    }
```

Et déplacer l'évaluation de `wants_quit()` en **fin de corps de boucle**, hors de la branche client :

```cpp
    // Hors de toute branche : une sortie peut etre decidee par la fin d'une
    // mise a jour, pas seulement par une touche.
    if (session.wants_quit()) running = false;
```

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
cmake --build build-debug -j"$(nproc)" && ./build-debug/sshos_tests
```

Vérifier aussi qu'un démon au repos **ne tourne pas à chaud** :

```bash
./build-release/sshos --daemon && sleep 5 && \
  ps -o %cpu= -p "$(./build-release/sshos --status | sed 's/[^0-9]//g')" && \
  ./build-release/sshos --kill
```

Attendu : une charge CPU proche de `0.0`. Un `update_delay_ms()` qui rendrait `0` en permanence transformerait la boucle en scrutin actif.

- [ ] **Step 5: Commiter**

```bash
git add src/daemon/daemon.cpp tests/test_daemon.cpp
git commit -m "fix(demon): l'echeance de mise a jour bat aussi sans client

Le plancher d'une seconde et le rafraichissement periodique sont gardes
par if (client), volontairement : sans client, epoll_wait bloque
indefiniment. Mais le bureau detache est l'etat NORMAL de ce projet, donc
une verification quotidienne gardee de la meme facon ne battrait presque
jamais. help_delay_ms est le precedent : lui non plus n'a pas la garde.

wants_quit() passe en fin de corps de boucle. Il n'etait lu que dans la
branche EPOLLIN du client : une sortie decidee a la fin d'une mise a jour
aurait attendu la frappe suivante."
```

---

## Task 11: Le redémarrage

**Contexte.** Spec §7.4. Trois défauts à corriger d'un coup : la raison de détachement doit être une **constante** et non du texte libre ; l'écouteur doit être fermé **avant** l'émission, sinon le client se reconnecte au démon mourant ; et le client doit **boucler** au lieu d'un essai unique.

**Files:**
- Modify: `src/common/proto.hpp`, `src/daemon/daemon.cpp:181-200, :746`, `src/client/client.cpp:281-284`, `src/client/client.hpp`, `src/main.cpp:114-131`
- Test: `tests/test_proto.cpp`, `tests/test_daemon.cpp`

**Interfaces:**
- Consumes: `daemon_exe_path()` (tâche 3).
- Produces: `inline constexpr const char* kDetachReasonUpdate = "mise a jour";`

- [ ] **Step 1: Écrire les tests qui échouent**

```cpp
// LA RAISON EST UNE CONSTANTE, PAS UNE PHRASE. Les quatre raisons
// existantes sont des litteraux francais que le client se contente
// d'imprimer (daemon.cpp:537, 607, 685, 746). Faire dependre un
// COMPORTEMENT d'une comparaison de texte libre casserait a la premiere
// reformulation ; la constante partagee ne derive pas.
TEST(proto_carries_a_stable_update_detach_reason) {
  const sshos::Msg m = sshos::Detached{sshos::kDetachReasonUpdate};
  const std::string wire = sshos::encode(m);

  sshos::Decoder d;
  d.feed(wire);
  const auto got = d.next();
  REQUIRE(got.has_value());
  const auto* det = std::get_if<sshos::Detached>(&*got);
  REQUIRE(det != nullptr);
  CHECK_EQ(det->reason, std::string(sshos::kDetachReasonUpdate));
}

// L'ECOUTEUR SE FERME AVANT L'EMISSION. Une adresse abstraite est liberee
// a la fermeture du descripteur, pas a la sortie du processus : si le
// demon emet puis sort, le client -- qui reagit en microsecondes -- se
// reconnecte au cadavre, son connect() reussit, et il se fait fermer au nez
// sur un message trompeur.
TEST(daemon_closes_its_listener_before_announcing_an_update_restart) { /* ... */ }
```

- [ ] **Step 2: Lancer et constater le rouge**

```bash
cmake --build build-release -j"$(nproc)" 2>&1 | tail -5
```

- [ ] **Step 3: Implémenter**

Dans `src/common/proto.hpp`, près de `Detached` :

```cpp
// La raison d'un détachement pour mise à jour. C'est la SEULE raison qui
// porte un comportement : le client la compare par ÉGALITÉ et rejoue son
// chemin de démarrage au lieu de rendre la main au shell. Toutes les
// autres raisons sont purement informatives, et reformuler l'une d'elles
// n'a aucune conséquence -- ce qui est précisément pourquoi celle-ci est
// une constante partagée et non une phrase écrite deux fois.
//
// Aucun changement de format de trame : `Detached` porte déjà une chaîne,
// donc `kBuildId` n'a pas à être incrémenté.
inline constexpr const char* kDetachReasonUpdate = "mise a jour";
```

Dans `daemon.cpp`, sur le chemin du redémarrage : fermer le descripteur d'écoute, **puis** `drop_client(kDetachReasonUpdate)`, **puis** sortir.

Dans `client.cpp:281-284`, distinguer la raison et rendre un code que `main()` sait lire (par exemple `kClientRestartRequested = 2`), sans changer le message affiché pour les autres raisons. Dans `main.cpp`, boucler **une fois** sur le chemin d'attache lorsque `run_client()` rend ce code, en réutilisant la cadence déjà écrite (`kConnectAttempts` × `kConnectDelayUs`, `main.cpp:17-18`) plutôt qu'un essai unique.

- [ ] **Step 4: Lancer et constater le vert**

```bash
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
cmake --build build-debug -j"$(nproc)" && ./build-debug/sshos_tests
```

- [ ] **Step 5: Commiter**

```bash
git add src/common/proto.hpp src/daemon/daemon.cpp src/client/client.cpp src/client/client.hpp src/main.cpp tests/test_proto.cpp tests/test_daemon.cpp
git commit -m "feat(protocole): le redemarrage pour mise a jour

Trois choses d'un coup. La raison devient une constante partagee,
comparee par egalite : les quatre raisons existantes sont des litteraux
que le client imprime, et faire dependre un comportement d'un texte libre
casserait a la premiere reformulation.

L'ecouteur se ferme AVANT l'emission. Une adresse abstraite est liberee a
la fermeture du descripteur, pas a la sortie du processus : en emettant
d'abord, le client se reconnectait au demon mourant, son connect
reussissait, et il se faisait fermer au nez sur un message trompeur.

Et le client boucle avec la cadence deja ecrite au lieu d'un essai
unique."
```

---

## Task 12: `tools/update.sh`

**Contexte.** Spec §5, §6.6, §8. Le seul endroit où vivent `git`, `cmake` et le réseau.

**Files:**
- Create: `tools/update.sh` (mode 0755)

- [ ] **Step 1: Écrire le script**

Trois modes : `--check`, `--apply`, `--rollback`. Le préfixe vient de **`SSHOS_PREFIX`**, jamais de `~` en dur — c'est ce qui permet à la sonde de la tâche 13 de ne pas écraser l'installation réelle.

Points **obligatoires**, chacun tiré d'un défaut identifié :

1. **`flock` sur `$SHARE/lock` pour toute la séquence** — sans lui, deux applications concurrentes font que `sshos.previous` finit par contenir le **nouveau** binaire, et `--rollback` restaure la version cassée.
2. **`status=checking` / `applying` écrits AVANT de travailler, avec `pid=$$`** — sinon un démon qui redémarre pendant une application ne peut pas savoir qu'un travail court.
3. **Contrôle de descendance** :
   ```sh
   if ! git -C "$SRC" merge-base --is-ancestor "$installed" "$remote"; then
     write_state history-rewritten "historique reecrit, reinstallation necessaire"
     exit 0
   fi
   ```
   Ce dépôt a **force-poussé `main` deux fois** ; sans ce contrôle, une réécriture proposerait une « mise à jour » vers un historique sans relation.
4. **Assainissement du message** avant écriture — un seul `\n` forgerait une paire clé=valeur :
   ```sh
   msg=$(printf '%s' "$raw" | tr -d '\000-\037\177' | cut -c1-200)
   ```
5. **Écriture atomique** : `state.tmp` dans le **même** répertoire, puis `mv -f`.
6. **La pose du binaire** exactement comme au §8.2 : `cp` vers `.previous`, `cp` vers `.new`, `chmod 0755`, `mv -f`.
7. **Délais** : `timeout 60` pour une vérification, `timeout 1800` pour une application.
8. **`--rollback` réécrit `installed_commit` depuis `previous_commit`** et pose `status=available` — sinon l'état continuerait d'annoncer le commit neuf et l'utilisateur ne pourrait plus jamais réappliquer la mise à jour dont il vient de revenir.
9. **Pas de descente d'échelon pendant une mise à jour** : un échec de l'échelon inscrit dans `source=` donne `apply-failed`.
10. **La suite complète avant la pose**, `SSHOS_GOLDEN_DIR` posée, critère = **code de retour**.

- [ ] **Step 2: Vérifier la syntaxe**

```bash
sh -n tools/update.sh && echo "syntaxe OK"
command -v shellcheck >/dev/null && shellcheck tools/update.sh || echo "shellcheck absent"
```

- [ ] **Step 3: Essai manuel dans un préfixe jetable**

```bash
rm -rf /tmp/sshos-maj && mkdir -p /tmp/sshos-maj/share
SSHOS_PREFIX=/tmp/sshos-maj sh tools/update.sh --check
cat /tmp/sshos-maj/share/sshos/state
```

Attendu : un fichier d'état bien formé, `checked_at` renseigné, et **aucune écriture** hors de `/tmp/sshos-maj`.

- [ ] **Step 4: Vérifier que le vrai `~/.local` n'a pas bougé**

```bash
ls -la ~/.local/share/sshos/ 2>/dev/null || echo "rien, comme attendu"
```

- [ ] **Step 5: Commiter**

```bash
rm -rf /tmp/sshos-maj
git add tools/update.sh
git commit -m "feat(outils): verification, application et retour arriere

Le seul endroit ou vivent git, cmake et le reseau. Le demon ne fait que
lire ce que ce script ecrit.

Le verrou n'est pas un ornement : sans lui, deux applications
concurrentes font que sshos.previous finit par contenir le NOUVEAU
binaire, et --rollback restaure alors la version cassee -- le filet de
securite detruit par la course qu'il devait rattraper.

Le controle de descendance non plus : ce depot a force-pousse main deux
fois, et une reecriture proposerait sinon une mise a jour vers un
historique sans relation avec celui en place.

Le prefixe vient de SSHOS_PREFIX, jamais de ~ en dur : c'est ce qui
permet a la sonde de ne pas ecraser l'installation reelle."
```

---

## Task 13: La sonde bout-en-bout

**Contexte.** Spec §11.3. Quatorze défauts de ce projet n'ont été trouvés ni par les tests unitaires ni par la relecture, seulement par une sonde. Les trois inversions corrigées par cette révision sont **indétectables** par un test unitaire : elles demandent un vrai redémarrage.

**Files:**
- Create: `tools/sonde_update.py`

- [ ] **Step 1: Écrire la sonde**

Sur le modèle de `tools/sonde.py`. Elle doit, **dans un `HOME` et un `SSHOS_PREFIX` temporaires** :

1. monter un faux dépôt git local, y commiter, le déclarer comme `origin` ;
2. `--check` → `status=available` ;
3. faire échouer la suite volontairement → `--apply` → **`apply-failed`, binaire inchangé** ;
4. remettre la suite au vert → `--apply` → binaire remplacé, `sshos.previous` = l'ancien ;
5. `--rollback` → l'ancien binaire revient **et `installed_commit` est réécrit** ;
6. lancer deux `--apply` concurrents → le second attend, `sshos.previous` **n'est pas** le nouveau binaire ;
7. réécrire l'historique du faux dépôt (`git commit --amend`) → `--check` → **`history-rewritten`** ;
8. remplacer le binaire **pendant qu'un démon l'exécute** → doit réussir (la séquence `cp`/`rename` du §8.2), là où une écriture en place rendrait ETXTBSY.

**Trois interdictions**, chacune payée cher sur ce projet :

- **jamais de commande détachée** (`&`, `nohup`, `disown`) ;
- la sonde **tue le démon qu'elle a lancé avant chaque essai**, sinon elle mesure le bureau de l'essai précédent ;
- elle identifie ses processus **par le pid qu'elle a elle-même obtenu**, jamais par un motif de nom — une sonde qui se cherche par motif se trouve elle-même.

- [ ] **Step 2: Lancer la sonde**

```bash
python3 tools/sonde_update.py
```

Attendu : chaque étape annoncée et validée, sortie 0.

- [ ] **Step 3: Vérifier qu'elle n'a rien touché de réel**

```bash
ls -la ~/.local/libexec/sshos ~/.local/share/sshos/state 2>/dev/null || echo "rien, comme attendu"
./build-release/sshos --status
```

Attendu : le bureau installé de l'utilisateur, s'il existe, est intact.

- [ ] **Step 4: Commiter**

```bash
git add tools/sonde_update.py
git commit -m "test(sonde): la chaine de mise a jour, de bout en bout

Quatorze defauts de ce projet n'ont ete trouves ni par les tests
unitaires ni par la relecture, seulement par une sonde. Les trois
inversions que la revision 2 de la spec corrige sont du meme genre :
aucune n'est detectable par un test ecrit d'apres la spec, elles
demandent un vrai redemarrage.

Elle verifie en particulier deux courses qu'aucun test unitaire ne peut
atteindre : deux applications concurrentes qui detruiraient
sshos.previous, et le remplacement d'un binaire pendant qu'un demon
l'execute -- ou une ecriture en place rendrait ETXTBSY a tous les coups.

HOME et SSHOS_PREFIX temporaires : SSHOS_BOOT_ID n'isole que le nom du
socket, pas les chemins."
```

---

## Task 14: Le workflow de publication

**Contexte.** Spec §10.

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 1: Écrire le workflow**

```yaml
name: release
on:
  push:
    branches: [main]

jobs:
  build:
    runs-on: ubuntu-latest
    # Conteneur EPINGLE. La cible est le systeme de l'auteur, pas un plus
    # petit denominateur commun : une image ancienne signifierait un
    # compilateur et des en-tetes anciens, donc des fonctionnalites
    # interdites dans sa propre application. Et ubuntu-latest derive.
    container: ubuntu:26.04
    steps:
      - run: apt-get update && apt-get install -y --no-install-recommends g++ cmake make git ca-certificates
      - uses: actions/checkout@v4
      - run: cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build build-release -j"$(nproc)"
      # Le critere est ZERO ECHEC, jamais un compte de cas : le total
      # perime a chaque commit qui ajoute un test.
      - run: ./build-release/sshos_tests
      - run: tar czf golden.tar.gz -C tests golden
      - run: |
          cd build-release && sha256sum sshos sshos_tests > ../SHA256SUMS
          cd .. && sha256sum golden.tar.gz >> SHA256SUMS
      - run: |
          gh release create "commit-${GITHUB_SHA}" \
            --title "${GITHUB_SHA}" --notes "Publication automatique." \
            build-release/sshos build-release/sshos_tests golden.tar.gz SHA256SUMS
        env:
          GH_TOKEN: ${{ github.token }}
```

- [ ] **Step 2: Vérifier la syntaxe YAML**

```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release.yml')); print('YAML OK')"
```

- [ ] **Step 3: Commiter et pousser**

```bash
git add .github/workflows/release.yml
git commit -m "ci(release): publier le binaire, les tests et les references

Conteneur epingle a ubuntu:26.04 plutot que ubuntu-latest : la cible est
le systeme de l'auteur, pas un plus petit denominateur commun, et
ubuntu-latest derive au fil du temps.

golden.tar.gz est publie avec sshos_tests. Sans les references, les cas
golden echouent chez celui qui telecharge -- test_golden.cpp gravait le
chemin de la machine de compilation -- et l'echelon binaire de
l'installeur raterait systematiquement son propre garde-fou.

Le critere de publication est zero echec, jamais un compte de cas."
git branch -f main m1-noyau && git push origin main
```

- [ ] **Step 4: Regarder la CI, et ne pas croire qu'elle passera du premier coup**

```bash
gh run watch
```

Ce projet a des tests sensibles à l'environnement — chronologie de `SIGSTOP`, récolte des enfants, survie du démon. **Une passe de réglage est probable.** Tant qu'aucune release n'est publiée, l'échelon 2 ne se déclenche pas et l'installeur compile : rien n'est cassé pendant ce temps.

- [ ] **Step 5: Vérifier la release, puis l'échelon 2 pour de vrai**

```bash
gh release list --limit 3
rm -rf /tmp/sshos-bin && mkdir -p /tmp/sshos-bin
# Forcer l'echelon 2 en cachant git au script.
HOME=/tmp/sshos-bin PATH=/usr/bin:/bin sh -c 'sh tools/install.sh'
```

Attendu : `source=release` dans le fichier d'état, et la suite complète au vert **depuis le binaire téléchargé** — la preuve que les tâches 2 et 14 se répondent.

---

# Auto-relecture

**Couverture de la spec.** §2 → Global Constraints. §3.1 → tâche 4. §3.2 → tâche 1. §3.3/§3.4 → documentaires. §4 → tâche 5. §4.4 → tâche 11. §5.0 → tâches 4 et 12 (drapeaux `curl`). §5.1–§5.7 → tâches 4 et 12. §6.1/§6.2/§6.3 → tâche 6. §6.4 → tâche 10. §6.5/§6.6 → tâches 9 et 12. §6.7 → tâche 6. §7.1 → tâche 7. §7.2 → tâche 8. §7.3 → tâche 9. §7.4 → tâches 3 et 11. §8 → tâches 6 et 12. §8.1/§8.2 → tâche 12. §9 → tâche 4. §10 → tâche 14. §11 → tâches 5, 6, 9, 13. §12 → hors périmètre, rien à implémenter. §13 → table historique.

**Cohérence des types.** `UpdateStatus` et `UpdateState` (tâche 5) sont consommés tels quels par `UpdateService` (tâche 6). `UpdateEntry{label, enabled, id}` (tâche 6) alimente `MenuItem{id, label, enabled}` (tâche 7). `PanelHit::Update` (tâche 8) est routé en tâche 9. `daemon_exe_path()` (tâche 3) est consommé par les tâches 4 et 11. `Session::update_delay_ms()` et `tick_update()` (tâche 9) sont consommés par la tâche 10. `kDetachReasonUpdate` (tâche 11) est utilisé des deux côtés du protocole.

**Point de vigilance à l'exécution.** Les tâches 7 et 8 ajoutent un champ à `MenuItem` et une valeur à `PanelHit`. Avec `-Wswitch -Werror`, tout `switch` exhaustif existant sur `PanelHit` cesse de compiler tant que le nouveau cas n'est pas traité : c'est voulu, et c'est le compilateur qui liste les sites à visiter.
