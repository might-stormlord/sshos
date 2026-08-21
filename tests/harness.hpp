#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "render/cell.hpp"

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

// Un test qui lève au lieu d'échouer proprement (helper mal nourri, ressource
// système indisponible pendant le run...) ne doit pas abattre le binaire
// entier : tous les cas qui suivent dans le registre ne tourneraient jamais
// et aucune ligne de résumé ne serait imprimée. main() encadre chaque
// c.fn() d'un try/catch et route ici plutôt que vers fail(file, line, ...) :
// une exception qui a remonté toute la pile du test n'a pas de site
// __FILE__/__LINE__ pertinent, seulement le nom du cas qui l'a laissée
// s'échapper.
inline void fail_uncaught(const char* test_name, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s  exception non interceptee : %s\n",
               test_name, what.c_str());
}

// Meme raisonnement que fail_uncaught, pour un cas dont le PROCESSUS ouvrier
// a du etre tue depuis l'exterieur apres avoir depasse le delai imparti
// (voir le superviseur dans tests/main.cpp) : ni __FILE__ ni __LINE__ ne
// sont pertinents, le depassement de delai est detecte par un AUTRE
// processus (le superviseur, via sem_timedwait), jamais dans la pile du cas
// lui-meme.
inline void fail_timeout(const char* test_name, long timeout_ms) {
  ++failures();
  std::fprintf(stderr,
               "  FAIL %s  timeout : le cas n'a pas rendu la main en %ld ms "
               "(reglable via TERMOS_TEST_TIMEOUT_MS)\n",
               test_name, timeout_ms);
}

// Pour un cas dont le PROCESSUS ouvrier a disparu de facon anormale (signal,
// code de sortie non nul) avant d'avoir poste son achevement -- crash,
// SIGSEGV, abort ASan/UBSan... Deliberement DISTINCTE de fail_uncaught : ce
// dernier dit "exception non interceptee", ce qui serait faux ici (rien n'a
// ete lance ni capture dans une pile C++ ; le processus entier a disparu, pas
// une fonction). Sous ASan en particulier, un crash reel se termine souvent
// par un _exit(1) que le sanitizer declenche APRES avoir imprime son propre
// rapport -- ni un signal ni une exception -- et le dire induirait en erreur
// qui lit le resume sur la vraie cause. Le detail (signal nomme ou code de
// sortie) vient de describe_exit() dans tests/main.cpp.
inline void fail_worker_died(const char* test_name, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s  processus ouvrier interrompu : %s\n",
               test_name, what.c_str());
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

template <>
inline std::string show<char32_t>(const char32_t& v) {
  // Afficher les caractères ASCII imprimables tels quels, sinon en notation
  // hex. Éviter static_cast<char> qui tronque et rend illisible les
  // codepoints non-ASCII (日 → garbage, U+FFFD → garbage).
  if (v >= 0x20 && v < 0x7f && v != '\'') {
    char buf[8];
    std::snprintf(buf, sizeof buf, "U'%c'", static_cast<char>(v));
    return buf;
  }
  char buf[16];
  std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(v));
  return buf;
}

template <>
inline std::string show<sshos::Color>(const sshos::Color& v) {
  std::ostringstream os;
  os << "Color(kind=";
  switch (v.kind) {
    case sshos::ColorKind::Default: os << "Default"; break;
    case sshos::ColorKind::Indexed: os << "Indexed(" << static_cast<int>(v.idx) << ")"; break;
    case sshos::ColorKind::Rgb:
      os << "Rgb(" << static_cast<int>(v.r) << ","
         << static_cast<int>(v.g) << "," << static_cast<int>(v.b) << ")";
      break;
  }
  os << ")";
  return os.str();
}

}  // namespace th

#define TEST(name)                                        \
  static void test_##name();                              \
  static th::Registrar reg_##name(#name, &test_##name);   \
  static void test_##name()

// CHECK / CHECK_EQ enregistrent un échec et laissent le test continuer :
// c'est voulu, ça permet de voir toutes les assertions en défaut d'un seul
// coup plutôt qu'une seule à la fois. Utiliser CHECK/CHECK_EQ par défaut.
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

// REQUIRE / REQUIRE_EQ enregistrent un échec exactement comme CHECK/CHECK_EQ,
// puis font `return;` : le reste du test n'est pas exécuté. À réserver aux
// cas où continuer serait non seulement redondant mais dangereux — typiquement
// juste avant de déréférencer quelque chose dont l'assertion qui précède
// vient de prouver l'absence (un `.at(0)` sur un vecteur dont on vient de
// vérifier la taille, par exemple). Ne pas les utiliser par défaut : elles
// cachent les assertions suivantes si la condition échoue, alors que
// CHECK/CHECK_EQ les montrent toutes.
//
// Le `return;` est nu : REQUIRE/REQUIRE_EQ ne compilent que dans une
// fonction de retour void, en pratique le corps d'un TEST(...). Elles ne
// conviennent pas à une fonction utilitaire qui retourne une valeur (ex. un
// helper `KeyEvent one_key(...)` — voir tests/test_input.cpp) ; dans ce cas
// la seule protection possible reste CHECK_EQ suivi d'un déréférencement qui
// peut lever, ce que le catch-all de tests/main.cpp transforme désormais en
// un échec propre au lieu de faire tomber tout le binaire.
#define REQUIRE(cond)                                                  \
  do {                                                                 \
    if (!(cond)) {                                                     \
      th::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");              \
      return;                                                          \
    }                                                                  \
  } while (0)

#define REQUIRE_EQ(a, b)                                                     \
  do {                                                                       \
    auto&& _a = (a);                                                        \
    auto&& _b = (b);                                                        \
    if (!(_a == _b)) {                                                      \
      th::fail(__FILE__, __LINE__,                                          \
               "REQUIRE_EQ(" #a ", " #b ")\n       obtenu = " + th::show(_a) + \
                   "\n       attendu = " + th::show(_b));                   \
      return;                                                               \
    }                                                                       \
  } while (0)
