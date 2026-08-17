#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "daemon/host.hpp"
#include "daemon/session.hpp"
#include "harness.hpp"
#include "input/events.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"

using sshos::Session;
using sshos::Surface;

namespace {

constexpr int kCols = 60;
constexpr int kRows = 20;

struct GoldenPlatform : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    // 2026-08-10 14:05:00 UTC, le même instant figé que le reste de la
    // suite : une référence de rendu qui contient l'heure courante ne
    // serait une référence de rien.
    return std::chrono::system_clock::time_point(std::chrono::seconds(1786370700));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point{};
  }
  std::string read_file(std::string_view) const override { return {}; }
};

// SSHOS_GOLDEN_DIR d'abord, __FILE__ ensuite.
//
// __FILE__ est absolu (CMake passe des chemins absolus), donc le répertoire
// des références se déduit de la position de ce fichier, sans dépendre du
// répertoire courant du lanceur de tests. C'est exactement ce qu'il faut en
// développement -- et exactement ce qu'il ne faut pas pour un binaire de
// test PUBLIÉ : le chemin gravé est celui de la machine qui a compilé, et
// il n'existe pas chez celui qui le télécharge.
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

// Pose une variable d'environnement et rend l'état d'origine à la sortie de
// portée, qu'elle ait été définie ou absente. Il en existe déjà deux copies
// dans la suite (test_net.cpp, test_tty.cpp) ; une troisième reste plus
// simple que de faire dépendre cinquante fichiers de test d'un en-tête
// commun pour vingt lignes.
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

void key(Session& s, sshos::Key k, char32_t c = 0, uint8_t mods = 0) {
  s.on_input(sshos::InputEvent{sshos::KeyEvent{k, c, mods}});
}

void chord(Session& s, char32_t c) {
  key(s, sshos::Key::Char, U'a', sshos::mod::Ctrl);
  key(s, sshos::Key::Char, c, 0);
}

void click(Session& s, int x, int y) {
  s.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Press, 1, x, y, 0}});
  s.on_input(sshos::InputEvent{
      sshos::MouseEvent{sshos::MouseAction::Release, 1, x, y, 0}});
}

std::string dump_chars(const Surface& s) {
  std::string out;
  for (int y = 0; y < s.h(); ++y) {
    out += s.text_row(y);
    out += '\n';
  }
  return out;
}

std::string show_color(const sshos::Color& c) {
  switch (c.kind) {
    case sshos::ColorKind::Default:
      return "-";
    case sshos::ColorKind::Indexed:
      return std::to_string(static_cast<int>(c.idx));
    case sshos::ColorKind::Rgb: {
      char buf[16];
      std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
      return buf;
    }
  }
  return "?";
}

std::string dump_colors(const Surface& s) {
  std::string out;
  for (int y = 0; y < s.h(); ++y) {
    for (int x = 0; x < s.w(); ++x) {
      const sshos::Cell& c = s.at(x, y);
      if (x != 0) out += ' ';
      out += show_color(c.fg);
      out += '/';
      out += show_color(c.bg);
    }
    out += '\n';
  }
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Imprime la différence AVANT de régénérer : une régénération silencieuse
// transforme n'importe quelle régression en nouvelle vérité.
void report_diff(const std::string& path, const std::string& want,
                 const std::string& got) {
  std::vector<std::string> a;
  std::vector<std::string> b;
  std::istringstream ia(want);
  std::istringstream ib(got);
  std::string line;
  while (std::getline(ia, line)) a.push_back(line);
  while (std::getline(ib, line)) b.push_back(line);

  std::printf("  golden %s :\n", path.c_str());
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const std::string av = i < a.size() ? a[i] : std::string("<absente>");
    const std::string bv = i < b.size() ? b[i] : std::string("<absente>");
    if (av == bv) continue;
    std::printf("    %zu - |%s|\n", i, av.c_str());
    std::printf("    %zu + |%s|\n", i, bv.c_str());
  }
}

void compare(const std::string& name, const std::string& suffix,
             const std::string& got, const char* file, int line) {
  const std::string path = golden_dir() + name + suffix;
  const std::string want = read_file(path);
  if (want == got) return;
  // Imprimer AVANT de régénérer, toujours : sans ça, UPDATE_GOLDEN=1
  // transforme n'importe quelle régression en nouvelle vérité sans que
  // personne n'ait rien vu passer.
  report_diff(path, want, got);
  if (std::getenv("UPDATE_GOLDEN") != nullptr) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << got;
    return;
  }
  th::fail(file, line, "golden " + path);
}

