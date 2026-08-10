# ssh_os 2.0 — Jalon 1 : le noyau (rendu, protocole, client, démon détaché)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Amener `sshos` d'un dossier vide à une boîte colorée affichée à travers SSH par un démon qui survit à la déconnexion.

**Architecture:** Client mince. Le démon détient tout l'état, compose une grille de cellules et n'envoie que des diffs déjà encodés en ANSI ; le client met son terminal en mode brut, relaie des octets bruts et recopie ce qu'il reçoit. Un seul thread, un seul `epoll`, aucun mutex.

**Tech Stack:** C++20, CMake, glibc/Linux (`epoll`, `signalfd`, `timerfd`, sockets UNIX abstraits). Aucune bibliothèque externe.

**Spec de référence :** `docs/superpowers/specs/2026-08-10-ssh-os-design.md`. Ce plan couvre le jalon 1 de §15. Les jalons 2 à 6 (WM et panneau, Terminal, Fichiers, Moniteur, Éditeur) feront l'objet de plans distincts.

## Global Constraints

Ces contraintes valent pour **toutes** les tâches et ne sont pas répétées dans chacune.

- **C++20**, `set(CMAKE_CXX_EXTENSIONS OFF)`.
- Options de compilation : `-Wall -Wextra -Wpedantic -Werror`. En `Debug` : `-fsanitize=address,undefined -g -O0`. En `Release` : `-O2`.
- **Conséquence directe de `-Wpedantic -Werror` : `\e` est une extension GCC et ne compile pas.** Écrire `\033` dans tous les littéraux de chaîne. Une seule occurrence de `\e` casse la compilation du projet entier.
- **Zéro dépendance externe.** Pas de gtest, pas de ncurses, pas de fmt. Uniquement la bibliothèque standard et l'API POSIX/Linux.
- **Un seul binaire** : `sshos`, modes `(aucun)` / `--daemon` / `--kill` / `--status`.
- **Un thread, un `epoll`, aucun mutex.**
- Tous les descripteurs du démon sont ouverts `CLOEXEC`. Toutes les écritures sont non bloquantes.
- Adresse du socket : abstraite, `\0sshos/<uid>/<boot_id>`.
- Plafond de rendu : **33 ms** (30 fps). Plafond de contre-pression : **1 Mo**. Plafond de fenêtres : **64**.
- Délai d'ambiguïté `ESC` : **50 ms**. Touche leader par défaut : `Ctrl+A`.
- Le français est la langue des messages destinés à l'utilisateur **et des commentaires** ; le code et les identifiants sont en anglais. (Les commentaires portent le *pourquoi* d'une décision, souvent gagné à la revue de conception : ils sont écrits dans la langue de travail du projet.)

## Commandes de référence

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug     # une fois
cmake --build build -j                       # compiler
./build/sshos_tests                          # tous les tests
./build/sshos_tests diff                     # seulement ceux dont le nom contient "diff"
```

---

## Structure des fichiers

Chaque fichier a une responsabilité et une seule. Les en-têtes ne tirent que ce dont ils ont besoin.

| Fichier | Responsabilité |
|---|---|
| `CMakeLists.txt` | Build : bibliothèque `sshos_core`, binaire `sshos`, binaire `sshos_tests` |
| `tests/harness.hpp` | `TEST` / `CHECK` / `CHECK_EQ`, affichage `fichier:ligne`, échappement des octets de contrôle |
| `tests/main.cpp` | Lanceur : déroule le registre, filtre par sous-chaîne, code de retour |
| `src/common/fd.hpp/.cpp` | `Fd` RAII, `set_nonblock`, `set_cloexec` |
| `src/common/log.hpp/.cpp` | Puits de journalisation **injecté**, jamais global |
| `src/common/net.hpp/.cpp` | Socket UNIX abstrait : `bind` comme mutex, `connect`, `SO_PEERCRED` |
| `src/common/proto.hpp/.cpp` | Codec des messages : encodage, décodeur incrémental |
| `src/common/evloop.hpp/.cpp` | Enveloppes `epoll`, `timerfd`, `signalfd` |
| `src/render/cell.hpp` | `Color`, `Style`, `Cell`, `Rect`, `Size`, `Pos` |
| `src/render/width.hpp/.cpp` | Table Unicode de largeur embarquée, politique East Asian Ambiguous |
| `src/render/surface.hpp/.cpp` | `Surface` (grille) et `View` (rectangle clippé et translaté) |
| `src/render/profile.hpp/.cpp` | Profil de sortie, quantification des couleurs, encodage SGR |
| `src/render/diff.hpp/.cpp` | Diffeur : enveloppe de frame, état SGR, règles de largeur |
| `src/input/events.hpp` | `KeyEvent`, `MouseEvent`, `PasteEvent`, `FocusEvent`, `InputEvent` |
| `src/input/parser.hpp/.cpp` | Octets → événements : CSI, souris `Cb` bit à bit, collage, `ESC` isolé |
| `src/client/tty_guard.hpp/.cpp` | RAII du terminal : mode brut, modes DEC, restauration, filet `SIGSEGV` |
| `src/client/client.hpp/.cpp` | Boucle d'attache : handshake, relais d'octets, `SIGWINCH` |
| `src/daemon/daemonize.hpp/.cpp` | Double `fork`, `setsid`, assainissement, `exec /proc/self/exe` |
| `src/daemon/session.hpp/.cpp` | Le bureau. Au jalon 1 : une boîte colorée. Remplacé au jalon 2 |
| `src/daemon/daemon.hpp/.cpp` | Boucle `epoll`, gestion du client, contre-pression, cadence 30 fps |
| `src/main.cpp` | Aiguillage des modes |

`src/daemon/session.*` est délibérément un bouchon : il donne au jalon 1 quelque chose à afficher, et le jalon 2 le remplace par le vrai gestionnaire de fenêtres sans toucher au reste.

---

## Tâches

### Task 1: Squelette de build et harnais de test

Le harnais est de l'outillage, pas du produit : on ne le développe pas en TDD, on le vérifie en lui faisant signaler un échec pour de vrai puis en retirant l'échec.

**Files:**
- Create: `CMakeLists.txt`
- Create: `tests/harness.hpp`
- Create: `tests/main.cpp`
- Create: `tests/test_harness.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: rien.
- Produces: macros `TEST(name)`, `CHECK(cond)`, `CHECK_EQ(a, b)` ; binaire `build/sshos_tests` acceptant un filtre en argument ; code de retour 0 si tout passe, 1 sinon.

- [ ] **Step 1: Écrire le harnais**

Créer `tests/harness.hpp` :

```cpp
#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace th {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
}

// Les chaînes attendues contiennent des octets de contrôle : sans échappement,
// un diff de sortie ANSI est illisible et le test ne sert à rien.
inline std::string show(const std::string& v) {
  std::string out = "\"";
  for (unsigned char c : v) {
    if (c == 0x1b) {
      out += "\\e";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

template <class T>
std::string show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

}  // namespace th

#define TEST(name)                                        \
  static void test_##name();                              \
  static th::Registrar reg_##name(#name, &test_##name);   \
  static void test_##name()

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) th::fail(__FILE__, __LINE__, "CHECK(" #cond ")");     \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto&& _a = (a);                                                        \
    auto&& _b = (b);                                                        \
    if (!(_a == _b)) {                                                      \
      th::fail(__FILE__, __LINE__,                                          \
               "CHECK_EQ(" #a ", " #b ")\n       obtenu = " + th::show(_a) + \
                   "\n       attendu = " + th::show(_b));                   \
    }                                                                       \
  } while (0)
```

- [ ] **Step 2: Écrire le lanceur**

Créer `tests/main.cpp` :

```cpp
#include <cstring>

#include "harness.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed_cases = 0;

  for (const auto& c : th::registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    const int before = th::failures();
    std::printf("- %s\n", c.name);
    c.fn();
    ++ran;
    if (th::failures() > before) ++failed_cases;
  }

  std::printf("\n%d cas, %d en echec, %d assertions echouees\n", ran,
              failed_cases, th::failures());
  return th::failures() == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Écrire le CMakeLists**

Créer `CMakeLists.txt`. La bibliothèque `sshos_core` et le binaire `sshos` sont ajoutés en tâche 2, quand il existe des sources : `add_library` sans source échoue.

```cmake
cmake_minimum_required(VERSION 3.20)
project(sshos CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -g -O0)
  add_link_options(-fsanitize=address,undefined)
else()
  add_compile_options(-O2)
endif()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
add_executable(sshos_tests tests/main.cpp ${TEST_SOURCES})
target_include_directories(sshos_tests PRIVATE src tests)
```

Créer `.gitignore` :

```
build/
compile_commands.json
```

- [ ] **Step 4: Écrire un test qui échoue volontairement**

Créer `tests/test_harness.cpp` :

```cpp
#include <string>

#include "harness.hpp"

TEST(harness_reports_equality) {
  CHECK_EQ(2 + 2, 4);
  CHECK(true);
}

TEST(harness_reports_failure_ON_PURPOSE) {
  CHECK_EQ(std::string("\033[0m"), std::string("autre"));
}
```

- [ ] **Step 5: Compiler et vérifier que l'échec est signalé lisiblement**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/sshos_tests; echo "code de retour = $?"
```

Attendu : le lanceur affiche les deux cas, signale `FAIL tests/test_harness.cpp:12`, montre `obtenu = "\e[0m"` — **avec l'échappement**, pas un octet d'échappement brut — et rend le code 1.

Si `obtenu` affiche un caractère invisible plutôt que `\e`, la surcharge `show(const std::string&)` n'est pas sélectionnée : vérifier qu'elle est bien déclarée avant le modèle générique.

- [ ] **Step 6: Retirer l'échec volontaire**

Remplacer le second cas de `tests/test_harness.cpp` par :

```cpp
TEST(harness_escapes_control_bytes) {
  CHECK_EQ(th::show(std::string("\033[0m")), std::string("\"\\e[0m\""));
}
```

- [ ] **Step 7: Vérifier que tout passe**

```bash
cmake --build build -j && ./build/sshos_tests; echo "code de retour = $?"
```

Attendu : `2 cas, 0 en echec, 0 assertions echouees`, code de retour 0.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt .gitignore tests/
git commit -m "build: squelette CMake et harnais de test maison"
```

---

### Task 2: `Fd` RAII et intégration de la bibliothèque au build

**Files:**
- Create: `src/common/fd.hpp`, `src/common/fd.cpp`
- Create: `tests/test_fd.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: le harnais de la tâche 1.
- Produces:
  - `class sshos::Fd` — propriétaire unique d'un descripteur : `Fd()`, `explicit Fd(int)`, déplaçable, non copiable, `int get() const`, `bool valid() const`, `int release()`, `void reset(int = -1)`, destructeur qui ferme.
  - `void sshos::set_nonblock(int fd)` et `void sshos::set_cloexec(int fd)`, qui lèvent `std::system_error` en cas d'échec.
  - Cible CMake `sshos_core`, liée par `sshos_tests` et par le binaire `sshos`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_fd.cpp` :

```cpp
#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "common/fd.hpp"
#include "harness.hpp"

using sshos::Fd;

static bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(fd_closes_on_destruction) {
  int raw = ::open("/dev/null", O_RDONLY);
  CHECK(raw >= 0);
  {
    Fd f(raw);
    CHECK(f.valid());
    CHECK_EQ(f.get(), raw);
  }
  CHECK(!fd_is_open(raw));
}

TEST(fd_move_transfers_ownership) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.get();
  Fd b(std::move(a));
  CHECK(!a.valid());
  CHECK_EQ(b.get(), raw);
  CHECK(fd_is_open(raw));
}

TEST(fd_release_gives_up_ownership) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.release();
  CHECK(!a.valid());
  CHECK(fd_is_open(raw));
  ::close(raw);
}