void run_scenario(const std::string& name,
                  const std::function<void(Session&)>& script, const char* file,
                  int line) {
  GoldenPlatform plat;
  sshos::NullFdRegistrar fds;
  Session sess(plat, fds, kCols, kRows);
  // Bordures ASCII partout : un cadre Unicode rendrait le fichier de
  // référence illisible dans un diff, et le jeu de caractères est déjà
  // couvert par les tests de View::box.
  sshos::OutputProfile prof;
  prof.utf8 = false;
  prof.depth = sshos::ColorDepth::Indexed256;
  sess.set_output(prof);

  Surface s(kCols, kRows);
  sess.render(s);  // le bureau s'ouvre
  script(sess);
  sess.render(s);

  compare(name, ".txt", dump_chars(s), file, line);
  compare(name, ".color.txt", dump_colors(s), file, line);
}

}  // namespace

#define GOLDEN(name, script) run_scenario(name, script, __FILE__, __LINE__)

TEST(golden_desktop_at_startup) {
  GOLDEN("demarrage", [](Session&) {});
}

TEST(golden_two_overlapping_windows) {
  GOLDEN("deux_fenetres", [](Session& s) { s.open_from_catalog("editeur"); });
}

TEST(golden_a_minimized_window_keeps_its_panel_entry) {
  GOLDEN("fenetre_reduite", [](Session& s) {
    s.open_from_catalog("fichiers");
    chord(s, U'-');
  });
}

TEST(golden_a_maximized_window) {
  GOLDEN("fenetre_maximisee", [](Session& s) { chord(s, U'z'); });
}

TEST(golden_a_fullscreen_window_hides_the_panel) {
  GOLDEN("plein_ecran", [](Session& s) { chord(s, U'f'); });
}

TEST(golden_the_menu_open_and_filtered) {
  GOLDEN("menu_ouvert", [](Session& s) {
    chord(s, U' ');
    key(s, sshos::Key::Char, U'p');
    key(s, sshos::Key::Char, U'a');
  });
}

TEST(golden_the_modal_asking_before_a_close) {
  GOLDEN("modale_ouverte", [](Session& s) {
    key(s, sshos::Key::Char, U'x');  // Bloc se déclare modifiée
    chord(s, U'w');
  });
}

// L'aide est la parade au risque « la touche leader est peu découvrable »
// du §16. Elle a donc un golden : c'est la seule page de ce bureau qu'un
// utilisateur lira MOT À MOT, et une colonne décalée s'y voit tout de
// suite.
TEST(golden_the_help_overlay) {
  GOLDEN("aide_ouverte", [](Session& s) { chord(s, U'?'); });
}

TEST(golden_the_panel_on_the_left_edge) {
  GOLDEN("panneau_a_gauche", [](Session& s) {
    chord(s, U' ');
    for (const char32_t c : {U'g', U'a', U'u'}) {
      key(s, sshos::Key::Char, c);
    }
    key(s, sshos::Key::Enter);
    click(s, 30, 10);  // un clic anodin, pour figer aussi le focus
  });
}

// LES RÉFÉRENCES DOIVENT POUVOIR DÉMÉNAGER. __FILE__ est gravé à la
// compilation : un binaire de test publié dans une release chercherait le
// répertoire de la machine qui l'a compilé, qui n'existe pas chez celui qui
// le télécharge. Les neuf cas ci-dessus échoueraient alors par
// construction, et l'échelon binaire de l'installeur raterait
// systématiquement son propre garde-fou -- « on n'installe qu'après le
// vert » ne pourrait jamais être vert.
//
// La variable est donc consultée d'abord ; le chemin compilé reste le repli
// du développement, où il est exactement ce qu'il faut.
TEST(golden_dir_prefers_the_environment_over_the_compiled_path) {
  EnvVarGuard guard("SSHOS_GOLDEN_DIR");

  ::setenv("SSHOS_GOLDEN_DIR", "/tmp/refs", 1);
  CHECK_EQ(golden_dir(), std::string("/tmp/refs/"));

  // La barre oblique finale est ajoutée si elle manque, jamais doublée.
  ::setenv("SSHOS_GOLDEN_DIR", "/tmp/refs/", 1);
  CHECK_EQ(golden_dir(), std::string("/tmp/refs/"));

  // Vide vaut absente : on ne va pas chercher les références à la racine.
  ::setenv("SSHOS_GOLDEN_DIR", "", 1);
  CHECK(golden_dir().find("tests/golden/") != std::string::npos);

  ::unsetenv("SSHOS_GOLDEN_DIR");
  CHECK(golden_dir().find("tests/golden/") != std::string::npos);
}