TEST(fd_cloexec_and_nonblock_are_set) {
  Fd a(::open("/dev/null", O_RDONLY));
  sshos::set_cloexec(a.get());
  sshos::set_nonblock(a.get());
  CHECK((::fcntl(a.get(), F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((::fcntl(a.get(), F_GETFL) & O_NONBLOCK) != 0);
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC de compilation, `common/fd.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/common/fd.hpp` :

```cpp
#pragma once

#include <unistd.h>

#include <utility>

namespace sshos {

// Propriétaire unique d'un descripteur. Non copiable : deux propriétaires
// signifieraient une double fermeture, et une double fermeture après
// réattribution du numéro ferme le descripteur de quelqu'un d'autre.
class Fd {
 public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}

  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = -1;
    }
    return *this;
  }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  ~Fd() { reset(); }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  int release() noexcept { return std::exchange(fd_, -1); }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

void set_nonblock(int fd);
void set_cloexec(int fd);

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/common/fd.cpp` :

```cpp
#include "common/fd.hpp"

#include <fcntl.h>

#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

void set_nonblock(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags == -1) throw_errno("fcntl F_GETFL");
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) throw_errno("fcntl F_SETFL");
}

void set_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags == -1) throw_errno("fcntl F_GETFD");
  if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) throw_errno("fcntl F_SETFD");
}

}  // namespace sshos
```

- [ ] **Step 5: Ajouter la bibliothèque au CMakeLists**

Dans `CMakeLists.txt`, insérer avant le bloc `sshos_tests` :

```cmake
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(sshos_core STATIC ${CORE_SOURCES})
target_include_directories(sshos_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

et remplacer la définition de `sshos_tests` par :

```cmake
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
add_executable(sshos_tests tests/main.cpp ${TEST_SOURCES})
target_include_directories(sshos_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(sshos_tests PRIVATE sshos_core)
```

`GLOB_RECURSE` avec `CONFIGURE_DEPENDS` évite de modifier ce fichier à chaque tâche : `src/main.cpp` est retiré de la bibliothèque parce qu'il définit `main` et servira au binaire `sshos` en tâche 13.

- [ ] **Step 6: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ./build/sshos_tests fd
```

Attendu : `4 cas, 0 en echec`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/common/fd.hpp src/common/fd.cpp tests/test_fd.cpp
git commit -m "feat(common): Fd RAII, set_nonblock, set_cloexec"
```

---

### Task 3: Cellule, couleur, style et table de largeur Unicode

La largeur est le champ le plus dangereux du projet : dans un protocole qui n'envoie que des diffs, un désaccord de largeur ne se répare jamais tout seul (spec §4.1).

**Files:**
- Create: `src/render/cell.hpp`
- Create: `src/render/width.hpp`, `src/render/width.cpp`
- Create: `tests/test_width.cpp`

**Interfaces:**
- Consumes: rien.
- Produces:
  - `struct sshos::Rect { int x, y, w, h; }`, `struct sshos::Size { int w, h; }`, `struct sshos::Pos { int x, y; }`.
  - `enum class sshos::ColorKind { Default, Indexed, Rgb }` et `struct sshos::Color` avec `Color::def()`, `Color::indexed(uint8_t)`, `Color::rgb(uint8_t, uint8_t, uint8_t)`.
  - `namespace sshos::attr` : `Bold`, `Dim`, `Italic`, `Underline`, `Reverse`, `Strike` (`uint16_t`).
  - `struct sshos::Style { Color fg, bg; uint16_t attrs; }`.
  - `struct sshos::Cell { char32_t ch; uint32_t cluster; Color fg, bg; uint16_t attrs; uint8_t width; }`.
  - `int sshos::char_width(char32_t)` → `0`, `1` ou `2`.
  - `void sshos::set_ambiguous_wide(bool)` — politique East Asian Ambiguous, étroit par défaut.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_width.cpp` :

```cpp
#include "harness.hpp"
#include "render/cell.hpp"
#include "render/width.hpp"

using sshos::char_width;

TEST(width_ascii_is_one) {
  CHECK_EQ(char_width(U'a'), 1);
  CHECK_EQ(char_width(U' '), 1);
  CHECK_EQ(char_width(U'~'), 1);
}

TEST(width_cjk_is_two) {
  CHECK_EQ(char_width(U'\u65e5'), 2);  // 日
  CHECK_EQ(char_width(U'\uac00'), 2);  // 가 hangul
  CHECK_EQ(char_width(U'\uff21'), 2);  // Ａ pleine chasse
}

TEST(width_emoji_is_two) {
  CHECK_EQ(char_width(U'\U0001f600'), 2);  // 😀
}

TEST(width_combining_is_zero) {
  CHECK_EQ(char_width(U'\u0301'), 0);      // accent aigu combinant
  CHECK_EQ(char_width(U'\u200d'), 0);      // ZWJ
  CHECK_EQ(char_width(U'\ufe0f'), 0);      // sélecteur de variation 16
  CHECK_EQ(char_width(U'\U000e0101'), 0);  // sélecteur de variation 18
}

TEST(width_control_is_zero) {
  CHECK_EQ(char_width(U'\n'), 0);
  CHECK_EQ(char_width(U'\033'), 0);
}

// East Asian Ambiguous : étroit par défaut, large sur demande.
TEST(width_ambiguous_follows_policy) {
  sshos::set_ambiguous_wide(false);
  CHECK_EQ(char_width(U'\u00b1'), 1);  // ±
  CHECK_EQ(char_width(U'\u2500'), 1);  // ─ dessin de boîte

  sshos::set_ambiguous_wide(true);
  CHECK_EQ(char_width(U'\u00b1'), 2);
  CHECK_EQ(char_width(U'\u2500'), 2);

  sshos::set_ambiguous_wide(false);  // remettre l'état par défaut
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `render/cell.hpp: No such file or directory`.

- [ ] **Step 3: Écrire `cell.hpp`**

Créer `src/render/cell.hpp` :

```cpp
#pragma once

#include <cstdint>

namespace sshos {

struct Size {
  int w = 0;
  int h = 0;
  bool operator==(const Size&) const = default;
};

struct Pos {
  int x = 0;
  int y = 0;
  bool operator==(const Pos&) const = default;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool operator==(const Rect&) const = default;
  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

// Type somme explicite. Un entier nu confondrait SGR 39/49 (couleur par
// défaut du terminal) avec la couleur indexée 7, et écraserait le truecolor.
enum class ColorKind : uint8_t { Default, Indexed, Rgb };

struct Color {
  ColorKind kind = ColorKind::Default;
  uint8_t idx = 0;
  uint8_t r = 0, g = 0, b = 0;

  static Color def() { return {}; }
  static Color indexed(uint8_t i) { return {ColorKind::Indexed, i, 0, 0, 0}; }
  static Color rgb(uint8_t rr, uint8_t gg, uint8_t bb) {
    return {ColorKind::Rgb, 0, rr, gg, bb};
  }
  bool operator==(const Color&) const = default;
};

namespace attr {
inline constexpr uint16_t Bold = 1 << 0;
inline constexpr uint16_t Dim = 1 << 1;
inline constexpr uint16_t Italic = 1 << 2;
inline constexpr uint16_t Underline = 1 << 3;
inline constexpr uint16_t Reverse = 1 << 4;
inline constexpr uint16_t Strike = 1 << 5;
}  // namespace attr

struct Style {
  Color fg = Color::def();
  Color bg = Color::def();
  uint16_t attrs = 0;
  bool operator==(const Style&) const = default;
};

// width : 1 normal, 2 pleine chasse, 0 cellule de continuation.
// cluster : 0 quand le graphème tient dans `ch`, sinon index dans le
// réservoir de la Surface. Le réservoir ne coûte que sur ce qui l'exige.
struct Cell {
  char32_t ch = U' ';
  uint32_t cluster = 0;
  Color fg = Color::def();
  Color bg = Color::def();
  uint16_t attrs = 0;
  uint8_t width = 1;
  bool operator==(const Cell&) const = default;
};

inline constexpr Cell kContinuation{U'\0', 0, Color::def(), Color::def(), 0, 0};

}  // namespace sshos
```

- [ ] **Step 4: Écrire `width.hpp`**

Créer `src/render/width.hpp` :

```cpp
#pragma once

namespace sshos {

// 0 (combinant, contrôle), 1 (normal) ou 2 (pleine chasse).
// Table embarquée, jamais wcwidth() : wcwidth dépend de la locale du DÉMON,
// qui n'a aucune raison d'être celle du client — le démon tourne détaché,
// avec l'environnement fossilisé de la première session SSH.
int char_width(char32_t cp);

// Politique East Asian Ambiguous. Étroit par défaut ; le client la fixe à
// l'attache après sonde (spec §4.1).
void set_ambiguous_wide(bool wide);
bool ambiguous_wide();

}  // namespace sshos
```

- [ ] **Step 5: Écrire `width.cpp`**

Créer `src/render/width.cpp`. Les tables sont triées et interrogées par recherche dichotomique.

```cpp
#include "render/width.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace sshos {
namespace {

struct Range {
  char32_t lo, hi;
};

// Marques combinantes, formateurs, sélecteurs de variation.
constexpr std::array<Range, 14> kZero{{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x0E31, 0x0E31},
    {0x0E47, 0x0E4E}, {0x200B, 0x200F}, {0x2060, 0x2064}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF},
}};

// East Asian Wide et Fullwidth.
constexpr std::array<Range, 15> kWide{{
    {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
    {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
}};

// East Asian Ambiguous : large ou étroit selon le terminal, d'où la sonde.
constexpr std::array<Range, 8> kAmbiguous{{
    {0x00A1, 0x00A1}, {0x00B0, 0x00B4}, {0x00B6, 0x00BA}, {0x2010, 0x2027},
    {0x2190, 0x21FF}, {0x2500, 0x257F}, {0x25A0, 0x25FF}, {0x2E80, 0x2E80},
}};

template <std::size_t N>
bool in(const std::array<Range, N>& table, char32_t cp) {
  const auto it = std::upper_bound(
      table.begin(), table.end(), cp,
      [](char32_t v, const Range& r) { return v < r.lo; });
  if (it == table.begin()) return false;
  return cp <= std::prev(it)->hi;
}

bool g_ambiguous_wide = false;

}  // namespace

void set_ambiguous_wide(bool wide) { g_ambiguous_wide = wide; }
bool ambiguous_wide() { return g_ambiguous_wide; }

int char_width(char32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  if (in(kZero, cp)) return 0;
  if (g_ambiguous_wide && in(kAmbiguous, cp)) return 2;
  if (in(kWide, cp)) return 2;
  return 1;
}

}  // namespace sshos
```

`kWide` contient `{0x2E80, 0x303E}` et `kAmbiguous` contient `{0x2E80, 0x2E80}` : l'ordre des tests dans `char_width` fait que la politique ambiguë est consultée en premier, ce qui est le comportement voulu.

- [ ] **Step 6: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests width
```

Attendu : `6 cas, 0 en echec`.

- [ ] **Step 7: Commit**

```bash
git add src/render/cell.hpp src/render/width.hpp src/render/width.cpp tests/test_width.cpp
git commit -m "feat(render): Cell, Color typee, table de largeur Unicode embarquee"
```

---

### Task 4: `Surface` et `View`

`View` est le seul type qu'une application verra jamais. Écrire hors du clip n'est pas une erreur : c'est ignoré. C'est ce qui rend structurellement impossible qu'un bug d'arithmétique dans une application peigne par-dessus la barre des tâches.

**Files:**
- Create: `src/render/surface.hpp`, `src/render/surface.cpp`
- Create: `tests/test_surface.cpp`

**Interfaces:**
- Consumes: `Cell`, `Style`, `Rect` (tâche 3) ; `char_width` (tâche 3).
- Produces:
  - `class sshos::Surface` : `Surface(int w, int h)`, `int w() const`, `int h() const`, `void resize(int, int)`, `const Cell& at(int, int) const`, `Cell& at(int, int)`, `void clear(Style)`, `View root()`.
  - `class sshos::View` : `int w() const`, `int h() const`, `void put(int, int, char32_t, Style)`, `int text(int, int, std::string_view, Style)` (rend le nombre de colonnes avancées), `void fill(Rect, Style)`, `View sub(Rect) const`.
  - `size_t sshos::utf8_decode(std::string_view, size_t pos, char32_t& out)` — rend le nombre d'octets consommés, `out` vaut U+FFFD sur séquence invalide.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_surface.cpp` :

```cpp
#include <string>

#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Cell;
using sshos::Rect;
using sshos::Style;
using sshos::Surface;
using sshos::View;

TEST(surface_starts_blank) {
  Surface s(4, 2);
  CHECK_EQ(s.w(), 4);
  CHECK_EQ(s.h(), 2);
  CHECK_EQ(s.at(0, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(3, 1).width), 1);
}

TEST(surface_view_translates_coordinates) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  CHECK_EQ(v.w(), 3);
  CHECK_EQ(v.h(), 2);
  v.put(0, 0, U'X', Style{});
  CHECK_EQ(s.at(2, 1).ch, U'X');
  CHECK_EQ(s.at(0, 0).ch, U' ');
}

// La propriété qui rend une application incapable de nuire à ses voisines.
TEST(surface_view_silently_drops_out_of_clip_writes) {
  Surface s(10, 4);
  View v = s.root().sub(Rect{2, 1, 3, 2});
  v.put(-1, 0, U'A', Style{});
  v.put(3, 0, U'B', Style{});
  v.put(0, 2, U'C', Style{});
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 10; ++x) CHECK_EQ(s.at(x, y).ch, U' ');
  }
}

TEST(surface_nested_sub_clips_to_parent) {
  Surface s(10, 4);
  View outer = s.root().sub(Rect{2, 0, 4, 1});
  View inner = outer.sub(Rect{2, 0, 10, 1});  // déborde volontairement
  CHECK_EQ(inner.w(), 2);
  inner.put(0, 0, U'Z', Style{});
  CHECK_EQ(s.at(4, 0).ch, U'Z');
}

TEST(surface_text_writes_utf8) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "abc", Style{});
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'a');
  CHECK_EQ(s.at(2, 0).ch, U'c');
}

TEST(surface_text_marks_wide_and_continuation) {
  Surface s(6, 1);
  View v = s.root();
  const int cols = v.text(0, 0, "\xe6\x97\xa5x", Style{});  // 日x
  CHECK_EQ(cols, 3);
  CHECK_EQ(s.at(0, 0).ch, U'\u65e5');
  CHECK_EQ(static_cast<int>(s.at(0, 0).width), 2);
  CHECK_EQ(static_cast<int>(s.at(1, 0).width), 0);
  CHECK_EQ(s.at(2, 0).ch, U'x');
}

// Règle 2 du §4.1 : jamais de glyphe large en dernière colonne. Sans elle,
// le terminal replie la ligne et le modele de frame precedente est perdu.
TEST(surface_never_places_wide_glyph_in_last_column) {
  Surface s(3, 2);
  View v = s.root();
  const int cols = v.text(2, 0, "\xe6\x97\xa5", Style{});  // 日 en derniere colonne
  CHECK_EQ(cols, 0);
  CHECK_EQ(s.at(2, 0).ch, U' ');
  CHECK_EQ(static_cast<int>(s.at(2, 0).width), 1);
}

TEST(surface_fill_respects_clip) {
  Surface s(6, 3);
  View v = s.root().sub(Rect{1, 1, 2, 1});
  Style red;
  red.bg = sshos::Color::indexed(1);
  v.fill(Rect{0, 0, 100, 100}, red);
  CHECK_EQ(s.at(1, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(2, 1).bg, sshos::Color::indexed(1));
  CHECK_EQ(s.at(3, 1).bg, sshos::Color::def());
  CHECK_EQ(s.at(1, 0).bg, sshos::Color::def());
}

TEST(utf8_decode_handles_truncated_input) {
  char32_t cp = 0;
  const std::string truncated = "\xe6\x97";  // moitie de 日
  const size_t used = sshos::utf8_decode(truncated, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(2));
  CHECK_EQ(cp, U'\ufffd');
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `render/surface.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/render/surface.hpp` :

```cpp
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "render/cell.hpp"

namespace sshos {

class View;

class Surface {
 public:
  Surface(int w, int h);

  int w() const { return w_; }
  int h() const { return h_; }

  void resize(int w, int h);
  void clear(Style s);

  const Cell& at(int x, int y) const { return cells_[static_cast<size_t>(y) * w_ + x]; }
  Cell& at(int x, int y) { return cells_[static_cast<size_t>(y) * w_ + x]; }

  View root();

 private:
  int w_ = 0;
  int h_ = 0;
  std::vector<Cell> cells_;
};

// Rectangle clippé et translaté sur une Surface. Une application ne reçoit
// jamais autre chose que ça.
class View {
 public:
  View(Surface& s, Rect clip) : s_(&s), clip_(clip) {}

  int w() const { return clip_.w; }
  int h() const { return clip_.h; }

  void put(int x, int y, char32_t ch, Style st);
  int text(int x, int y, std::string_view utf8, Style st);
  void fill(Rect r, Style st);
  View sub(Rect r) const;

 private:
  bool map(int x, int y, int& ox, int& oy) const;

  Surface* s_;
  Rect clip_;
};

// Rend le nombre d'octets consommés. `out` vaut U+FFFD sur séquence
// invalide ou tronquée, et la consommation avance toujours d'au moins 1 :
// un décodeur qui n'avance pas boucle indéfiniment.
size_t utf8_decode(std::string_view s, size_t pos, char32_t& out);

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/render/surface.cpp` :

```cpp
#include "render/surface.hpp"

#include <algorithm>

#include "render/width.hpp"

namespace sshos {

Surface::Surface(int w, int h) { resize(w, h); }

void Surface::resize(int w, int h) {
  w_ = std::max(0, w);
  h_ = std::max(0, h);
  cells_.assign(static_cast<size_t>(w_) * h_, Cell{});
}

void Surface::clear(Style s) {
  Cell c;
  c.fg = s.fg;
  c.bg = s.bg;
  c.attrs = s.attrs;
  std::fill(cells_.begin(), cells_.end(), c);
}

View Surface::root() { return View(*this, Rect{0, 0, w_, h_}); }

bool View::map(int x, int y, int& ox, int& oy) const {
  if (x < 0 || y < 0 || x >= clip_.w || y >= clip_.h) return false;
  ox = clip_.x + x;
  oy = clip_.y + y;
  return ox >= 0 && oy >= 0 && ox < s_->w() && oy < s_->h();
}

void View::put(int x, int y, char32_t ch, Style st) {
  int ox = 0;
  int oy = 0;
  if (!map(x, y, ox, oy)) return;

  const int cw = char_width(ch);
  if (cw == 0) return;
  // Règle 2 du §4.1 : jamais de glyphe large en dernière colonne.
  if (cw == 2 && (x + 1 >= clip_.w || ox + 1 >= s_->w())) return;

  Cell& c = s_->at(ox, oy);
  c.ch = ch;
  c.cluster = 0;
  c.fg = st.fg;
  c.bg = st.bg;
  c.attrs = st.attrs;
  c.width = static_cast<uint8_t>(cw);

  if (cw == 2) {
    Cell& cont = s_->at(ox + 1, oy);
    cont = kContinuation;
    cont.bg = st.bg;
  }
}

int View::text(int x, int y, std::string_view utf8, Style st) {
  int col = x;
  size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = 0;
    i += utf8_decode(utf8, i, cp);
    const int cw = char_width(cp);
    if (cw == 0) continue;
    if (col + cw > clip_.w) break;
    put(col, y, cp, st);
    col += cw;
  }
  return col - x;
}

void View::fill(Rect r, Style st) {
  const int x0 = std::max(0, r.x);
  const int y0 = std::max(0, r.y);
  const int x1 = std::min(clip_.w, r.x + r.w);
  const int y1 = std::min(clip_.h, r.y + r.h);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      int ox = 0;
      int oy = 0;
      if (!map(x, y, ox, oy)) continue;
      Cell& c = s_->at(ox, oy);
      c = Cell{};
      c.fg = st.fg;
      c.bg = st.bg;
      c.attrs = st.attrs;
    }
  }
}

View View::sub(Rect r) const {
  const int x0 = clip_.x + std::max(0, r.x);
  const int y0 = clip_.y + std::max(0, r.y);
  const int x1 = std::min(clip_.x + clip_.w, clip_.x + r.x + r.w);
  const int y1 = std::min(clip_.y + clip_.h, clip_.y + r.y + r.h);
  return View(*s_, Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)});
}

size_t utf8_decode(std::string_view s, size_t pos, char32_t& out) {
  out = U'\ufffd';
  if (pos >= s.size()) return 1;

  const auto b0 = static_cast<unsigned char>(s[pos]);
  int need = 0;
  char32_t cp = 0;

  if (b0 < 0x80) {
    out = b0;
    return 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    need = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    need = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    need = 3;
    cp = b0 & 0x07;
  } else {
    return 1;  // octet de continuation isolé ou séquence illégale
  }

  if (pos + need >= s.size() + 0 && pos + need > s.size() - 1) {
    // séquence tronquée : consommer ce qui est là, sans jamais rendre 0
    return s.size() - pos;
  }

  for (int k = 1; k <= need; ++k) {
    const auto bk = static_cast<unsigned char>(s[pos + k]);
    if ((bk & 0xC0) != 0x80) return static_cast<size_t>(k);
    cp = (cp << 6) | (bk & 0x3F);
  }

  out = cp;
  return static_cast<size_t>(need + 1);
}

}  // namespace sshos
```

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests surface
```

Attendu : `9 cas, 0 en echec`. Si `utf8_decode_handles_truncated_input` échoue, la condition de troncature est mal écrite : elle doit rendre `s.size() - pos` (ici 2), jamais 0.

- [ ] **Step 6: Commit**

```bash
git add src/render/surface.hpp src/render/surface.cpp tests/test_surface.cpp
git commit -m "feat(render): Surface et View avec clipping et regles de largeur"
```

---

### Task 5: Profil de sortie et encodage SGR

Le thème est écrit une fois en RGB et converti à l'attache selon ce que le client sait afficher.

**Files:**
- Create: `src/render/profile.hpp`, `src/render/profile.cpp`
- Create: `tests/test_profile.cpp`

**Interfaces:**
- Consumes: `Color`, `Style`, `attr::*` (tâche 3).
- Produces:
  - `enum class sshos::ColorDepth { Mono16, Indexed256, TrueColor }`.
  - `struct sshos::OutputProfile { ColorDepth depth; bool utf8; static OutputProfile detect(std::string_view term, std::string_view colorterm, bool utf8); }`.
  - `std::string sshos::sgr_transition(const Style& from, const Style& to, const OutputProfile&)`.
  - `std::string sshos::encode_utf8(char32_t)`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_profile.cpp` :

```cpp
#include <string>

#include "harness.hpp"
#include "render/profile.hpp"

using sshos::Color;
using sshos::ColorDepth;
using sshos::OutputProfile;
using sshos::sgr_transition;
using sshos::Style;

TEST(profile_detects_truecolor) {
  const auto p = OutputProfile::detect("xterm-256color", "truecolor", true);
  CHECK(p.depth == ColorDepth::TrueColor);
  CHECK(p.utf8);
}

TEST(profile_detects_256_and_16) {
  CHECK(OutputProfile::detect("xterm-256color", "", true).depth ==
        ColorDepth::Indexed256);
  CHECK(OutputProfile::detect("xterm", "", true).depth == ColorDepth::Mono16);
  CHECK(OutputProfile::detect("vt100", "", false).depth == ColorDepth::Mono16);
}

TEST(sgr_emits_nothing_when_style_is_unchanged) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  CHECK_EQ(sgr_transition(Style{}, Style{}, p), std::string(""));
}

TEST(sgr_truecolor_foreground) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style to;
  to.fg = Color::rgb(255, 0, 0);
  CHECK_EQ(sgr_transition(Style{}, to, p), std::string("\033[38;2;255;0;0m"));
}

TEST(sgr_quantizes_to_256_and_16) {
  Style to;
  to.fg = Color::rgb(255, 0, 0);
  const auto p256 = OutputProfile::detect("xterm-256color", "", true);
  CHECK_EQ(sgr_transition(Style{}, to, p256), std::string("\033[38;5;196m"));
  const auto p16 = OutputProfile::detect("xterm", "", true);
  CHECK_EQ(sgr_transition(Style{}, to, p16), std::string("\033[31m"));
}

TEST(sgr_adds_attribute_incrementally) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style to;
  to.attrs = sshos::attr::Bold;
  CHECK_EQ(sgr_transition(Style{}, to, p), std::string("\033[1m"));
}

// Retirer un attribut n'a pas de code incrémental fiable : on réinitialise
// puis on repose l'état complet.
TEST(sgr_resets_when_an_attribute_is_removed) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.attrs = sshos::attr::Bold | sshos::attr::Underline;
  from.fg = Color::rgb(1, 2, 3);
  Style to;
  to.attrs = sshos::attr::Bold;
  CHECK_EQ(sgr_transition(from, to, p), std::string("\033[0m\033[1m"));
}

TEST(sgr_returns_to_default_color_with_39_and_49) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.fg = Color::rgb(255, 0, 0);
  from.bg = Color::indexed(4);
  CHECK_EQ(sgr_transition(from, Style{}, p), std::string("\033[39m\033[49m"));
}

TEST(encode_utf8_roundtrip) {
  CHECK_EQ(sshos::encode_utf8(U'a'), std::string("a"));
  CHECK_EQ(sshos::encode_utf8(U'日'), std::string("\xe6\x97\xa5"));
  CHECK_EQ(sshos::encode_utf8(U'\U0001f600'), std::string("\xf0\x9f\x98\x80"));
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `render/profile.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/render/profile.hpp` :

```cpp
#pragma once

#include <string>
#include <string_view>

#include "render/cell.hpp"

namespace sshos {

enum class ColorDepth { Mono16, Indexed256, TrueColor };

struct OutputProfile {
  ColorDepth depth = ColorDepth::Mono16;
  bool utf8 = false;

  static OutputProfile detect(std::string_view term, std::string_view colorterm,
                              bool utf8);
};

// Séquence minimale pour passer du style `from` au style `to`. Le diffeur
// suit l'état courant sur toute la frame : une ligne uniforme ne coûte
// qu'un seul SGR.
std::string sgr_transition(const Style& from, const Style& to,
                           const OutputProfile& p);

std::string encode_utf8(char32_t cp);

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/render/profile.cpp` :

```cpp
#include "render/profile.hpp"

#include <array>
#include <cstdio>

namespace sshos {
namespace {

std::string num(int v) { return std::to_string(v); }

// Cube 6x6x6 d'xterm, base 16.
uint8_t quantize_256(uint8_t r, uint8_t g, uint8_t b) {
  const auto q = [](uint8_t v) { return static_cast<int>(v * 5 / 255); };
  return static_cast<uint8_t>(16 + 36 * q(r) + 6 * q(g) + q(b));
}

// Bit 0 = rouge, bit 1 = vert, bit 2 = bleu : l'ordre ANSI historique.
int quantize_16(uint8_t r, uint8_t g, uint8_t b) {
  return (r > 127 ? 1 : 0) | (g > 127 ? 2 : 0) | (b > 127 ? 4 : 0);
}

void indexed_to_rgb(uint8_t idx, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (idx < 16) {
    const int lo = (idx & 8) != 0 ? 255 : 128;
    r = ((idx & 1) != 0) ? static_cast<uint8_t>(lo) : 0;
    g = ((idx & 2) != 0) ? static_cast<uint8_t>(lo) : 0;
    b = ((idx & 4) != 0) ? static_cast<uint8_t>(lo) : 0;
  } else if (idx < 232) {
    const int v = idx - 16;
    const std::array<int, 6> steps{0, 95, 135, 175, 215, 255};
    r = static_cast<uint8_t>(steps[(v / 36) % 6]);
    g = static_cast<uint8_t>(steps[(v / 6) % 6]);
    b = static_cast<uint8_t>(steps[v % 6]);
  } else {
    const auto v = static_cast<uint8_t>(8 + (idx - 232) * 10);
    r = g = b = v;
  }
}

std::string color_code(const Color& c, bool foreground, const OutputProfile& p) {
  const int base = foreground ? 38 : 48;
  const int simple = foreground ? 30 : 40;
  const int reset = foreground ? 39 : 49;

  if (c.kind == ColorKind::Default) return "\033[" + num(reset) + "m";

  uint8_t r = c.r;
  uint8_t g = c.g;
  uint8_t b = c.b;
  if (c.kind == ColorKind::Indexed) indexed_to_rgb(c.idx, r, g, b);

  switch (p.depth) {
    case ColorDepth::TrueColor:
      return "\033[" + num(base) + ";2;" + num(r) + ";" + num(g) + ";" + num(b) + "m";
    case ColorDepth::Indexed256:
      if (c.kind == ColorKind::Indexed)
        return "\033[" + num(base) + ";5;" + num(c.idx) + "m";
      return "\033[" + num(base) + ";5;" + num(quantize_256(r, g, b)) + "m";
    case ColorDepth::Mono16:
      return "\033[" + num(simple + quantize_16(r, g, b)) + "m";
  }
  return "";
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

}  // namespace

OutputProfile OutputProfile::detect(std::string_view term,
                                    std::string_view colorterm, bool utf8) {
  OutputProfile p;
  p.utf8 = utf8;
  if (contains(colorterm, "truecolor") || contains(colorterm, "24bit")) {
    p.depth = ColorDepth::TrueColor;
  } else if (contains(term, "256color")) {
    p.depth = ColorDepth::Indexed256;
  } else {
    p.depth = ColorDepth::Mono16;
  }
  return p;
}

std::string sgr_transition(const Style& from, const Style& to,
                           const OutputProfile& p) {
  if (from == to) return "";

  std::string out;
  Style base = from;

  // Aucun code n'éteint un attribut de façon portable : on réinitialise.
  if ((from.attrs & ~to.attrs) != 0) {
    out += "\033[0m";
    base = Style{};
  }

  const std::array<std::pair<uint16_t, int>, 6> codes{{
      {attr::Bold, 1},
      {attr::Dim, 2},
      {attr::Italic, 3},
      {attr::Underline, 4},
      {attr::Reverse, 7},
      {attr::Strike, 9},
  }};
  for (const auto& [bit, code] : codes) {
    if ((to.attrs & bit) != 0 && (base.attrs & bit) == 0) {
      out += "\033[" + num(code) + "m";
    }
  }

  if (!(base.fg == to.fg)) out += color_code(to.fg, true, p);
  if (!(base.bg == to.bg)) out += color_code(to.bg, false, p);
  return out;
}

std::string encode_utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

}  // namespace sshos
```

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests profile
```

Attendu : `9 cas, 0 en echec`.

- [ ] **Step 6: Commit**

```bash
git add src/render/profile.hpp src/render/profile.cpp tests/test_profile.cpp
git commit -m "feat(render): profil de sortie et transitions SGR"
```

---

### Task 6: Le diffeur

Le composant dont la correction se mesure en octets exacts. Toutes ses assertions portent sur la sortie ANSI littérale : c'est le seul niveau où une régression se voit.

**Files:**
- Create: `src/render/diff.hpp`, `src/render/diff.cpp`
- Create: `tests/test_diff.cpp`

**Interfaces:**
- Consumes: `Surface`, `Cell` (tâches 3-4) ; `OutputProfile`, `sgr_transition`, `encode_utf8` (tâche 5).
- Produces: `class sshos::Differ` — `explicit Differ(OutputProfile)`, `void invalidate()`, `std::string frame(const Surface& cur, std::optional<Pos> cursor)`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_diff.cpp` :

```cpp
#include <optional>
#include <string>

#include "harness.hpp"
#include "render/diff.hpp"
#include "render/surface.hpp"

using sshos::Color;
using sshos::Differ;
using sshos::OutputProfile;
using sshos::Pos;
using sshos::Style;
using sshos::Surface;

static OutputProfile tc() {
  return OutputProfile::detect("xterm-256color", "truecolor", true);
}

TEST(diff_first_frame_is_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;1H"));
}

// La propriété « bureau au repos = zéro octet ».
TEST(diff_emits_nothing_when_nothing_changed) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);
  CHECK_EQ(d.frame(s, std::nullopt), std::string(""));
}

TEST(diff_touches_only_the_changed_run) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  Style red;
  red.fg = Color::rgb(255, 0, 0);
  s.root().put(1, 0, U'X', red);
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;2H\033[38;2;255;0;0mX\033[1;1H"));
}

TEST(diff_skips_identical_rows) {
  Surface s(3, 2);
  s.root().text(0, 0, "abc", Style{});
  s.root().text(0, 1, "def", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().put(0, 1, U'Z', Style{});
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[2;1HZ\033[1;1H"));
}

// Règle 1 du §4.1 : un segment ne démarre JAMAIS sur une cellule de
// continuation. Ici seule la continuation change, et le diffeur doit
// remonter à la cellule de tête et réémettre la paire entière.
TEST(diff_never_starts_a_run_on_a_continuation_cell) {
  Surface s(3, 1);
  s.root().text(0, 0, "\xe6\x97\xa5" "a", Style{});  // 日a
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.at(1, 0).bg = Color::indexed(4);  // seule la continuation change
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\xe6\x97\xa5\033[1;1H"));
}

// Règle 3 du §4.1 : après un graphème non-ASCII, la position implicite du
// curseur n'est pas fiable — le run se termine et la reprise est absolue.
TEST(diff_reanchors_after_a_non_ascii_glyph) {
  Surface s(4, 1);
  Differ d(tc());
  d.frame(s, std::nullopt);

  s.root().text(0, 0, "\xe6\x97\xa5" "ab", Style{});  // 日ab
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1H\xe6\x97\xa5\033[1;3Hab\033[1;1H"));
}

TEST(diff_shows_the_cursor_only_when_asked) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  CHECK_EQ(d.frame(s, Pos{2, 0}),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;3H\033[?25h"));
}

TEST(diff_invalidate_forces_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);
  CHECK_EQ(d.frame(s, std::nullopt), std::string(""));
  d.invalidate();
  CHECK_EQ(d.frame(s, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habc\033[1;1H"));
}

TEST(diff_resize_forces_a_full_repaint) {
  Surface s(3, 1);
  s.root().text(0, 0, "abc", Style{});
  Differ d(tc());
  d.frame(s, std::nullopt);

  Surface bigger(4, 1);
  bigger.root().text(0, 0, "abcd", Style{});
  CHECK_EQ(d.frame(bigger, std::nullopt),
           std::string("\033[?25l\033[0m\033[1;1Habcd\033[1;1H"));
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `render/diff.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/render/diff.hpp` :

```cpp
#pragma once

#include <optional>
#include <string>

#include "render/profile.hpp"
#include "render/surface.hpp"

namespace sshos {

class Differ {
 public:
  explicit Differ(OutputProfile p) : profile_(p), prev_(0, 0) {}

  // Jette l'état supposé du terminal : la frame suivante est complète.
  void invalidate() { valid_ = false; }

  // Rend les octets à envoyer, ou une chaîne vide si rien n'a bougé.
  std::string frame(const Surface& cur, std::optional<Pos> cursor);

 private:
  OutputProfile profile_;
  Surface prev_;
  bool valid_ = false;
  // Suivi du curseur en champs séparés : un std::optional comparé à
  // lui-même ne distingue pas « pas encore de frame » de « curseur caché ».
  Pos last_target_{0, 0};
  bool last_shown_ = false;
  bool first_ = true;
};

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/render/diff.cpp` :

```cpp
#include "render/diff.hpp"

namespace sshos {
namespace {

std::string cup(int x, int y) {
  return "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

Style style_of(const Cell& c) { return Style{c.fg, c.bg, c.attrs}; }

}  // namespace

std::string Differ::frame(const Surface& cur, std::optional<Pos> cursor) {
  if (prev_.w() != cur.w() || prev_.h() != cur.h()) {
    prev_.resize(cur.w(), cur.h());
    valid_ = false;
  }

  const bool full = !valid_;
  bool any = false;
  std::string body;

  Style pen;          // état SGR courant, valable sur TOUTE la frame
  bool pos_known = false;
  int px = 0;
  int py = 0;

  for (int y = 0; y < cur.h(); ++y) {
    int x = 0;
    while (x < cur.w()) {
      const bool differs = full || !(cur.at(x, y) == prev_.at(x, y));
      if (!differs) {
        ++x;
        continue;
      }

      // Règle 1 : ne jamais démarrer sur une cellule de continuation, on
      // remonte à la cellule de tête et on réémet la paire entière.
      int start = x;
      while (start > 0 && cur.at(start, y).width == 0) --start;

      // L'extension part de la première cellule DIFFÉRENTE, pas de `start` :
      // partir de `start` ferait sortir immédiatement quand la cellule de
      // tête est identique, et le segment serait vide — boucle infinie.
      int end = x + 1;
      while (end < cur.w()) {
        const bool d = full || !(cur.at(end, y) == prev_.at(end, y));
        const bool cont = cur.at(end, y).width == 0;
        if (!d && !cont) break;
        ++end;
      }

      for (int c = start; c < end; ++c) {
        const Cell& cell = cur.at(c, y);
        if (cell.width == 0) continue;  // couverte par sa cellule de tête
        if (!pos_known || px != c || py != y) {
          body += cup(c, y);
          pos_known = true;
          px = c;
          py = y;
        }
        body += sgr_transition(pen, style_of(cell), profile_);
        pen = style_of(cell);
        body += encode_utf8(cell.ch);
        px += cell.width;
        // Règle 3 : après un graphème non-ASCII la position implicite du
        // curseur n'est pas fiable — la cellule suivante se réancre au CUP.
        if (cell.ch >= 0x80) pos_known = false;
      }

      any = true;
      x = end;
    }
  }

  const Pos target = cursor.value_or(Pos{0, 0});
  const bool shown = cursor.has_value();
  const bool cursor_changed =
      first_ || !(target == last_target_) || shown != last_shown_;
  if (!any && !full && !cursor_changed) return "";

  std::string out = "\033[?25l\033[0m";
  out += body;
  out += cup(target.x, target.y);
  if (cursor.has_value()) out += "\033[?25h";

  for (int y = 0; y < cur.h(); ++y) {
    for (int x = 0; x < cur.w(); ++x) prev_.at(x, y) = cur.at(x, y);
  }
  valid_ = true;
  last_target_ = target;
  last_shown_ = shown;
  first_ = false;
  return out;
}

}  // namespace sshos
```

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests diff
```

Attendu : `9 cas, 0 en echec`.

Le message d'échec montre les octets échappés (`\e[1;1H…`) : lire le diff caractère par caractère, la faute est presque toujours un `CUP` manquant ou en trop. Si `diff_emits_nothing_when_nothing_changed` échoue en rendant l'enveloppe seule, c'est que le court-circuit `!any && !full && !cursor_changed` est évalué trop tard.

- [ ] **Step 6: Commit**

```bash
git add src/render/diff.hpp src/render/diff.cpp tests/test_diff.cpp
git commit -m "feat(render): diffeur avec enveloppe de frame et regles de largeur"
```

---

### Task 7: Codec du protocole

**Files:**
- Create: `src/common/proto.hpp`, `src/common/proto.cpp`
- Create: `tests/test_proto.cpp`

**Interfaces:**
- Consumes: rien.
- Produces:
  - `struct sshos::Hello { uint32_t build_id; uint16_t cols, rows; std::string term, colorterm; bool utf8; std::vector<std::pair<std::string, std::string>> env; }`.
  - `struct sshos::Welcome {}`, `struct sshos::Incompatible { std::string reason; }`, `struct sshos::Detached { std::string reason; }`, `struct sshos::Input { std::string bytes; }`, `struct sshos::Resize { uint16_t cols, rows; }`, `struct sshos::FrameMsg { std::string ansi; }`.
  - `using sshos::Msg = std::variant<Hello, Welcome, Incompatible, Detached, Input, Resize, FrameMsg>`.
  - `std::string sshos::encode(const Msg&)`.
  - `class sshos::Decoder` — `void feed(std::string_view)`, `std::optional<Msg> next()`.
  - `constexpr uint32_t sshos::kBuildId`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_proto.cpp` :

```cpp
#include <string>
#include <variant>

#include "common/proto.hpp"
#include "harness.hpp"

using namespace sshos;

static Msg roundtrip(const Msg& m) {
  Decoder d;
  d.feed(encode(m));
  auto out = d.next();
  CHECK(out.has_value());
  return out.value();
}

TEST(proto_roundtrips_hello) {
  Hello h;
  h.build_id = 42;
  h.cols = 200;
  h.rows = 50;
  h.term = "xterm-256color";
  h.colorterm = "truecolor";
  h.utf8 = true;
  h.env.emplace_back("SSH_AUTH_SOCK", "/tmp/agent.1");
  const auto got = std::get<Hello>(roundtrip(h));
  CHECK_EQ(got.build_id, 42u);
  CHECK_EQ(static_cast<int>(got.cols), 200);
  CHECK_EQ(got.term, std::string("xterm-256color"));
  CHECK(got.utf8);
  CHECK_EQ(got.env.size(), static_cast<size_t>(1));
  CHECK_EQ(got.env[0].second, std::string("/tmp/agent.1"));
}

TEST(proto_roundtrips_simple_messages) {
  CHECK_EQ(std::get<Incompatible>(roundtrip(Incompatible{"vieux demon"})).reason,
           std::string("vieux demon"));
  CHECK_EQ(std::get<Input>(roundtrip(Input{"\033[A"})).bytes, std::string("\033[A"));
  CHECK_EQ(static_cast<int>(std::get<Resize>(roundtrip(Resize{80, 24})).rows), 24);
  CHECK_EQ(std::get<FrameMsg>(roundtrip(FrameMsg{"\033[1;1Hx"})).ansi,
           std::string("\033[1;1Hx"));
  CHECK(std::holds_alternative<Welcome>(roundtrip(Welcome{})));
}

// Le décodeur est nourri de morceaux arbitraires venant de read().
TEST(proto_decoder_survives_byte_by_byte_feeding) {
  const std::string wire = encode(Msg{Input{"hello"}}) + encode(Msg{Resize{80, 24}});
  Decoder d;
  int produced = 0;
  for (char c : wire) {
    d.feed(std::string_view(&c, 1));
    while (auto m = d.next()) ++produced;
  }
  CHECK_EQ(produced, 2);
}

TEST(proto_decoder_yields_nothing_on_partial_message) {
  const std::string wire = encode(Msg{Input{"hello"}});
  Decoder d;
  d.feed(std::string_view(wire).substr(0, wire.size() - 1));
  CHECK(!d.next().has_value());
  d.feed(std::string_view(wire).substr(wire.size() - 1));
  CHECK(d.next().has_value());
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `common/proto.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/common/proto.hpp` :

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sshos {

// Incrémenté à chaque changement incompatible du protocole. Comparé au
// handshake : mieux vaut un message clair qu'un affichage corrompu.
inline constexpr uint32_t kBuildId = 1;

struct Hello {
  uint32_t build_id = kBuildId;
  uint16_t cols = 0;
  uint16_t rows = 0;
  std::string term;
  std::string colorterm;
  bool utf8 = false;
  std::vector<std::pair<std::string, std::string>> env;
};

struct Welcome {};
struct Incompatible { std::string reason; };
struct Detached { std::string reason; };
struct Input { std::string bytes; };
struct Resize { uint16_t cols = 0; uint16_t rows = 0; };
struct FrameMsg { std::string ansi; };

using Msg = std::variant<Hello, Welcome, Incompatible, Detached, Input, Resize,
                         FrameMsg>;

std::string encode(const Msg& m);

// Décodeur incrémental : les messages arrivent découpés n'importe comment.
class Decoder {
 public:
  void feed(std::string_view bytes) { buf_.append(bytes); }
  std::optional<Msg> next();

 private:
  std::string buf_;
};

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/common/proto.cpp` :

```cpp
#include "common/proto.hpp"

#include <cstring>

namespace sshos {
namespace {

enum class Tag : uint8_t {
  Hello = 1, Welcome = 2, Incompatible = 3, Detached = 4,
  Input = 5, Resize = 6, Frame = 7,
};

void put_u8(std::string& o, uint8_t v) { o += static_cast<char>(v); }

void put_u16(std::string& o, uint16_t v) {
  put_u8(o, static_cast<uint8_t>(v >> 8));
  put_u8(o, static_cast<uint8_t>(v & 0xFF));
}

void put_u32(std::string& o, uint32_t v) {
  put_u16(o, static_cast<uint16_t>(v >> 16));
  put_u16(o, static_cast<uint16_t>(v & 0xFFFF));
}

void put_str(std::string& o, const std::string& s) {
  put_u32(o, static_cast<uint32_t>(s.size()));
  o += s;
}

struct Reader {
  std::string_view s;
  size_t i = 0;
  bool ok = true;

  uint8_t u8() {
    if (i + 1 > s.size()) { ok = false; return 0; }
    return static_cast<uint8_t>(s[i++]);
  }
  uint16_t u16() { const uint16_t hi = u8(); return static_cast<uint16_t>((hi << 8) | u8()); }
  uint32_t u32() { const uint32_t hi = u16(); return (hi << 16) | u16(); }
  std::string str() {
    const uint32_t n = u32();
    if (!ok || i + n > s.size()) { ok = false; return {}; }
    std::string out(s.substr(i, n));
    i += n;
    return out;
  }
};

std::string body_of(const Msg& m, Tag& tag) {
  std::string b;
  if (const auto* h = std::get_if<Hello>(&m)) {
    tag = Tag::Hello;
    put_u32(b, h->build_id);
    put_u16(b, h->cols);
    put_u16(b, h->rows);
    put_str(b, h->term);
    put_str(b, h->colorterm);
    put_u8(b, h->utf8 ? 1 : 0);
    put_u32(b, static_cast<uint32_t>(h->env.size()));
    for (const auto& [k, v] : h->env) { put_str(b, k); put_str(b, v); }
  } else if (std::get_if<Welcome>(&m) != nullptr) {
    tag = Tag::Welcome;
  } else if (const auto* x = std::get_if<Incompatible>(&m)) {
    tag = Tag::Incompatible;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Detached>(&m)) {
    tag = Tag::Detached;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Input>(&m)) {
    tag = Tag::Input;
    put_str(b, x->bytes);
  } else if (const auto* x = std::get_if<Resize>(&m)) {
    tag = Tag::Resize;
    put_u16(b, x->cols);
    put_u16(b, x->rows);
  } else {
    tag = Tag::Frame;
    put_str(b, std::get<FrameMsg>(m).ansi);
  }
  return b;
}

}  // namespace

std::string encode(const Msg& m) {
  Tag tag = Tag::Welcome;
  const std::string body = body_of(m, tag);
  std::string out;
  put_u8(out, static_cast<uint8_t>(tag));
  put_u32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

std::optional<Msg> Decoder::next() {
  if (buf_.size() < 5) return std::nullopt;

  Reader head{buf_, 0, true};
  const auto tag = static_cast<Tag>(head.u8());
  const uint32_t len = head.u32();
  if (buf_.size() < 5 + len) return std::nullopt;

  Reader r{std::string_view(buf_).substr(5, len), 0, true};
  std::optional<Msg> out;

  switch (tag) {
    case Tag::Hello: {
      Hello h;
      h.build_id = r.u32();
      h.cols = r.u16();
      h.rows = r.u16();
      h.term = r.str();
      h.colorterm = r.str();
      h.utf8 = r.u8() != 0;
      const uint32_t n = r.u32();
      for (uint32_t k = 0; k < n && r.ok; ++k) {
        std::string key = r.str();
        h.env.emplace_back(std::move(key), r.str());
      }
      out = Msg{std::move(h)};
      break;
    }
    case Tag::Welcome: out = Msg{Welcome{}}; break;
    case Tag::Incompatible: out = Msg{Incompatible{r.str()}}; break;
    case Tag::Detached: out = Msg{Detached{r.str()}}; break;
    case Tag::Input: out = Msg{Input{r.str()}}; break;
    case Tag::Resize: { Resize z; z.cols = r.u16(); z.rows = r.u16(); out = Msg{z}; break; }
    case Tag::Frame: out = Msg{FrameMsg{r.str()}}; break;
    default: break;  // tag inconnu : message consommé et ignoré
  }

  buf_.erase(0, 5 + len);
  if (!r.ok) return std::nullopt;
  return out;
}

}  // namespace sshos
```

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests proto
```

Attendu : `4 cas, 0 en echec`.

- [ ] **Step 6: Commit**

```bash
git add src/common/proto.hpp src/common/proto.cpp tests/test_proto.cpp
git commit -m "feat(common): codec du protocole et decodeur incremental"
```

---

### Task 8: Socket UNIX abstrait

Le `bind()` est le mutex : pas de fichier de verrou, pas de détection d'obsolescence, rien à voir disparaître sous un démon vivant (spec §3.3).

**Files:**
- Create: `src/common/net.hpp`, `src/common/net.cpp`
- Create: `tests/test_net.cpp`

**Interfaces:**
- Consumes: `Fd`, `set_cloexec`, `set_nonblock` (tâche 2).
- Produces:
  - `struct sshos::AddressInUse : std::runtime_error`.
  - `std::string sshos::read_boot_id()`.
  - `std::string sshos::socket_name(uid_t uid, std::string_view boot_id)`.
  - `Fd sshos::bind_abstract(std::string_view name)` — lève `AddressInUse` si `EADDRINUSE`, `std::system_error` sinon.
  - `Fd sshos::connect_abstract(std::string_view name)`.
  - `Fd sshos::accept_peer(int listen_fd, uid_t expected_uid)` — rend un `Fd` invalide si le pair a un autre uid.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_net.cpp` :

```cpp
#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "common/net.hpp"
#include "harness.hpp"

using sshos::Fd;

static std::string unique_name() {
  return "sshos-test/" + std::to_string(::getpid());
}

TEST(net_socket_name_is_stable) {
  CHECK_EQ(sshos::socket_name(1000, "abc"), std::string("sshos/1000/abc"));
}

TEST(net_bind_acts_as_a_mutex) {
  const std::string name = unique_name();
  Fd first = sshos::bind_abstract(name);
  CHECK(first.valid());

  bool threw = false;
  try {
    Fd second = sshos::bind_abstract(name);
  } catch (const sshos::AddressInUse&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_connect_reaches_the_listener_and_peer_uid_matches) {
  const std::string name = unique_name() + "-conn";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  CHECK(client.valid());

  Fd served = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(served.valid());
}

TEST(net_accept_rejects_a_foreign_uid) {
  const std::string name = unique_name() + "-uid";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  // Un uid qui n'est certainement pas le nôtre.
  Fd served = sshos::accept_peer(listener.get(), ::getuid() + 4242);
  CHECK(!served.valid());
}

TEST(net_connect_fails_when_nobody_listens) {
  bool threw = false;
  try {
    Fd f = sshos::connect_abstract(unique_name() + "-absent");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_boot_id_is_not_empty) {
  CHECK(!sshos::read_boot_id().empty());
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `common/net.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/common/net.hpp` :

```cpp
#pragma once

#include <sys/types.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "common/fd.hpp"

namespace sshos {

struct AddressInUse : std::runtime_error {
  AddressInUse() : std::runtime_error("adresse deja utilisee") {}
};

std::string read_boot_id();
std::string socket_name(uid_t uid, std::string_view boot_id);

// Adresse abstraite : sun_path[0] == '\0'. Rien sur le système de fichiers,
// donc rien à nettoyer et rien que logind puisse effacer.
Fd bind_abstract(std::string_view name);
Fd connect_abstract(std::string_view name);

// Une adresse abstraite n'a pas de permissions : tout processus de la
// machine peut s'y connecter. SO_PEERCRED est la seule barrière.
Fd accept_peer(int listen_fd, uid_t expected_uid);

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/common/net.cpp` :

```cpp
#include "common/net.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Rend la longueur d'adresse à passer à bind/connect. Pour une adresse
// abstraite elle s'arrête au dernier octet du nom : pas de terminateur.
socklen_t fill(sockaddr_un& addr, std::string_view name) {
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 > sizeof addr.sun_path) {
    throw std::runtime_error("nom de socket trop long");
  }
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

Fd make_socket() {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw_errno("socket");
  return Fd(fd);
}

}  // namespace

std::string read_boot_id() {
  std::ifstream in("/proc/sys/kernel/random/boot_id");
  std::string id;
  in >> id;
  if (id.empty()) id = "nobootid";
  return id;
}

std::string socket_name(uid_t uid, std::string_view boot_id) {
  return "sshos/" + std::to_string(uid) + "/" + std::string(boot_id);
}

Fd bind_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    if (errno == EADDRINUSE) throw AddressInUse();
    throw_errno("bind");
  }
  if (::listen(fd.get(), 4) != 0) throw_errno("listen");
  return fd;
}

Fd connect_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    throw_errno("connect");
  }
  return fd;
}

Fd accept_peer(int listen_fd, uid_t expected_uid) {
  const int raw = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (raw < 0) return Fd();
  Fd fd(raw);

  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return Fd();
  }
  if (cred.uid != expected_uid) return Fd();
  return fd;
}

}  // namespace sshos
```

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests net
```

Attendu : `6 cas, 0 en echec`.

Si `net_bind_acts_as_a_mutex` échoue en ne levant rien, vérifier que `fill` rend bien `offsetof(...) + 1 + name.size()` : une longueur égale à `sizeof(sockaddr_un)` fait que le noyau compare des octets nuls de bourrage et deux noms distincts entrent en collision.

- [ ] **Step 6: Commit**

```bash
git add src/common/net.hpp src/common/net.cpp tests/test_net.cpp
git commit -m "feat(common): socket UNIX abstrait, bind comme mutex, SO_PEERCRED"
```

---

### Task 9: Parseur d'entrée

Le seul composant qui voit les octets du clavier et de la souris. Tout le reste du système travaille sur des événements typés.

**Files:**
- Create: `src/input/events.hpp`
- Create: `src/input/parser.hpp`, `src/input/parser.cpp`
- Create: `tests/test_input.cpp`

**Interfaces:**
- Consumes: rien.
- Produces:
  - `enum class sshos::Key` : `None, Char, Enter, Tab, BackTab, Backspace, Escape, Up, Down, Left, Right, Home, End, PgUp, PgDn, Insert, Delete, F1..F12`.
  - `namespace sshos::mod` : `Shift = 1`, `Alt = 2`, `Ctrl = 4` (`uint8_t`).
  - `struct sshos::KeyEvent { Key key; char32_t ch; uint8_t mods; }`.
  - `enum class sshos::MouseAction { Press, Release, Motion, WheelUp, WheelDown }`.
  - `struct sshos::MouseEvent { MouseAction action; uint8_t button; int x, y; uint8_t mods; }` — coordonnées **0-indexées**.
  - `struct sshos::PasteEvent { std::string text; }`, `struct sshos::FocusEvent { bool focused; }`.
  - `using sshos::InputEvent = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent>`.
  - `class sshos::InputParser` — `void feed(std::string_view)`, `std::optional<InputEvent> next()`, `void timeout()`, `bool esc_pending() const`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_input.cpp` :

```cpp
#include <string>
#include <variant>
#include <vector>

#include "harness.hpp"
#include "input/parser.hpp"

using namespace sshos;

static std::vector<InputEvent> drain(InputParser& p, std::string_view bytes) {
  p.feed(bytes);
  std::vector<InputEvent> out;
  while (auto e = p.next()) out.push_back(*e);
  return out;
}

static KeyEvent one_key(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  return std::get<KeyEvent>(v.at(0));
}

static MouseEvent one_mouse(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  return std::get<MouseEvent>(v.at(0));
}

TEST(input_plain_characters) {
  const auto k = one_key("a");
  CHECK(k.key == Key::Char);
  CHECK_EQ(k.ch, U'a');
  CHECK_EQ(static_cast<int>(k.mods), 0);
}

TEST(input_control_characters) {
  const auto ctrl_a = one_key("\001");
  CHECK(ctrl_a.key == Key::Char);
  CHECK_EQ(ctrl_a.ch, U'a');
  CHECK_EQ(static_cast<int>(ctrl_a.mods), static_cast<int>(mod::Ctrl));

  CHECK(one_key("\r").key == Key::Enter);
  CHECK(one_key("\t").key == Key::Tab);
  CHECK(one_key("\177").key == Key::Backspace);
}

TEST(input_utf8_characters_wait_for_all_their_bytes) {
  InputParser p;
  CHECK(drain(p, "\xc3").empty());          // moitié de é
  const auto v = drain(p, "\xa9");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'é');
}

TEST(input_arrows_and_function_keys) {
  CHECK(one_key("\033[A").key == Key::Up);
  CHECK(one_key("\033[D").key == Key::Left);
  CHECK(one_key("\033[H").key == Key::Home);
  CHECK(one_key("\033[3~").key == Key::Delete);
  CHECK(one_key("\033[5~").key == Key::PgUp);
  CHECK(one_key("\033OP").key == Key::F1);
  CHECK(one_key("\033[15~").key == Key::F5);
  CHECK(one_key("\033[Z").key == Key::BackTab);
}

TEST(input_modified_arrows) {
  const auto k = one_key("\033[1;5A");  // Ctrl+Haut
  CHECK(k.key == Key::Up);
  CHECK_EQ(static_cast<int>(k.mods), static_cast<int>(mod::Ctrl));

  const auto s = one_key("\033[1;2C");  // Shift+Droite
  CHECK(s.key == Key::Right);
  CHECK_EQ(static_cast<int>(s.mods), static_cast<int>(mod::Shift));
}

// ESC isolé n'est décidable qu'au temps mort : c'est le préfixe de tout.
TEST(input_lone_escape_needs_the_timeout) {
  InputParser p;
  CHECK(drain(p, "\033").empty());
  CHECK(p.esc_pending());
  p.timeout();
  auto e = p.next();
  CHECK(e.has_value());
  CHECK(std::get<KeyEvent>(*e).key == Key::Escape);
  CHECK(!p.esc_pending());
}

TEST(input_escape_followed_by_a_letter_is_alt) {
  const auto k = one_key("\033a");
  CHECK(k.key == Key::Char);
  CHECK_EQ(k.ch, U'a');
  CHECK_EQ(static_cast<int>(k.mods), static_cast<int>(mod::Alt));
}

TEST(input_sgr_mouse_press_and_release) {
  const auto press = one_mouse("\033[<0;10;5M");
  CHECK(press.action == MouseAction::Press);
  CHECK_EQ(static_cast<int>(press.button), 0);
  CHECK_EQ(press.x, 9);   // 0-indexé
  CHECK_EQ(press.y, 4);

  const auto rel = one_mouse("\033[<0;10;5m");
  CHECK(rel.action == MouseAction::Release);
}

// Bit 32 = mouvement. Le mode 1002 le signale à chaque cellule franchie.
TEST(input_sgr_mouse_motion) {
  const auto m = one_mouse("\033[<32;10;5M");
  CHECK(m.action == MouseAction::Motion);
  CHECK_EQ(static_cast<int>(m.button), 0);
}

// Bit 64 = molette. Une molette n'émet JAMAIS de relâchement : la traiter
// comme un bouton verrouille la machine à états en glissement perpétuel.
TEST(input_wheel_is_not_a_button) {
  CHECK(one_mouse("\033[<64;1;1M").action == MouseAction::WheelUp);
  CHECK(one_mouse("\033[<65;1;1M").action == MouseAction::WheelDown);
}

TEST(input_mouse_modifier_bits) {
  const auto m = one_mouse("\033[<16;1;1M");  // +16 = Ctrl
  CHECK_EQ(static_cast<int>(m.mods), static_cast<int>(mod::Ctrl));
  const auto s = one_mouse("\033[<4;1;1M");   // +4 = Shift
  CHECK_EQ(static_cast<int>(s.mods), static_cast<int>(mod::Shift));
}

// Sans encadrement, chaque octet collé traverse le répartiteur de
// raccourcis : coller un transcript coloré tire des accords au hasard.
TEST(input_bracketed_paste_is_opaque) {
  InputParser p;
  const auto v = drain(p, "\033[200~\033[Ax\033[201~");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text, std::string("\033[Ax"));
}

TEST(input_incomplete_paste_yields_nothing) {
  InputParser p;
  CHECK(drain(p, "\033[200~abc").empty());
  const auto v = drain(p, "def\033[201~");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text, std::string("abcdef"));
}

TEST(input_focus_events) {
  InputParser p;
  const auto v = drain(p, "\033[I\033[O");
  CHECK_EQ(v.size(), static_cast<size_t>(2));
  CHECK(std::get<FocusEvent>(v.at(0)).focused);
  CHECK(!std::get<FocusEvent>(v.at(1)).focused);
}

// read() découpe où il veut : le parseur doit être insensible au découpage.
TEST(input_is_insensitive_to_chunk_boundaries) {
  const std::string wire = "\033[<0;10;5Mabc\033[1;5A\033[200~xy\033[201~";
  InputParser whole;
  const auto expected = drain(whole, wire);

  InputParser piecewise;
  std::vector<InputEvent> got;
  for (char c : wire) {
    piecewise.feed(std::string_view(&c, 1));
    while (auto e = piecewise.next()) got.push_back(*e);
  }
  CHECK_EQ(got.size(), expected.size());
  CHECK_EQ(got.size(), static_cast<size_t>(6));
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `input/parser.hpp: No such file or directory`.

- [ ] **Step 3: Écrire `events.hpp`**

Créer `src/input/events.hpp` :

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace sshos {

enum class Key : uint16_t {
  None, Char, Enter, Tab, BackTab, Backspace, Escape,
  Up, Down, Left, Right, Home, End, PgUp, PgDn, Insert, Delete,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

namespace mod {
inline constexpr uint8_t Shift = 1;
inline constexpr uint8_t Alt = 2;
inline constexpr uint8_t Ctrl = 4;
}  // namespace mod

struct KeyEvent {
  Key key = Key::None;
  char32_t ch = 0;   // valable seulement si key == Key::Char
  uint8_t mods = 0;
};

enum class MouseAction { Press, Release, Motion, WheelUp, WheelDown };

struct MouseEvent {
  MouseAction action = MouseAction::Press;
  uint8_t button = 0;
  int x = 0;  // 0-indexé
  int y = 0;
  uint8_t mods = 0;
};

struct PasteEvent { std::string text; };
struct FocusEvent { bool focused = false; };

using InputEvent = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent>;

}  // namespace sshos
```

- [ ] **Step 4: Écrire `parser.hpp`**

Créer `src/input/parser.hpp` :

```cpp
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "input/events.hpp"

namespace sshos {

class InputParser {
 public:
  void feed(std::string_view bytes) {
    buf_.append(bytes);
    pump();
  }

  std::optional<InputEvent> next() {
    if (ready_.empty()) return std::nullopt;
    InputEvent e = std::move(ready_.front());
    ready_.pop_front();
    return e;
  }

  // Appelé quand le délai d'ambiguïté a expiré sans octet supplémentaire.
  void timeout();

  bool esc_pending() const { return esc_pending_; }

 private:
  void pump();
  // Rend le nombre d'octets consommés, ou 0 si la séquence est incomplète.
  size_t step();

  std::string buf_;
  std::deque<InputEvent> ready_;
  bool in_paste_ = false;
  bool esc_pending_ = false;
};

}  // namespace sshos
```

- [ ] **Step 5: Écrire `parser.cpp`**

Créer `src/input/parser.cpp` :

```cpp
#include "input/parser.hpp"

#include <cstdlib>
#include <vector>

namespace sshos {
namespace {

constexpr std::string_view kPasteEnd = "\033[201~";

Key key_from_tilde(int n) {
  switch (n) {
    case 1: return Key::Home;
    case 2: return Key::Insert;
    case 3: return Key::Delete;
    case 4: return Key::End;
    case 5: return Key::PgUp;
    case 6: return Key::PgDn;
    case 15: return Key::F5;
    case 17: return Key::F6;
    case 18: return Key::F7;
    case 19: return Key::F8;
    case 20: return Key::F9;
    case 21: return Key::F10;
    case 23: return Key::F11;
    case 24: return Key::F12;
    default: return Key::None;
  }
}

Key key_from_final(char f) {
  switch (f) {
    case 'A': return Key::Up;
    case 'B': return Key::Down;
    case 'C': return Key::Right;
    case 'D': return Key::Left;
    case 'H': return Key::Home;
    case 'F': return Key::End;
    case 'Z': return Key::BackTab;
    case 'P': return Key::F1;
    case 'Q': return Key::F2;
    case 'R': return Key::F3;
    case 'S': return Key::F4;
    default: return Key::None;
  }
}

std::vector<int> split_params(std::string_view s) {
  std::vector<int> out;
  int cur = -1;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
    } else if (c == ';') {
      out.push_back(cur);
      cur = -1;
    }
  }
  out.push_back(cur);
  return out;
}

// La valeur du paramètre de modificateur est 1 + un masque dont les bits
// sont, dans l'ordre, Shift, Alt, Ctrl — exactement la disposition de mod::.
uint8_t mods_from_param(int p) {
  if (p <= 1) return 0;
  return static_cast<uint8_t>((p - 1) & 0x07);
}

// Nombre d'octets attendus pour une séquence UTF-8 commençant par b0.
int utf8_len(unsigned char b0) {
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0) == 0xC0) return 2;
  if ((b0 & 0xF0) == 0xE0) return 3;
  if ((b0 & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

void InputParser::timeout() {
  if (esc_pending_ && buf_.size() == 1 && buf_[0] == '\033') {
    buf_.clear();
    ready_.push_back(InputEvent{KeyEvent{Key::Escape, 0, 0}});
  }
  esc_pending_ = false;
}

void InputParser::pump() {
  while (!buf_.empty()) {
    const size_t used = step();
    if (used == 0) return;  // séquence incomplète : attendre d'autres octets
    buf_.erase(0, used);
  }
}

size_t InputParser::step() {
  esc_pending_ = false;

  if (in_paste_) {
    const size_t end = buf_.find(kPasteEnd);
    if (end == std::string::npos) return 0;
    ready_.push_back(InputEvent{PasteEvent{buf_.substr(0, end)}});
    in_paste_ = false;
    return end + kPasteEnd.size();
  }

  const auto b0 = static_cast<unsigned char>(buf_[0]);

  if (b0 != 0x1b) {
    if (b0 == '\r' || b0 == '\n') {
      ready_.push_back(InputEvent{KeyEvent{Key::Enter, 0, 0}});
      return 1;
    }
    if (b0 == '\t') {
      ready_.push_back(InputEvent{KeyEvent{Key::Tab, 0, 0}});
      return 1;
    }
    if (b0 == 0x7f || b0 == 0x08) {
      ready_.push_back(InputEvent{KeyEvent{Key::Backspace, 0, 0}});
      return 1;
    }
    if (b0 < 0x20) {
      const char32_t letter = (b0 == 0) ? U' ' : static_cast<char32_t>(U'a' + b0 - 1);
      ready_.push_back(InputEvent{KeyEvent{Key::Char, letter, mod::Ctrl}});
      return 1;
    }
    const int need = utf8_len(b0);
    if (buf_.size() < static_cast<size_t>(need)) return 0;
    char32_t cp = b0;
    if (need > 1) {
      cp = b0 & (0xFF >> (need + 1));
      for (int k = 1; k < need; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(buf_[k]) & 0x3F);
      }
    }
    ready_.push_back(InputEvent{KeyEvent{Key::Char, cp, 0}});
    return static_cast<size_t>(need);
  }

  // ESC seul : indécidable tant qu'aucun octet ne suit.
  if (buf_.size() == 1) {
    esc_pending_ = true;
    return 0;
  }

  if (buf_[1] == 'O') {
    if (buf_.size() < 3) return 0;
    const Key k = key_from_final(buf_[2]);
    if (k != Key::None) ready_.push_back(InputEvent{KeyEvent{k, 0, 0}});
    return 3;
  }

  if (buf_[1] != '[') {
    // ESC + octet : accord Alt.
    const auto b1 = static_cast<unsigned char>(buf_[1]);
    if (b1 >= 0x20 && b1 < 0x7f) {
      ready_.push_back(
          InputEvent{KeyEvent{Key::Char, static_cast<char32_t>(b1), mod::Alt}});
    }
    return 2;
  }

  // CSI : paramètres jusqu'à un octet final dans 0x40..0x7E.
  size_t i = 2;
  while (i < buf_.size()) {
    const auto c = static_cast<unsigned char>(buf_[i]);
    if (c >= 0x40 && c <= 0x7E) break;
    ++i;
  }
  if (i >= buf_.size()) return 0;  // final pas encore arrivé

  const char final_byte = buf_[i];
  const std::string_view params(buf_.data() + 2, i - 2);
  const size_t used = i + 1;

  if (!params.empty() && params[0] == '<') {
    const auto p = split_params(params.substr(1));
    if (p.size() < 3) return used;
    const int cb = p[0];
    MouseEvent m;
    m.x = p[1] - 1;
    m.y = p[2] - 1;
    if ((cb & 4) != 0) m.mods |= mod::Shift;
    if ((cb & 8) != 0) m.mods |= mod::Alt;
    if ((cb & 16) != 0) m.mods |= mod::Ctrl;

    if ((cb & 64) != 0) {
      // Molette : jamais de relâchement, donc hors machine à états.
      m.action = (cb & 1) != 0 ? MouseAction::WheelDown : MouseAction::WheelUp;
    } else if ((cb & 32) != 0) {
      m.action = MouseAction::Motion;
      m.button = static_cast<uint8_t>(cb & 3);
    } else {
      m.action = final_byte == 'm' ? MouseAction::Release : MouseAction::Press;
      m.button = static_cast<uint8_t>(cb & 3);
    }
    ready_.push_back(InputEvent{m});
    return used;
  }

  if (final_byte == 'I' || final_byte == 'O') {
    ready_.push_back(InputEvent{FocusEvent{final_byte == 'I'}});
    return used;
  }

  const auto p = split_params(params);

  if (final_byte == '~') {
    const int n = p.empty() || p[0] < 0 ? 0 : p[0];
    if (n == 200) {
      in_paste_ = true;
      return used;
    }
    const Key k = key_from_tilde(n);
    if (k != Key::None) {
      ready_.push_back(
          InputEvent{KeyEvent{k, 0, p.size() > 1 ? mods_from_param(p[1]) : uint8_t{0}}});
    }
    return used;
  }

  const Key k = key_from_final(final_byte);
  if (k != Key::None) {
    ready_.push_back(
        InputEvent{KeyEvent{k, 0, p.size() > 1 ? mods_from_param(p[1]) : uint8_t{0}}});
  }
  return used;
}

}  // namespace sshos
```

- [ ] **Step 6: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests input
```

Attendu : `15 cas, 0 en echec`.

Si `input_is_insensitive_to_chunk_boundaries` échoue, la cause est presque toujours un `step()` qui rend un nombre d'octets non nul sur une séquence incomplète : chaque chemin « pas assez d'octets » doit rendre exactement `0`.

- [ ] **Step 7: Commit**

```bash
git add src/input/events.hpp src/input/parser.hpp src/input/parser.cpp tests/test_input.cpp
git commit -m "feat(input): parseur clavier, souris SGR, collage encadre, focus"
```

---

### Task 10: `TtyGuard` et boucle client

Le client fait trois choses : mettre le terminal en mode brut, relayer des octets, remettre le terminal comme il l'a trouvé. La troisième est celle qui doit marcher **même quand tout le reste plante**.

**Files:**
- Create: `src/client/tty_guard.hpp`, `src/client/tty_guard.cpp`
- Create: `src/client/client.hpp`, `src/client/client.cpp`
- Create: `tests/test_tty.cpp`

**Interfaces:**
- Consumes: `Fd` (tâche 2) ; `Decoder`, `encode`, `Msg` (tâche 7) ; `connect_abstract`, `socket_name`, `read_boot_id` (tâche 8).
- Produces:
  - `std::string sshos::tty_setup_sequence()` et `std::string sshos::tty_restore_sequence()` — pures, testables aux octets près.
  - `class sshos::TtyGuard` — constructeur : `tcgetattr` puis mode brut puis écriture de `tty_setup_sequence()`. Destructeur : écriture de `tty_restore_sequence()` puis `tcsetattr`. Non copiable, non déplaçable.
  - `int sshos::run_client(std::string_view socket_name)` — code de retour du processus.
  - `std::vector<std::pair<std::string, std::string>> sshos::collect_env_delta()`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_tty.cpp` :

```cpp
#include <cstdlib>
#include <string>

#include "client/tty_guard.hpp"
#include "harness.hpp"

TEST(tty_setup_sequence_is_exact) {
  CHECK_EQ(sshos::tty_setup_sequence(),
           std::string("\033[?1049h"    // écran alterné
                       "\033[?1002h"    // souris : boutons + glissement
                       "\033[?1006h"    // encodage SGR
                       "\033[?2004h"    // collage encadré
                       "\033[?1004h"    // rapports de focus
                       "\033[?7l"));    // pas de repli automatique
}

// Le miroir exact, dans l'ordre inverse. Un mode oublié ici, c'est un
// terminal inutilisable après un plantage.
TEST(tty_restore_sequence_is_the_mirror) {
  CHECK_EQ(sshos::tty_restore_sequence(),
           std::string("\033[?25h"
                       "\033[?7h"
                       "\033[?1004l"
                       "\033[?2004l"
                       "\033[?1006l"
                       "\033[?1002l"
                       "\033[?1049l"));
}

TEST(tty_every_mode_set_is_also_unset) {
  const std::string on = sshos::tty_setup_sequence();
  const std::string off = sshos::tty_restore_sequence();
  for (const char* m : {"1049", "1002", "1006", "2004", "1004", "7"}) {
    const std::string set = std::string("\033[?") + m + "h";
    const std::string unset = std::string("\033[?") + m + "l";
    if (on.find(set) != std::string::npos) {
      CHECK(off.find(unset) != std::string::npos);
    }
  }
}

TEST(env_delta_carries_the_ssh_variables_that_exist) {
  ::setenv("SSH_AUTH_SOCK", "/tmp/agent.test", 1);
  ::unsetenv("SSH_TTY");
  const auto d = sshos::collect_env_delta();
  bool found_auth = false;
  bool found_tty = false;
  for (const auto& [k, v] : d) {
    if (k == "SSH_AUTH_SOCK") {
      found_auth = true;
      CHECK_EQ(v, std::string("/tmp/agent.test"));
    }
    if (k == "SSH_TTY") found_tty = true;
  }
  CHECK(found_auth);
  CHECK(!found_tty);  // une variable absente n'est pas transmise vide
  ::unsetenv("SSH_AUTH_SOCK");
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `client/tty_guard.hpp: No such file or directory`.

- [ ] **Step 3: Écrire `tty_guard.hpp`**

Créer `src/client/tty_guard.hpp` :

```cpp
#pragma once

#include <termios.h>

#include <string>
#include <utility>
#include <vector>

namespace sshos {

std::string tty_setup_sequence();
std::string tty_restore_sequence();

// Variables que le démon doit rafraîchir à chaque attache, appliquées
// uniquement aux NOUVEAUX enfants (modèle update-environment de tmux) :
// réécrire l'environnement d'un shell déjà lancé est impossible, et
// fossiliser SSH_AUTH_SOCK fait réclamer une passphrase à vie.
std::vector<std::pair<std::string, std::string>> collect_env_delta();

// RAII : le destructeur remet le terminal en état. Non déplaçable — un
// second propriétaire restaurerait deux fois, dont une trop tôt.
class TtyGuard {
 public:
  explicit TtyGuard(int fd);
  ~TtyGuard();

  TtyGuard(const TtyGuard&) = delete;
  TtyGuard& operator=(const TtyGuard&) = delete;
  TtyGuard(TtyGuard&&) = delete;
  TtyGuard& operator=(TtyGuard&&) = delete;

  // Filet de sécurité pour les signaux fatals : installe des gestionnaires
  // qui restaurent puis relancent le signal avec la disposition par défaut.
  static void install_crash_handlers();

 private:
  int fd_;
  termios saved_{};
  bool armed_ = false;
};

}  // namespace sshos
```

- [ ] **Step 4: Écrire `tty_guard.cpp`**

Créer `src/client/tty_guard.cpp` :

```cpp
#include "client/tty_guard.hpp"

#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>

namespace sshos {
namespace {

int g_crash_fd = -1;

void write_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

extern "C" void on_fatal(int sig) {
  if (g_crash_fd >= 0) {
    const std::string s = tty_restore_sequence();
    ::write(g_crash_fd, s.data(), s.size());
  }
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
  g_crash_fd = fd_;
  write_all(fd_, tty_setup_sequence());
}

TtyGuard::~TtyGuard() {
  if (!armed_) return;
  write_all(fd_, tty_restore_sequence());
  ::tcsetattr(fd_, TCSANOW, &saved_);
  g_crash_fd = -1;
}

void TtyGuard::install_crash_handlers() {
  for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTERM, SIGINT}) {
    ::signal(sig, on_fatal);
  }
}

}  // namespace sshos
```

- [ ] **Step 5: Écrire la boucle client**

Créer `src/client/client.hpp` :

```cpp
#pragma once

#include <string_view>

namespace sshos {

// Se connecte, fait le handshake, relaie jusqu'à Detached ou fermeture.
// Rend le code de retour du processus.
int run_client(std::string_view socket_name);

}  // namespace sshos
```

Créer `src/client/client.cpp` :

```cpp
#include "client/client.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

#include "client/tty_guard.hpp"
#include "common/net.hpp"
#include "common/proto.hpp"
#include "render/cell.hpp"  // Size

namespace sshos {
namespace {

volatile sig_atomic_t g_winch = 0;

extern "C" void on_winch(int) { g_winch = 1; }

Size term_size(int fd) {
  winsize ws{};
  if (::ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return Size{80, 24};
  return Size{ws.ws_col, ws.ws_row};
}

bool write_all(int fd, std::string_view s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int run_client(std::string_view socket_name) {
  Fd sock;
  try {
    sock = connect_abstract(socket_name);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sshos: connexion au demon impossible : %s\n", e.what());
    return 1;
  }

  TtyGuard::install_crash_handlers();
  TtyGuard guard(STDIN_FILENO);
  ::signal(SIGWINCH, on_winch);

  const Size sz = term_size(STDIN_FILENO);
  Hello hello;
  hello.cols = static_cast<uint16_t>(sz.w);
  hello.rows = static_cast<uint16_t>(sz.h);
  if (const char* t = std::getenv("TERM")) hello.term = t;
  if (const char* c = std::getenv("COLORTERM")) hello.colorterm = c;
  const char* lang = std::getenv("LC_ALL");
  if (lang == nullptr) lang = std::getenv("LANG");
  hello.utf8 = lang != nullptr && std::string(lang).find("UTF-8") != std::string::npos;
  hello.env = collect_env_delta();
  if (!write_all(sock.get(), encode(Msg{hello}))) return 1;

  Decoder dec;
  std::string in_buf(65536, '\0');
  int rc = 0;

  for (;;) {
    if (g_winch != 0) {
      g_winch = 0;
      const Size s = term_size(STDIN_FILENO);
      Resize r;
      r.cols = static_cast<uint16_t>(s.w);
      r.rows = static_cast<uint16_t>(s.h);
      if (!write_all(sock.get(), encode(Msg{r}))) break;
    }

    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sock.get(), POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      const ssize_t n = ::read(STDIN_FILENO, in_buf.data(), in_buf.size());
      if (n <= 0) break;
      if (!write_all(sock.get(), encode(Msg{Input{in_buf.substr(0, static_cast<size_t>(n))}}))) break;
    }

    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t n = ::read(sock.get(), in_buf.data(), in_buf.size());
      if (n <= 0) break;
      dec.feed(std::string_view(in_buf.data(), static_cast<size_t>(n)));
      bool stop = false;
      while (auto m = dec.next()) {
        if (const auto* f = std::get_if<FrameMsg>(&*m)) {
          if (!write_all(STDOUT_FILENO, f->ansi)) { stop = true; break; }
        } else if (const auto* d = std::get_if<Detached>(&*m)) {
          std::fprintf(stderr, "\r\nsshos: detache (%s)\r\n", d->reason.c_str());
          stop = true;
          break;
        } else if (const auto* i = std::get_if<Incompatible>(&*m)) {
          std::fprintf(stderr, "\r\nsshos: %s\r\n", i->reason.c_str());
          rc = 1;
          stop = true;
          break;
        }
      }
      if (stop) break;
    }
  }
  return rc;
}

}  // namespace sshos
```

`Size` vient de `render/cell.hpp` : c'est la seule chose que le client emprunte au moteur de rendu, tout le reste lui arrive déjà encodé en ANSI.

- [ ] **Step 6: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests tty
```

Attendu : `4 cas, 0 en echec`.

- [ ] **Step 7: Commit**

```bash
git add src/client/ tests/test_tty.cpp
git commit -m "feat(client): TtyGuard reversible, boucle d'attache, delta d'environnement"
```

---

### Task 11: Détachement du démon

**Files:**
- Create: `src/daemon/daemonize.hpp`, `src/daemon/daemonize.cpp`
- Create: `tests/test_daemonize.cpp`

**Interfaces:**
- Consumes: rien.
- Produces:
  - `pid_t sshos::spawn_detached(const std::vector<std::string>& argv)` — rend le pid de l'enfant **intermédiaire**, que l'appelant doit récolter avec `waitpid`. Le petit-enfant est chef de session, réparenté, sans terminal de contrôle.
  - `void sshos::become_daemon()` — appelée par le processus déjà détaché, en tête de `--daemon` : `chdir("/")`, redirection de 0/1/2 vers `/dev/null`, assainissement des signaux.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_daemonize.cpp` :

```cpp
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "daemon/daemonize.hpp"
#include "harness.hpp"

static std::string tmp_path(const char* suffix) {
  return "/tmp/sshos-test-" + std::to_string(::getpid()) + "-" + suffix;
}

static bool wait_for_file(const std::string& path, int tries) {
  for (int i = 0; i < tries; ++i) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) return true;
    ::usleep(20 * 1000);
  }
  return false;
}

// Trois propriétés en un test : l'intermédiaire meurt tout de suite, le
// petit-enfant vit après sa mort, et son parent n'est plus nous.
TEST(daemonize_detaches_the_grandchild) {
  const std::string marker = tmp_path("ppid");
  ::unlink(marker.c_str());

  const pid_t mid = sshos::spawn_detached(
      {"/bin/sh", "-c", "sleep 0.15; echo $PPID > " + marker});
  CHECK(mid > 0);

  int status = 0;
  const pid_t reaped = ::waitpid(mid, &status, 0);
  CHECK_EQ(reaped, mid);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);

  // Le fichier n'existe pas encore : le petit-enfant dort toujours.
  struct stat st {};
  CHECK_EQ(::stat(marker.c_str(), &st), -1);

  CHECK(wait_for_file(marker, 100));

  std::ifstream in(marker);
  pid_t recorded = 0;
  in >> recorded;
  CHECK(recorded != ::getpid());
  ::unlink(marker.c_str());
}

// Le masque de signaux survit à execve : un SIGCHLD encore bloqué casse
// tout enfant qui attend ses propres processus (make -j8, par exemple).
TEST(daemonize_clears_the_signal_mask_before_exec) {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGCHLD);
  sigaddset(&block, SIGUSR1);
  ::sigprocmask(SIG_BLOCK, &block, nullptr);

  const std::string marker = tmp_path("sigmask");
  ::unlink(marker.c_str());
  const pid_t mid = sshos::spawn_detached(
      {"/bin/sh", "-c", "grep SigBlk /proc/self/status > " + marker});
  int status = 0;
  ::waitpid(mid, &status, 0);
  CHECK(wait_for_file(marker, 100));

  std::ifstream in(marker);
  std::string label;
  std::string mask;
  in >> label >> mask;
  CHECK_EQ(label, std::string("SigBlk:"));
  CHECK_EQ(mask, std::string("0000000000000000"));

  ::sigprocmask(SIG_UNBLOCK, &block, nullptr);
  ::unlink(marker.c_str());
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `daemon/daemonize.hpp: No such file or directory`.

- [ ] **Step 3: Écrire l'en-tête**

Créer `src/daemon/daemonize.hpp` :

```cpp
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace sshos {

// Double fork + setsid : le petit-enfant est chef de session, sans
// terminal de contrôle, réparenté à init. Aucun PR_SET_PDEATHSIG — il
// tuerait le démon à la mort du client, ce qui est exactement l'inverse
// de la fonctionnalité recherchée.
//
// Rend le pid de l'enfant intermédiaire ; l'appelant DOIT le récolter,
// sinon il reste zombie.
pid_t spawn_detached(const std::vector<std::string>& argv);

// À appeler en tête du mode --daemon, dans le processus déjà détaché.
void become_daemon();

}  // namespace sshos
```

- [ ] **Step 4: Écrire l'implémentation**

Créer `src/daemon/daemonize.cpp` :

```cpp
#include "daemon/daemonize.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace sshos {
namespace {

void reset_signal_state() {
  // Le masque survit à execve : le laisser en place casse tout enfant qui
  // attend ses propres processus.
  sigset_t empty;
  sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);

  // Les dispositions SIG_IGN survivent aussi : un SIGPIPE ignoré hérité
  // fait que `yes | head -1` ne s'arrête jamais.
  for (int sig = 1; sig < NSIG; ++sig) {
    if (sig == SIGKILL || sig == SIGSTOP) continue;
    ::signal(sig, SIG_DFL);
  }
}

void redirect_std_to_devnull() {
  const int null_fd = ::open("/dev/null", O_RDWR);
  if (null_fd < 0) return;
  ::dup2(null_fd, STDIN_FILENO);
  ::dup2(null_fd, STDOUT_FILENO);
  ::dup2(null_fd, STDERR_FILENO);
  if (null_fd > STDERR_FILENO) ::close(null_fd);
}

}  // namespace

pid_t spawn_detached(const std::vector<std::string>& argv) {
  const pid_t first = ::fork();
  if (first != 0) return first;  // parent : rend le pid à récolter

  // Enfant intermédiaire.
  if (::setsid() == static_cast<pid_t>(-1)) ::_exit(127);

  const pid_t second = ::fork();
  if (second != 0) ::_exit(second < 0 ? 127 : 0);

  // Petit-enfant : le démon.
  if (::chdir("/") != 0) ::_exit(127);
  redirect_std_to_devnull();
  reset_signal_state();
  ::signal(SIGHUP, SIG_IGN);

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const auto& s : argv) raw.push_back(const_cast<char*>(s.c_str()));
  raw.push_back(nullptr);

  ::execv(raw[0], raw.data());
  ::_exit(127);
}

void become_daemon() {
  if (::chdir("/") != 0) { /* sans conséquence si le cwd est déjà valide */ }
  redirect_std_to_devnull();
  ::signal(SIGHUP, SIG_IGN);
  ::signal(SIGPIPE, SIG_IGN);  // un client qui meurt ne doit pas tuer le démon
}

}  // namespace sshos
```

Le démon réel est lancé par `spawn_detached({"/proc/self/exe", "--daemon"})` : `/proc/self/exe` est le seul chemin fiable vers le binaire courant — `argv[0]` peut être un nom relatif dont le `cwd` vient d'être remplacé par `/`.

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests daemonize
```

Attendu : `2 cas, 0 en echec`.

Ces tests créent des fichiers dans `/tmp` nommés d'après le pid et les suppriment ; un échec en cours de route peut en laisser traîner, sans conséquence.

- [ ] **Step 6: Commit**

```bash
git add src/daemon/daemonize.hpp src/daemon/daemonize.cpp tests/test_daemonize.cpp
git commit -m "feat(daemon): detachement par double fork et assainissement des signaux"
```

---

### Task 12: Contre-pression et cadence de rendu

Deux petites machines dont dépend la tenue du démon sous charge : une file de sortie qui ne bloque jamais, et une horloge qui plafonne le rendu à 30 fps.

**Files:**
- Create: `src/common/outqueue.hpp`, `src/common/outqueue.cpp`
- Create: `src/common/frameclock.hpp`
- Create: `tests/test_outqueue.cpp`

**Interfaces:**
- Consumes: `set_nonblock` (tâche 2).
- Produces:
  - `class sshos::OutQueue` — `explicit OutQueue(size_t ceiling)`, `void push(std::string_view)`, `bool flush(int fd)` (rend `false` si le pair est mort), `bool wants_write() const`, `size_t size() const`, `bool take_overflow()`.
  - `class sshos::FrameClock` — `explicit FrameClock(std::chrono::milliseconds)`, `void mark_dirty()`, `bool dirty() const`, `int delay_ms(Clock::time_point) const`, `void note_render(Clock::time_point)`, avec `using Clock = std::chrono::steady_clock`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_outqueue.cpp` :

```cpp
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

#include "common/fd.hpp"
#include "common/frameclock.hpp"
#include "common/outqueue.hpp"
#include "harness.hpp"

using sshos::FrameClock;
using sshos::OutQueue;

namespace {

struct Pair {
  sshos::Fd a;
  sshos::Fd b;
};

Pair make_pair() {
  int sv[2] = {-1, -1};
  ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  sshos::set_nonblock(sv[0]);
  return Pair{sshos::Fd(sv[0]), sshos::Fd(sv[1])};
}

}  // namespace

TEST(outqueue_drains_completely_when_the_socket_accepts) {
  Pair p = make_pair();
  OutQueue q(1 << 20);
  q.push("hello");
  CHECK(q.flush(p.a.get()));
  CHECK(!q.wants_write());
  CHECK_EQ(q.size(), static_cast<size_t>(0));

  char buf[16] = {};
  CHECK_EQ(::read(p.b.get(), buf, sizeof buf), static_cast<ssize_t>(5));
  CHECK_EQ(std::string(buf, 5), std::string("hello"));
}

// Personne ne lit : le tampon noyau se remplit, EAGAIN arrive, et le reste
// doit rester en file — pas d'écriture bloquante, jamais.
TEST(outqueue_keeps_the_remainder_and_asks_for_epollout) {
  Pair p = make_pair();
  OutQueue q(8 << 20);
  q.push(std::string(4 << 20, 'x'));
  CHECK(q.flush(p.a.get()));
  CHECK(q.wants_write());
  CHECK(q.size() > 0);
}

// Au-delà du plafond on jette TOUT : garder un préfixe produirait un diff
// appliqué sur un écran qui n'est pas celui qu'il suppose.
TEST(outqueue_drops_the_whole_queue_past_the_ceiling) {
  OutQueue q(4096);
  q.push(std::string(5000, 'x'));
  CHECK(q.take_overflow());
  CHECK_EQ(q.size(), static_cast<size_t>(0));
  CHECK(!q.take_overflow());  // le drapeau se consomme
}

TEST(outqueue_reports_a_dead_peer) {
  Pair p = make_pair();
  p.b.reset();  // le pair ferme
  OutQueue q(1 << 20);
  q.push(std::string(1 << 16, 'y'));
  CHECK(!q.flush(p.a.get()));
}

TEST(frameclock_renders_immediately_then_throttles) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  CHECK_EQ(fc.delay_ms(t0), -1);  // rien à faire
  fc.mark_dirty();
  CHECK_EQ(fc.delay_ms(t0), 0);   // premier rendu : tout de suite
  fc.note_render(t0);
  CHECK_EQ(fc.delay_ms(t0), -1);

  fc.mark_dirty();
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(10)), 23);
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(40)), 0);
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `common/outqueue.hpp: No such file or directory`.

- [ ] **Step 3: Écrire `outqueue.hpp` et `outqueue.cpp`**

Créer `src/common/outqueue.hpp` :

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sshos {

// File de sortie non bloquante. Le démon n'écrit JAMAIS en bloquant : un
// client sur une liaison lente gèlerait tout le bureau, y compris les
// processus des autres fenêtres.
class OutQueue {
 public:
  explicit OutQueue(size_t ceiling) : ceiling_(ceiling) {}

  void push(std::string_view bytes);

  // Rend false si le pair est mort. EAGAIN n'est pas une erreur.
  bool flush(int fd);

  // Vrai tant qu'il reste des octets : EPOLLOUT doit être ARMÉ. Le
  // désarmer dès que la file est vide, sinon un epoll niveau-déclenché
  // tourne à 100 % de CPU sur un socket en permanence inscriptible.
  bool wants_write() const { return off_ < buf_.size(); }

  size_t size() const { return buf_.size() - off_; }

  // Vrai une seule fois après un dépassement : l'appelant doit alors
  // invalider la frame précédente et repartir sur un repaint complet.
  bool take_overflow();

 private:
  void compact();

  std::string buf_;
  size_t off_ = 0;
  size_t ceiling_;
  bool overflowed_ = false;
};

}  // namespace sshos
```

Créer `src/common/outqueue.cpp` :

```cpp
#include "common/outqueue.hpp"

#include <sys/socket.h>

#include <cerrno>

namespace sshos {

void OutQueue::push(std::string_view bytes) {
  buf_.append(bytes);
  if (size() > ceiling_) {
    buf_.clear();
    off_ = 0;
    overflowed_ = true;
  }
}

bool OutQueue::flush(int fd) {
  while (off_ < buf_.size()) {
    const ssize_t n = ::send(fd, buf_.data() + off_, buf_.size() - off_,
                             MSG_NOSIGNAL);
    if (n > 0) {
      off_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  compact();
  return true;
}

bool OutQueue::take_overflow() {
  const bool v = overflowed_;
  overflowed_ = false;
  return v;
}

void OutQueue::compact() {
  if (off_ == 0) return;
  if (off_ == buf_.size()) {
    buf_.clear();
    off_ = 0;
    return;
  }
  if (off_ > (1 << 16)) {
    buf_.erase(0, off_);
    off_ = 0;
  }
}

}  // namespace sshos
```

- [ ] **Step 4: Écrire `frameclock.hpp`**

Créer `src/common/frameclock.hpp` :

```cpp
#pragma once

#include <algorithm>
#include <chrono>

namespace sshos {

// Drapeau « sale » plus plafond de cadence. Le démon ne compose pas une
// frame par octet reçu : il draine tout ce qui est lisible, marque sale,
// et compose au plus une fois par intervalle.
class FrameClock {
 public:
  using Clock = std::chrono::steady_clock;

  explicit FrameClock(std::chrono::milliseconds min_interval)
      : min_interval_(min_interval) {}

  void mark_dirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }

  // -1 : rien à faire. 0 : composer maintenant. n > 0 : armer le timer.
  int delay_ms(Clock::time_point now) const {
    if (!dirty_) return -1;
    if (!last_.has_value()) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_.value());
    if (elapsed >= min_interval_) return 0;
    return static_cast<int>((min_interval_ - elapsed).count());
  }

  void note_render(Clock::time_point now) {
    dirty_ = false;
    last_ = now;
  }

 private:
  std::chrono::milliseconds min_interval_;
  std::optional<Clock::time_point> last_;
  bool dirty_ = false;
};

}  // namespace sshos
```

Ajouter `#include <optional>` en tête de ce fichier : `std::optional` y est utilisé.

- [ ] **Step 5: Lancer les tests pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests outqueue && ./build/sshos_tests frameclock
```

Attendu : `4 cas` puis `1 cas`, 0 en échec.

Si `outqueue_keeps_the_remainder_and_asks_for_epollout` réussit à tout écrire, augmenter la taille poussée : la capacité du tampon d'un `socketpair` varie selon `net.core.wmem_default`.

- [ ] **Step 6: Commit**

```bash
git add src/common/outqueue.hpp src/common/outqueue.cpp src/common/frameclock.hpp tests/test_outqueue.cpp
git commit -m "feat(common): file de sortie non bloquante et cadence de rendu"
```

---

### Task 13: Session bouchon, boucle du démon, `main.cpp`, bout-en-bout

La tâche qui referme le jalon. `Session` est délibérément un bouchon — le jalon 2 le remplace par le vrai gestionnaire de fenêtres sans toucher au reste.

**Files:**
- Create: `src/common/platform.hpp`
- Create: `src/daemon/session.hpp`, `src/daemon/session.cpp`
- Create: `src/daemon/daemon.hpp`, `src/daemon/daemon.cpp`
- Create: `src/main.cpp`
- Create: `tests/test_session.cpp`
- Modify: `src/render/surface.hpp` (ajout de `text_row`), `src/render/surface.cpp`
- Modify: `src/common/net.hpp`, `src/common/net.cpp` (ajout de `peer_pid`)

**Interfaces:**
- Consumes: tout ce qui précède.
- Produces:
  - `std::string sshos::Surface::text_row(int y) const` — la ligne rendue en UTF-8, les cellules de continuation omises.
  - `pid_t sshos::peer_pid(int fd)` — pid du pair via `SO_PEERCRED`, `-1` si indisponible.
  - `struct sshos::Platform { virtual std::chrono::system_clock::time_point now() const = 0; virtual std::string read_file(std::string_view) const = 0; }` et `sshos::RealPlatform`.
  - `class sshos::Session` — `Session(Platform&, int cols, int rows)`, `void resize(int, int)`, `void on_input(const InputEvent&)`, `void render(Surface&)`, `bool wants_quit() const`.
  - `int sshos::run_daemon(std::string_view socket_name)`.

- [ ] **Step 1: Écrire le test qui échoue**

Créer `tests/test_session.cpp` :

```cpp
#include <chrono>
#include <string>

#include "common/platform.hpp"
#include "daemon/session.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Session;
using sshos::Surface;

namespace {

// Horloge figée : sans elle le harnais n'est pas déterministe par
// construction, et un test d'affichage d'heure est ininspectable.
struct FakePlatform : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    // 2026-08-10 14:05:00 UTC
    return std::chrono::system_clock::time_point(std::chrono::seconds(1786370700));
  }
  std::string read_file(std::string_view) const override { return {}; }
};

}  // namespace

TEST(surface_text_row_reads_back_what_was_written) {
  Surface s(6, 1);
  s.root().text(0, 0, "\xe6\x97\xa5" "ab", sshos::Style{});  // 日ab
  CHECK_EQ(s.text_row(0), std::string("\xe6\x97\xa5" "ab  "));
}

TEST(session_draws_a_panel_on_the_last_row) {
  FakePlatform plat;
  Session sess(plat, 40, 12);
  Surface s(40, 12);
  sess.render(s);
  const std::string panel = s.text_row(11);
  CHECK(panel.find("14:05") != std::string::npos);
  CHECK(panel.find("ssh_os") != std::string::npos);
}

TEST(session_draws_a_bordered_box_with_its_title) {
  FakePlatform plat;
  Session sess(plat, 40, 12);
  Surface s(40, 12);
  sess.render(s);

  bool found_title = false;
  for (int y = 0; y < 11; ++y) {
    if (s.text_row(y).find("ssh_os 2.0") != std::string::npos) found_title = true;
  }
  CHECK(found_title);
}

TEST(session_survives_a_terminal_smaller_than_the_minimum) {
  FakePlatform plat;
  Session sess(plat, 12, 3);
  Surface s(12, 3);
  sess.render(s);  // ne doit ni planter ni écrire hors surface
  CHECK(s.text_row(0).find("petit") != std::string::npos);
}

TEST(session_quits_on_ctrl_q) {
  FakePlatform plat;
  Session sess(plat, 40, 12);
  CHECK(!sess.wants_quit());
  sess.on_input(sshos::InputEvent{
      sshos::KeyEvent{sshos::Key::Char, U'q', sshos::mod::Ctrl}});
  CHECK(sess.wants_quit());
}
```

- [ ] **Step 2: Lancer le test pour vérifier qu'il échoue**

```bash
cmake --build build -j
```

Attendu : ÉCHEC, `common/platform.hpp: No such file or directory`.

- [ ] **Step 3: Ajouter `text_row` à `Surface`**

Dans `src/render/surface.hpp`, ajouter dans la partie publique de `Surface`, juste après `View root();` :

```cpp
  // Ligne rendue en UTF-8, cellules de continuation omises. Support des
  // assertions de propriété : `CHECK(s.text_row(3).find("Terminal") != npos)`
  // résiste à un changement de thème, un golden d'octets non.
  std::string text_row(int y) const;
```

et ajouter `#include <string>` en tête du fichier.

Dans `src/render/surface.cpp`, ajouter `#include "render/profile.hpp"` en tête et la définition suivante, avant la fermeture du namespace :

```cpp
std::string Surface::text_row(int y) const {
  std::string out;
  if (y < 0 || y >= h_) return out;
  for (int x = 0; x < w_; ++x) {
    const Cell& c = at(x, y);
    if (c.width == 0) continue;  // couverte par sa cellule de tête
    out += encode_utf8(c.ch);
  }
  return out;
}
```

- [ ] **Step 4: Ajouter `peer_pid` à `net`**

Dans `src/common/net.hpp`, avant la fermeture du namespace :

```cpp
// Pid du pair. C'est ainsi que `--kill` trouve le démon : pas de fichier
// de pid à maintenir, l'information est déjà dans le socket.
pid_t peer_pid(int fd);
```

Dans `src/common/net.cpp`, avant la fermeture du namespace :

```cpp
pid_t peer_pid(int fd) {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
  return static_cast<pid_t>(cred.pid);
}
```

- [ ] **Step 5: Écrire `platform.hpp`**

Créer `src/common/platform.hpp` :

```cpp
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace sshos {

// Couture d'injection. Tout ce qui touche au monde extérieur passe par
// ici : sans ça l'horloge du panneau rend le harnais non déterministe.
// `spawn()` rejoindra cette interface au jalon 3, avec le Terminal.
struct Platform {
  virtual ~Platform() = default;
  virtual std::chrono::system_clock::time_point now() const = 0;
  virtual std::string read_file(std::string_view path) const = 0;
};

struct RealPlatform : Platform {
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::now();
  }
  std::string read_file(std::string_view path) const override;
};

}  // namespace sshos
```

Créer `src/common/platform.cpp` :

```cpp
#include "common/platform.hpp"

#include <fstream>
#include <sstream>

namespace sshos {

std::string RealPlatform::read_file(std::string_view path) const {
  std::ifstream in{std::string(path)};
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

}  // namespace sshos
```

- [ ] **Step 6: Écrire `Session`**

Créer `src/daemon/session.hpp` :

```cpp
#pragma once

#include "common/platform.hpp"
#include "input/events.hpp"
#include "render/surface.hpp"

namespace sshos {

// Bouchon du jalon 1 : un panneau, une boîte à bordure, une horloge. Sa
// seule raison d'être est de prouver que la chaîne complète fonctionne.
// Le jalon 2 remplace cette classe par le vrai gestionnaire de fenêtres.
class Session {
 public:
  Session(Platform& plat, int cols, int rows);

  void resize(int cols, int rows);
  void on_input(const InputEvent& e);
  void render(Surface& out);
  bool wants_quit() const { return quit_; }

 private:
  Platform* plat_;
  int cols_;
  int rows_;
  bool quit_ = false;
  int clicks_ = 0;
};

}  // namespace sshos
```

Créer `src/daemon/session.cpp` :

```cpp
#include "daemon/session.hpp"

#include <ctime>
#include <string>
#include <variant>

namespace sshos {
namespace {

constexpr int kMinCols = 40;
constexpr int kMinRows = 12;

std::string clock_text(const Platform& plat) {
  const std::time_t t = std::chrono::system_clock::to_time_t(plat.now());
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

}  // namespace

Session::Session(Platform& plat, int cols, int rows)
    : plat_(&plat), cols_(cols), rows_(rows) {}

void Session::resize(int cols, int rows) {
  cols_ = cols;
  rows_ = rows;
}

void Session::on_input(const InputEvent& e) {
  if (const auto* k = std::get_if<KeyEvent>(&e)) {
    if (k->key == Key::Char && k->ch == U'q' && (k->mods & mod::Ctrl) != 0) {
      quit_ = true;
    }
  } else if (const auto* m = std::get_if<MouseEvent>(&e)) {
    if (m->action == MouseAction::Press) ++clicks_;
  }
}

void Session::render(Surface& out) {
  View v = out.root();
  Style bg;
  bg.bg = Color::indexed(4);
  v.fill(Rect{0, 0, out.w(), out.h()}, bg);

  if (out.w() < kMinCols || out.h() < kMinRows) {
    Style warn;
    warn.fg = Color::indexed(7);
    v.text(0, 0, "terminal trop petit - 40x12 minimum", warn);
    return;
  }

  // Panneau ancré en bas.
  Style panel;
  panel.bg = Color::indexed(0);
  panel.fg = Color::indexed(7);
  const int py = out.h() - 1;
  v.fill(Rect{0, py, out.w(), 1}, panel);
  v.text(1, py, "ssh_os", panel);
  const std::string t = clock_text(*plat_);
  v.text(out.w() - static_cast<int>(t.size()) - 1, py, t, panel);

  // Boîte centrée.
  const int bw = 24;
  const int bh = 6;
  const int bx = (out.w() - bw) / 2;
  const int by = (out.h() - 1 - bh) / 2;
  Style box;
  box.bg = Color::indexed(0);
  box.fg = Color::indexed(15);
  v.fill(Rect{bx, by, bw, bh}, box);
  for (int x = 1; x < bw - 1; ++x) {
    v.put(bx + x, by, U'-', box);
    v.put(bx + x, by + bh - 1, U'-', box);
  }
  for (int y = 1; y < bh - 1; ++y) {
    v.put(bx, by + y, U'|', box);
    v.put(bx + bw - 1, by + y, U'|', box);
  }
  v.put(bx, by, U'+', box);
  v.put(bx + bw - 1, by, U'+', box);
  v.put(bx, by + bh - 1, U'+', box);
  v.put(bx + bw - 1, by + bh - 1, U'+', box);

  v.text(bx + 2, by + 1, "ssh_os 2.0", box);
  v.text(bx + 2, by + 2, "clics: " + std::to_string(clicks_), box);
  v.text(bx + 2, by + 4, "Ctrl+Q pour quitter", box);
}

}  // namespace sshos
```

Ajouter `#include <cstdio>` en tête de `session.cpp` pour `std::snprintf`.

- [ ] **Step 7: Lancer les tests de session pour vérifier qu'ils passent**

```bash
cmake --build build -j && ./build/sshos_tests session && ./build/sshos_tests surface
```

Attendu : `5 cas` puis `10 cas`, 0 en échec.

- [ ] **Step 8: Écrire la boucle du démon**

Créer `src/daemon/daemon.hpp` :

```cpp
#pragma once

#include <string_view>

namespace sshos {

int run_daemon(std::string_view socket_name);

}  // namespace sshos
```

Créer `src/daemon/daemon.cpp` :

```cpp
#include "daemon/daemon.hpp"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <memory>
#include <string>

#include "common/frameclock.hpp"
#include "common/net.hpp"
#include "common/outqueue.hpp"
#include "common/platform.hpp"
#include "common/proto.hpp"
#include "daemon/session.hpp"
#include "input/parser.hpp"
#include "render/diff.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

constexpr size_t kBackpressureCeiling = 1u << 20;  // 1 Mo
constexpr int kFrameIntervalMs = 33;               // 30 fps

struct Client {
  Fd fd;
  Decoder dec;
  InputParser input;
  OutQueue out{kBackpressureCeiling};
  std::unique_ptr<Differ> differ;
  int cols = 80;
  int rows = 24;
};

void epoll_mod(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

void epoll_add(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

Fd make_signalfd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGCHLD);
  ::sigprocmask(SIG_BLOCK, &mask, nullptr);
  return Fd(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
}

}  // namespace

int run_daemon(std::string_view socket_name) {
  Fd listener;
  try {
    listener = bind_abstract(socket_name);
  } catch (const AddressInUse&) {
    return 0;  // un démon tourne déjà : rien à faire, ce n'est pas une erreur
  } catch (const std::exception&) {
    return 1;
  }
  set_nonblock(listener.get());

  RealPlatform plat;
  Session session(plat, 80, 24);
  Surface screen(80, 24);
  FrameClock clock{std::chrono::milliseconds(kFrameIntervalMs)};

  Fd ep(::epoll_create1(EPOLL_CLOEXEC));
  Fd sigfd = make_signalfd();
  Fd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));

  epoll_add(ep.get(), listener.get(), EPOLLIN);
  epoll_add(ep.get(), sigfd.get(), EPOLLIN);
  epoll_add(ep.get(), timer.get(), EPOLLIN);

  std::unique_ptr<Client> client;
  std::string scratch(65536, '\0');
  bool running = true;

  const auto drop_client = [&](const char* reason) {
    if (!client) return;
    if (reason != nullptr) {
      client->out.push(encode(Msg{Detached{reason}}));
      client->out.flush(client->fd.get());
    }
    ::epoll_ctl(ep.get(), EPOLL_CTL_DEL, client->fd.get(), nullptr);
    client.reset();
  };

  while (running) {
    epoll_event evs[16];
    const int timeout = clock.delay_ms(FrameClock::Clock::now());
    const int n = ::epoll_wait(ep.get(), evs, 16, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = evs[i].data.fd;
      const uint32_t events = evs[i].events;

      if (fd == sigfd.get()) {
        signalfd_siginfo si{};
        while (::read(sigfd.get(), &si, sizeof si) == sizeof si) {
          if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) running = false;
        }
        continue;
      }

      if (fd == timer.get()) {
        uint64_t ticks = 0;
        while (::read(timer.get(), &ticks, sizeof ticks) == sizeof ticks) {
        }
        continue;
      }

      if (fd == listener.get()) {
        Fd fresh = accept_peer(listener.get(), ::getuid());
        if (!fresh.valid()) continue;
        set_nonblock(fresh.get());
        // Le nouveau prend la main : l'ancien est détaché, pas partagé.
        drop_client("un autre client a pris la main");
        client = std::make_unique<Client>();
        client->fd = std::move(fresh);
        epoll_add(ep.get(), client->fd.get(), EPOLLIN);
        continue;
      }

      if (!client || fd != client->fd.get()) continue;

      // EPOLLHUP est signalé quel que soit le masque demandé : un
      // répartiteur qui ne teste que EPOLLIN boucle à 100 % de CPU.
      if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
        drop_client(nullptr);
        continue;
      }

      if ((events & EPOLLOUT) != 0) {
        if (!client->out.flush(client->fd.get())) {
          drop_client(nullptr);
          continue;
        }
        if (!client->out.wants_write()) epoll_mod(ep.get(), fd, EPOLLIN);
      }

      if ((events & EPOLLIN) != 0) {
        bool closed = false;
        for (;;) {
          const ssize_t got = ::read(fd, scratch.data(), scratch.size());
          if (got > 0) {
            client->dec.feed(std::string_view(scratch.data(), static_cast<size_t>(got)));
            continue;
          }
          if (got == 0) closed = true;
          if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          if (got < 0 && errno == EINTR) continue;
          if (got < 0) closed = true;
          break;
        }

        while (auto m = client->dec.next()) {
          if (const auto* h = std::get_if<Hello>(&*m)) {
            if (h->build_id != kBuildId) {
              client->out.push(encode(Msg{Incompatible{
                  "version du demon differente : relancez `sshos --kill`"}}));
              client->out.flush(client->fd.get());
              drop_client(nullptr);
              break;
            }
            client->cols = h->cols;
            client->rows = h->rows;
            client->differ = std::make_unique<Differ>(
                OutputProfile::detect(h->term, h->colorterm, h->utf8));
            set_ambiguous_wide(false);
            screen.resize(h->cols, h->rows);
            session.resize(h->cols, h->rows);
            client->out.push(encode(Msg{Welcome{}}));
            clock.mark_dirty();
          } else if (const auto* in = std::get_if<Input>(&*m)) {
            client->input.feed(in->bytes);
            while (auto e = client->input.next()) session.on_input(*e);
            clock.mark_dirty();
          } else if (const auto* rz = std::get_if<Resize>(&*m)) {
            client->cols = rz->cols;
            client->rows = rz->rows;
            screen.resize(rz->cols, rz->rows);
            session.resize(rz->cols, rz->rows);
            if (client->differ) client->differ->invalidate();
            clock.mark_dirty();
          }
        }

        if (session.wants_quit()) running = false;
        if (closed) drop_client(nullptr);
      }
    }

    // Composition : au plus une fois par intervalle, après avoir tout drainé.
    const auto now = FrameClock::Clock::now();
    if (client && client->differ && clock.delay_ms(now) == 0) {
      session.render(screen);
      const std::string ansi = client->differ->frame(screen, std::nullopt);
      if (!ansi.empty()) client->out.push(encode(Msg{FrameMsg{ansi}}));
      clock.note_render(now);

      if (client->out.take_overflow()) {
        // La file a été jetée : le diff n'est pas idempotent, il faut
        // repartir d'un écran complet.
        client->differ->invalidate();
        clock.mark_dirty();
      }
      if (!client->out.flush(client->fd.get())) {
        drop_client(nullptr);
      } else if (client->out.wants_write()) {
        epoll_mod(ep.get(), client->fd.get(), EPOLLIN | EPOLLOUT);
      }
    }

    // Réarmer le timer si un rendu reste dû plus tard.
    const int next = clock.delay_ms(FrameClock::Clock::now());
    itimerspec its{};
    if (next > 0) {
      its.it_value.tv_sec = next / 1000;
      its.it_value.tv_nsec = static_cast<long>(next % 1000) * 1000000L;
    }
    ::timerfd_settime(timer.get(), 0, &its, nullptr);
  }

  drop_client("le demon s'arrete");
  return 0;
}

}  // namespace sshos
```

- [ ] **Step 9: Écrire `main.cpp`**

Créer `src/main.cpp` :

```cpp
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "client/client.hpp"
#include "common/net.hpp"
#include "daemon/daemon.hpp"
#include "daemon/daemonize.hpp"

namespace {

constexpr int kConnectAttempts = 50;
constexpr int kConnectDelayUs = 20 * 1000;

std::string current_socket_name() {
  return sshos::socket_name(::getuid(), sshos::read_boot_id());
}

// Vrai si logind tuera les processus de l'utilisateur à la déconnexion,
// auquel cas le démon ne survivra pas malgré le détachement. Le seul cas
// où la fonctionnalité phare du projet échoue sans que rien ne soit cassé
// chez nous : mieux vaut le dire au premier lancement que le laisser
// découvrir à la reconnexion.
bool logind_kills_user_processes() {
  std::ifstream in("/etc/systemd/logind.conf");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#", 0) == 0) continue;
    if (line.find("KillUserProcesses=yes") != std::string::npos) return true;
  }
  return false;
}

int start_daemon_and_connect(const std::string& name) {
  const pid_t mid = sshos::spawn_detached({"/proc/self/exe", "--daemon"});
  if (mid < 0) {
    std::fprintf(stderr, "sshos: impossible de lancer le demon\n");
    return 1;
  }
  int status = 0;
  ::waitpid(mid, &status, 0);  // l'intermédiaire meurt aussitôt

  for (int i = 0; i < kConnectAttempts; ++i) {
    try {
      sshos::Fd probe = sshos::connect_abstract(name);
      return 0;
    } catch (const std::exception&) {
      ::usleep(kConnectDelayUs);
    }
  }
  std::fprintf(stderr, "sshos: le demon n'a pas repondu\n");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string name = current_socket_name();
  const std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "--daemon") {
    sshos::become_daemon();
    return sshos::run_daemon(name);
  }

  if (mode == "--status") {
    try {
      sshos::Fd s = sshos::connect_abstract(name);
      std::printf("demon actif (pid %d)\n", static_cast<int>(sshos::peer_pid(s.get())));
      return 0;
    } catch (const std::exception&) {
      std::printf("aucun demon\n");
      return 1;
    }
  }

  if (mode == "--kill") {
    try {
      sshos::Fd s = sshos::connect_abstract(name);
      const pid_t pid = sshos::peer_pid(s.get());
      s.reset();
      if (pid > 0 && ::kill(pid, SIGTERM) == 0) {
        std::printf("demon %d arrete\n", static_cast<int>(pid));
        return 0;
      }
    } catch (const std::exception&) {
    }
    std::printf("aucun demon\n");
    return 1;
  }

  if (!mode.empty()) {
    std::fprintf(stderr, "usage: sshos [--daemon|--status|--kill]\n");
    return 2;
  }

  // Mode normal : attacher, en démarrant le démon s'il n'existe pas.
  try {
    sshos::Fd probe = sshos::connect_abstract(name);
    probe.reset();
  } catch (const std::exception&) {
    if (logind_kills_user_processes()) {
      std::fprintf(stderr,
                   "sshos: attention, logind est configure avec "
                   "KillUserProcesses=yes ;\n        vos fenetres ne "
                   "survivront pas a la deconnexion.\n        Parade : "
                   "loginctl enable-linger %d\n",
                   static_cast<int>(::getuid()));
    }
    if (start_daemon_and_connect(name) != 0) return 1;
  }

  return sshos::run_client(name);
}
```

`main.cpp` n'utilise pas `Platform` : la couture d'injection sert au code testable, et `main` est précisément la seule partie qui ne l'est pas.

- [ ] **Step 10: Ajouter le binaire au CMakeLists**

Dans `CMakeLists.txt`, après la définition de `sshos_core` :

```cmake
add_executable(sshos src/main.cpp)
target_link_libraries(sshos PRIVATE sshos_core)
```

- [ ] **Step 11: Compiler et lancer toute la suite**

```bash
cmake --build build -j && ./build/sshos_tests
```

Attendu : tous les cas passent, code de retour 0, **80 cas** au total — 2 harnais, 4 `Fd`, 6 largeur, 9 surface, 9 profil, 9 diff, 4 protocole, 6 réseau, 15 entrée, 4 tty, 2 détachement, 5 file/cadence, 5 session.

- [ ] **Step 12: Vérification bout-en-bout manuelle**

Dans un terminal :

```bash
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j
./build-rel/sshos
```

Attendu : écran alterné, fond bleu, boîte à bordure `+---+` portant `ssh_os 2.0`, panneau noir en bas avec `ssh_os` à gauche et l'heure à droite.

Vérifier, dans l'ordre :
1. Cliquer dans la boîte : le compteur `clics:` augmente.
2. Redimensionner la fenêtre du terminal : la boîte reste centrée, le panneau reste collé en bas.
3. Rétrécir sous 40×12 : le message `terminal trop petit - 40x12 minimum` apparaît.
4. `Ctrl+Q` : retour au shell, **terminal intact** — le prompt s'affiche normalement, l'écho fonctionne, la souris ne crache pas de séquences.
5. Relancer, puis dans un autre terminal `kill -SEGV $(pgrep -f 'sshos$' | head -1)` côté **client** : le terminal doit rester utilisable grâce au filet `install_crash_handlers`.

- [ ] **Step 13: Vérification du détachement — sans SSH**

Tuer le *client* ne prouve rien : le démon a été détaché dès le départ. Le test qui compte fait disparaître la session entière.

```bash
./build-rel/sshos --kill
setsid --fork sh -c './build-rel/sshos --status; exit' ; sleep 1
# lancer le démon depuis une session qui disparaît ensuite
setsid --fork sh -c 'exec ./build-rel/sshos --daemon' ; sleep 1
./build-rel/sshos --status
```

Attendu : la dernière commande affiche `demon actif (pid N)` alors que la session qui l'a lancé n'existe plus.

- [ ] **Step 14: Vérification du détachement — à travers SSH**

C'est la vérification exigée par la spec §13.5. Elle ne vaut que si `sshd` accepte les connexions vers `localhost`.

```bash
ssh localhost "cd $(pwd) && ./build-rel/sshos --kill; exit" 2>/dev/null
ssh localhost "cd $(pwd) && ./build-rel/sshos --daemon & sleep 0.5; exit"
sleep 1
./build-rel/sshos --status
```

Attendu : `demon actif (pid N)`. La session SSH qui a lancé le démon est fermée depuis une seconde.

Si `--status` répond `aucun demon`, les causes par ordre de fréquence : `KillUserProcesses=yes` dans `/etc/systemd/logind.conf` (parade : `loginctl enable-linger $USER`) ; `SIGHUP` non ignoré avant l'`exec` ; `setsid()` ayant échoué parce que le processus était déjà chef de groupe.

Si `sshd` n'est pas disponible sur la machine, noter l'étape comme non exécutée — **ne pas la déclarer réussie** — et s'en tenir à l'étape 13.

- [ ] **Step 15: Commit**

```bash
git add src/common/platform.hpp src/common/platform.cpp src/daemon/session.hpp \
        src/daemon/session.cpp src/daemon/daemon.hpp src/daemon/daemon.cpp \
        src/main.cpp src/render/surface.hpp src/render/surface.cpp \
        src/common/net.hpp src/common/net.cpp tests/test_session.cpp CMakeLists.txt
git commit -m "feat: boucle du demon, session bouchon et attache bout-en-bout"
```

---

## Ce que ce jalon ne fait pas

Volontairement absents, traités par les plans suivants :

- **Fenêtres, `user_rect`, drag, redimensionnement au contour, panneau configurable, menu** → jalon 2. `Session` est un bouchon.
- **PTY, émulateur VT, `App`/`Host`, `spawn` dans `Platform`** → jalon 3.
- **Fichier de configuration `~/.config/sshos/config.ini`, touche leader, `esc_timeout_ms`** → jalon 2, quand il y aura des raccourcis à configurer. Le jalon 1 câble `Ctrl+Q` en dur, uniquement pour pouvoir sortir.
- **Sonde East Asian Ambiguous** → jalon 2. `set_ambiguous_wide(false)` est appelé en dur au handshake ; la sonde `\033[6n` viendra avec le reste du dialogue d'attache.
- **Journal `~/.local/state/sshos/daemon.log`** → jalon 2. Au jalon 1 le démon est muet, ce qui suffit : les échecs se voient au `--status`.
- **Goldens de frames** → jalon 2, quand le rendu aura une forme stable qui vaille la peine d'être figée. Au jalon 1 les assertions de propriété suffisent.

## Revue du plan

Vérifications faites sur le plan terminé.

**Couverture de la spec.** §3 architecture, socket et handshake → tâches 7, 8, 13. §4 rendu, `Cell`, largeurs, diffeur → tâches 3, 4, 5, 6. §7 entrée, souris, collage, focus → tâche 9. §10 détachement, `TtyGuard`, contre-pression → tâches 10, 11, 12, 13. §13 harnais et couture d'injection → tâches 1, 13. Les §5, §6, §8, §9, §12 relèvent des jalons 2 et 3 et sont listés ci-dessus comme hors périmètre.

**Cohérence des types.** `Style` est le triplet passé à `View::put`/`text`/`fill` et reconstruit par le diffeur via `style_of(Cell)` ; `Color` n'est jamais comparée qu'avec `operator==` généré ; `mod::Shift|Alt|Ctrl` a la même disposition de bits que le paramètre de modificateur CSI, ce dont `mods_from_param` dépend explicitement ; `OutputProfile` est construit une seule fois, au handshake, et vit dans le `Differ`.

**Trois défauts trouvés en traçant les tests à la main contre le code, et corrigés.** Le diffeur faisait démarrer l'extension d'un segment à `start` au lieu de la première cellule différente : quand la cellule de tête d'une paire large était identique et sa continuation modifiée, le segment sortait vide et la boucle ne progressait plus — `x = end = start` indéfiniment. Le diffeur n'émettait de `CUP` qu'en tête de segment, ce qui viole la règle 3 du §4.1 : le réancrage se fait maintenant par cellule. Enfin, le suivi du curseur par `std::optional` confondait « aucune frame émise » et « curseur caché », si bien qu'un bureau au repos réémettait l'enveloppe à chaque tour au lieu de zéro octet ; il est remplacé par `last_target_` / `last_shown_` / `first_`.

**Un point où le plan corrige son propre code.** L'étape 4 de la tâche 12 réclame l'ajout de `#include <optional>` à `frameclock.hpp`, signalé à l'endroit où il se pose.

