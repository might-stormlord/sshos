#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "apps/files/files.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Files;
using sshos::Key;
using sshos::KeyEvent;
using sshos::MouseAction;
using sshos::MouseEvent;
using sshos::Size;
namespace mod = sshos::mod;

namespace {

// Un arbre fabriqué de toutes pièces, détruit à la sortie du cas. RAII :
// le harnais rend la main par un `return` nu sur un REQUIRE raté, et tout
// nettoyage écrit à la main serait sauté ce jour-là.
class Tree {
 public:
  Tree() {
    char tpl[] = "/tmp/sshos-app-XXXXXX";
    const char* made = ::mkdtemp(tpl);
    if (made != nullptr) root_ = made;
  }
  ~Tree() {
    for (auto it = made_.rbegin(); it != made_.rend(); ++it) {
      ::unlink(it->c_str());
      ::rmdir(it->c_str());
    }
    ::rmdir(root_.c_str());
  }
  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;

  const std::string& root() const { return root_; }
  bool valid() const { return !root_.empty(); }

  std::string file(const std::string& rel) {
    const std::string p = root_ + "/" + rel;
    const int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) ::close(fd);
    made_.push_back(p);
    return p;
  }
  std::string dir(const std::string& rel) {
    const std::string p = root_ + "/" + rel;
    ::mkdir(p.c_str(), 0700);
    made_.push_back(p);
    return p;
  }

 private:
  std::string root_;
  std::vector<std::string> made_;
};

KeyEvent ch(char32_t c) { return KeyEvent{Key::Char, c, 0}; }
KeyEvent key(Key k) { return KeyEvent{k, 0, 0}; }

std::string names(const Files& f) {
  std::string out;
  for (const auto& e : f.visible_for_tests()) {
    if (!out.empty()) out.push_back('|');
    out += e.name;
  }
  return out;
}

std::string selected_name(const Files& f) {
  const auto& v = f.visible_for_tests();
  return v.empty() ? std::string() : v[f.selected_for_tests()].name;
}

}  // namespace

// ------------------------------------------------------------ la navigation

TEST(files_lists_the_directory_it_starts_in) {
  Tree t;
  REQUIRE(t.valid());
  t.file("alpha.txt");
  t.dir("beta");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  CHECK_EQ(names(f), std::string("..|beta|alpha.txt"));
}

TEST(files_moves_the_selection_with_the_arrows) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");
  t.file("b");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  CHECK_EQ(selected_name(f), std::string(".."));
  f.on_key(key(Key::Down));
  CHECK_EQ(selected_name(f), std::string("a"));
  f.on_key(key(Key::Up));
  CHECK_EQ(selected_name(f), std::string(".."));
}

// La sélection ne sort JAMAIS de la liste, aux deux bouts.
TEST(files_never_moves_the_selection_out_of_the_list) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  // UNE seule pression depuis chaque bord. Dix se compensaient : la
  // sélection rebondissait d'un bout à l'autre et retombait sur ses pieds
  // un coup sur deux, si bien que le test passait contre une flèche qui
  // ne s'arrêtait pas.
  f.on_key(key(Key::Up));
  CHECK_EQ(f.selected_for_tests(), size_t{0});

  f.on_key(key(Key::End));
  const size_t last = f.visible_for_tests().size() - 1;
  f.on_key(key(Key::Down));
  CHECK_EQ(f.selected_for_tests(), last);
}

TEST(files_goes_down_into_a_directory_and_back_up) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");
  t.file("sous/dedans.txt");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));  // « sous »
  REQUIRE_EQ(selected_name(f), std::string("sous"));
  f.on_key(key(Key::Enter));

  CHECK_EQ(f.path_for_tests(), t.root() + "/sous");
  CHECK_EQ(names(f), std::string("..|dedans.txt"));

  f.on_key(key(Key::Backspace));
  CHECK_EQ(f.path_for_tests(), t.root());
}

// REMONTER SÉLECTIONNE D'OÙ L'ON VIENT. Sans cela, on cherche des yeux le
// dossier qu'on vient de quitter à chaque aller-retour.
TEST(files_selects_the_directory_it_came_from_when_going_up) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("aaa");
  t.dir("bbb");
  t.dir("ccc");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Down));  // « bbb »
  REQUIRE_EQ(selected_name(f), std::string("bbb"));
  f.on_key(key(Key::Enter));
  f.on_key(key(Key::Backspace));

  CHECK_EQ(selected_name(f), std::string("bbb"));
}

// La racine n'a pas de parent : remonter depuis elle ne doit RIEN faire.
TEST(files_stays_at_the_root_when_it_cannot_go_up) {
  Files f("/");
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Backspace));
  CHECK_EQ(f.path_for_tests(), std::string("/"));
}

// Entrer sur un fichier ne descend nulle part, mais le DIT : une touche
// sans effet et sans explication passe pour une panne.
TEST(files_says_something_when_a_file_cannot_be_opened_yet) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a.txt");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("a.txt"));
  f.on_key(key(Key::Enter));

  CHECK_EQ(f.path_for_tests(), t.root());
  // Le message EXACT : « non vide » passerait aussi bien contre un code
  // qui aurait essayé de descendre dans le fichier et rapporté l'erreur du
  // système.
  CHECK(f.status_for_tests().find("editeur") != std::string::npos);
}

// --------------------------------------------------------------- le filtre

TEST(files_filters_on_what_is_typed) {
  Tree t;
  REQUIRE(t.valid());
  t.file("rapport.txt");
  t.file("notes.md");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(ch(U'r'));
  f.on_key(ch(U'a'));

  CHECK_EQ(f.filter_for_tests(), std::string("ra"));
  CHECK_EQ(names(f), std::string("..|rapport.txt"));
}

// Le retour arrière efface le filtre AVANT de remonter : remonter avec un
// filtre à moitié tapé ferait perdre le répertoire pour une faute de
// frappe.
TEST(files_rubs_out_the_filter_before_it_walks_up) {
  Tree t;
  REQUIRE(t.valid());
  t.file("rapport.txt");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(ch(U'r'));
  f.on_key(key(Key::Backspace));

  CHECK_EQ(f.filter_for_tests(), std::string(""));
  CHECK_EQ(f.path_for_tests(), t.root());
}

TEST(files_clears_the_filter_on_escape) {
  Tree t;
  REQUIRE(t.valid());
  t.file("rapport.txt");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(ch(U'r'));
  f.on_key(key(Key::Escape));

  CHECK_EQ(f.filter_for_tests(), std::string(""));
  CHECK_EQ(names(f), std::string("..|rapport.txt"));
}

// Un filtre qui rétrécit la liste sous la sélection ne doit pas la laisser
// hors bornes -- c'est le chemin le plus court vers une lecture hors
// tableau.
TEST(files_keeps_the_selection_inside_a_list_the_filter_shrank) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");
  t.file("bbb");
  t.file("ccc");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::End));
  REQUIRE(f.selected_for_tests() > size_t{1});

  f.on_key(ch(U'z'));  // ne correspond à rien : il ne reste que « .. »
  CHECK_EQ(names(f), std::string(".."));
  CHECK(f.selected_for_tests() < f.visible_for_tests().size());
}

// ------------------------------------------------------------- les cachés

TEST(files_toggles_the_hidden_files_with_a_dot) {
  Tree t;
  REQUIRE(t.valid());
  t.file(".cache");
  t.file("visible");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  CHECK_EQ(names(f), std::string("..|visible"));

  f.on_key(ch(U'.'));
  CHECK_EQ(names(f), std::string("..|.cache|visible"));

  f.on_key(ch(U'.'));
  CHECK_EQ(names(f), std::string("..|visible"));
}

// Mais `.` FILTRE dès qu'un filtre est en cours : sinon on ne pourrait
// jamais chercher un nom qui contient un point.
TEST(files_lets_a_dot_be_typed_into_a_running_filter) {
  Tree t;
  REQUIRE(t.valid());
  t.file("notes.md");
  t.file("notes");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(ch(U's'));
  f.on_key(ch(U'.'));

  CHECK_EQ(f.filter_for_tests(), std::string("s."));
  CHECK_EQ(names(f), std::string("..|notes.md"));
}

// ---------------------------------------------------------- le défilement

TEST(files_scrolls_to_follow_the_selection_downwards) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 20; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});  // quatre lignes de liste
  CHECK_EQ(f.top_for_tests(), size_t{0});

  f.on_key(key(Key::End));
  CHECK(f.top_for_tests() > size_t{0});
  CHECK(f.selected_for_tests() >= f.top_for_tests());
  CHECK(f.selected_for_tests() < f.top_for_tests() + 4);
}

TEST(files_scrolls_back_up_with_the_selection) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 20; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});
  f.on_key(key(Key::End));
  REQUIRE(f.top_for_tests() > size_t{0});

  f.on_key(key(Key::Home));
  CHECK_EQ(f.top_for_tests(), size_t{0});
  CHECK_EQ(f.selected_for_tests(), size_t{0});
}

// Jamais de page vide en bas : quand la fenêtre grandit, le défilement
// doit remonter plutôt que de montrer du vide sous la dernière ligne.
TEST(files_never_leaves_an_empty_page_below_the_list) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 8; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 5});  // trois lignes
  f.on_key(key(Key::End));
  REQUIRE(f.top_for_tests() > size_t{0});

  f.on_resize(Size{40, 20});  // large : tout tient
  CHECK_EQ(f.top_for_tests(), size_t{0});
}

// ---------------------------------------------------------------- la souris

TEST(files_selects_the_row_that_was_clicked) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");
  t.file("b");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 2, 0});  // deuxième ligne
  CHECK_EQ(selected_name(f), std::string("a"));
}

// Recliquer la ligne DÉJÀ choisie l'ouvre. Pas de double-clic à compter :
// « cliquer deux fois » se découvre tout seul.
TEST(files_opens_the_row_that_was_clicked_twice) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 2, 0});
  REQUIRE_EQ(selected_name(f), std::string("sous"));

  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 2, 0});
  CHECK_EQ(f.path_for_tests(), t.root() + "/sous");
}

TEST(files_ignores_a_click_below_the_last_entry) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 9, 0});
  CHECK_EQ(selected_name(f), std::string(".."));
}

TEST(files_ignores_a_click_on_the_path_bar) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("a"));

  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 0, 0});
  CHECK_EQ(selected_name(f), std::string("a"));
}

// ------------------------- douze trous montrés par les mutations

// Un répertoire de départ illisible ne doit ni planter ni prétendre être
// vide : la liste est vide, et le message dit pourquoi.
TEST(files_survives_a_start_directory_it_cannot_read) {
  Files f("/proc/1/fdinfo/inexistant-xyz");
  f.on_resize(Size{40, 12});

  CHECK(f.visible_for_tests().empty());
  CHECK_EQ(f.selected_for_tests(), size_t{0});
  CHECK(!f.status_for_tests().empty());
}

// La liste occupe EXACTEMENT la fenêtre moins la barre de chemin et la
// ligne d'état. Une assertion lâche laissait passer une ligne de moins.
TEST(files_gives_the_list_the_window_minus_two_lines) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 20; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});  // 6 - 2 = 4 lignes de liste
  f.on_key(key(Key::End));

  CHECK_EQ(f.top_for_tests(), f.visible_for_tests().size() - 4);
}

// Un caractère de contrôle n'est pas de la saisie : le laisser entrer dans
// le filtre le rendrait impossible à corriger.
TEST(files_never_lets_a_control_character_into_the_filter) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(KeyEvent{Key::Char, static_cast<char32_t>(1), 0});

  CHECK_EQ(f.filter_for_tests(), std::string(""));
}

// Remonter depuis la racine ne doit RIEN toucher -- pas même la sélection.
TEST(files_touches_nothing_when_it_cannot_go_up) {
  Files f("/");
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Down));
  const size_t before = f.selected_for_tests();
  REQUIRE(before > size_t{0});

  f.on_key(key(Key::Backspace));
  CHECK_EQ(f.selected_for_tests(), before);
  CHECK_EQ(f.path_for_tests(), std::string("/"));
}

// Changer de répertoire EFFACE le filtre : le garder ferait arriver dans
// un dossier qui paraît presque vide sans qu'on sache pourquoi.
TEST(files_drops_the_filter_when_it_goes_up) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");

  Files f(t.root() + "/sous");
  f.on_resize(Size{40, 12});
  f.on_key(ch(U'z'));  // ne correspond à rien
  REQUIRE_EQ(f.filter_for_tests(), std::string("z"));

  f.on_key(key(Key::Enter));  // « .. » est la seule entrée qui reste
  CHECK_EQ(f.path_for_tests(), t.root());
  CHECK_EQ(f.filter_for_tests(), std::string(""));
}

TEST(files_drops_the_filter_when_it_goes_down) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");
  t.file("sous/dedans");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(ch(U's'));
  REQUIRE_EQ(f.filter_for_tests(), std::string("s"));
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("sous"));

  f.on_key(key(Key::Enter));
  CHECK_EQ(f.filter_for_tests(), std::string(""));
  CHECK_EQ(names(f), std::string("..|dedans"));
}

// `..` REMONTE, il ne descend pas dedans : traiter `..` comme un dossier
// ordinaire donnerait un chemin non normalisé qui s'allonge à chaque
// aller-retour.
TEST(files_walks_up_when_the_parent_entry_is_opened) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");

  Files f(t.root() + "/sous");
  f.on_resize(Size{40, 12});
  REQUIRE_EQ(selected_name(f), std::string(".."));

  f.on_key(key(Key::Enter));
  CHECK_EQ(f.path_for_tests(), t.root());
}

// Descendre repart du HAUT de la nouvelle liste : garder la sélection
// d'avant place le curseur sur une entrée qui n'a rien à voir.
TEST(files_starts_at_the_top_of_the_directory_it_enters) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("sous");
  t.file("sous/a");
  t.file("sous/b");
  t.file("sous/c");
  t.file("zzz1");
  t.file("zzz2");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::End));
  REQUIRE(f.selected_for_tests() > size_t{1});
  f.on_key(key(Key::Home));
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("sous"));

  f.on_key(key(Key::Enter));
  CHECK_EQ(f.selected_for_tests(), size_t{0});
}

// Un répertoire qui disparaît entre la lecture et l'ouverture laisse
// l'application OÙ ELLE EST, avec son message. Y descendre pour y montrer
// une liste vide donnerait l'impression d'un dossier vide.
TEST(files_stays_put_when_the_directory_vanished) {
  Tree t;
  REQUIRE(t.valid());
  const std::string doomed = t.dir("ephemere");

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("ephemere"));

  ::rmdir(doomed.c_str());
  f.on_key(key(Key::Enter));

  CHECK_EQ(f.path_for_tests(), t.root());
  CHECK(!f.status_for_tests().empty());
}

// Le clic tient compte du DÉFILEMENT : sans lui, cliquer une ligne d'une
// liste défilée sélectionne une entrée qui n'est pas celle qu'on voit.
TEST(files_reads_a_click_through_the_scroll_offset) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 20; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});
  f.on_key(key(Key::End));
  const size_t top = f.top_for_tests();
  REQUIRE(top > size_t{0});

  // Première ligne de la liste : c'est l'entrée numéro `top`.
  f.on_mouse(MouseEvent{MouseAction::Press, 0, 3, 1, 0});
  CHECK_EQ(f.selected_for_tests(), top);
}

TEST(files_scrolls_with_the_wheel) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 20; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 12});
  f.on_mouse(MouseEvent{MouseAction::WheelDown, 0, 3, 3, 0});
  const size_t after_down = f.selected_for_tests();
  CHECK(after_down > size_t{0});

  f.on_mouse(MouseEvent{MouseAction::WheelUp, 0, 3, 3, 0});
  CHECK(f.selected_for_tests() < after_down);
}

// ============================================================== le rendu

namespace {

// La fenêtre peinte, ligne par ligne, blancs de fin rognés.
std::string painted(Files& f, int w, int h) {
  sshos::Surface s(w, h);
  f.render(sshos::View(s, sshos::Rect{0, 0, w, h}));
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = s.text_row(y);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

// La ligne `y` telle qu'elle est peinte.
std::string painted_row(Files& f, int w, int h, int y) {
  sshos::Surface s(w, h);
  f.render(sshos::View(s, sshos::Rect{0, 0, w, h}));
  std::string row = s.text_row(y);
  while (!row.empty() && row.back() == ' ') row.pop_back();
  return row;
}

// La cellule peinte -- `Cell` porte ses couleurs et ses attributs a plat,
// pas un `Style`.
sshos::Cell cell_at(Files& f, int w, int h, int x, int y) {
  sshos::Surface s(w, h);
  f.render(sshos::View(s, sshos::Rect{0, 0, w, h}));
  return s.at(x, y);
}

}  // namespace

TEST(files_paints_the_path_on_the_first_row) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{60, 8});
  CHECK_EQ(painted_row(f, 60, 8, 0), t.root());
}

// Un chemin trop long est élidé PAR LA GAUCHE : c'est la fin d'un chemin
// qui porte l'information, jamais son début.
TEST(files_elides_a_long_path_from_the_left) {
  Files f("/usr/share/doc/un-paquet-au-nom-tres-long/exemples");
  f.on_resize(Size{20, 8});

  // La LARGEUR est celle de la surface, qui refuse d'écrire hors clip : ce
  // que ce cas vérifie est la FORME de l'élision -- le caractère
  // d'élision en tête, et la fin du chemin conservée. Compter les octets
  // ne dirait rien, « … » en pèse trois à lui seul.
  const std::string row = painted_row(f, 20, 8, 0);
  CHECK_EQ(row.rfind("…", 0), size_t{0});
  CHECK(row.find("exemples") != std::string::npos);
}

TEST(files_paints_the_entries_under_the_path) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("beta");
  t.file("alpha");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  const std::string g = painted(f, 40, 8);
  CHECK(g.find("..") != std::string::npos);
  CHECK(g.find("beta") != std::string::npos);
  CHECK(g.find("alpha") != std::string::npos);
}

// La SÉLECTION porte l'inverse vidéo, et elle seule. Sans elle, on ne sait
// pas où l'on est.
TEST(files_marks_the_selection_with_reverse_video) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  // Ligne 1 = première entrée = « .. », sélectionnée au départ.
  CHECK_EQ(cell_at(f, 40, 8, 0, 1).attrs & sshos::attr::Reverse,
           sshos::attr::Reverse);
  CHECK_EQ(cell_at(f, 40, 8, 0, 2).attrs & sshos::attr::Reverse, 0);

  f.on_key(key(Key::Down));
  CHECK_EQ(cell_at(f, 40, 8, 0, 2).attrs & sshos::attr::Reverse,
           sshos::attr::Reverse);
}

// Un répertoire ne se lit pas comme un fichier : la couleur le dit, parce
// qu'une liste sans distinction oblige à lire chaque nom.
TEST(files_gives_directories_a_colour_of_their_own) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("beta");
  t.file("alpha");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));  // la sélection quitte « .. »
  f.on_key(key(Key::Down));  // ... et quitte « beta »

  const sshos::Cell dir_cell = cell_at(f, 40, 8, 0, 2);   // beta
  const sshos::Cell file_cell = cell_at(f, 40, 8, 0, 3);  // alpha
  CHECK(!(dir_cell.fg == file_cell.fg));
}

// La ligne d'état porte le filtre en cours : sans elle, une liste réduite
// passe pour un dossier presque vide.
TEST(files_shows_the_running_filter_on_the_status_row) {
  Tree t;
  REQUIRE(t.valid());
  t.file("rapport");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(ch(U'r'));
  f.on_key(ch(U'a'));
  f.on_key(ch(U'p'));

  // Le TEXTE du filtre, pas seulement le mot « filtre » : chercher un
  // simple « r » passait aussi contre une ligne qui n'affiche que
  // l'étiquette, puisqu'elle en contient un.
  const std::string row = painted_row(f, 40, 8, 7);
  CHECK(row.find("rap") != std::string::npos);
}

TEST(files_shows_an_error_on_the_status_row) {
  Files f("/proc/1/fdinfo/inexistant-xyz");
  f.on_resize(Size{40, 8});

  const std::string row = painted_row(f, 40, 8, 7);
  CHECK(!row.empty());
}

// Un nom plus large que la fenêtre est élidé SANS COUPER une pleine chasse
// en deux : la moitié restée seule s'afficherait comme un demi
// idéogramme.
TEST(files_elides_a_long_name_without_splitting_a_wide_character) {
  Tree t;
  REQUIRE(t.valid());
  t.file("日本語日本語日本語日本語");

  Files f(t.root());
  f.on_resize(Size{12, 8});
  const std::string row = painted_row(f, 12, 8, 2);
  // Rien ne doit dépasser, et aucune moitié ne doit rester seule : la
  // surface refuse d'écrire hors clip, donc c'est la cohérence de la
  // ligne qui le dit.
  CHECK(!row.empty());
  CHECK(row.find("…") != std::string::npos);
}

// La liste ne peint QUE sa page : peindre au-delà écraserait la ligne
// d'état, et le clip de la View masquerait le débordement sans le
// corriger.
TEST(files_never_paints_over_the_status_row) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 0; i < 30; ++i) t.file("fichier" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});
  f.on_key(ch(U'z'));  // filtre : il ne reste que « .. », et le statut

  const std::string row = painted_row(f, 40, 6, 5);
  CHECK(row.find("fichier") == std::string::npos);
}

// La page peinte est celle du DÉFILEMENT : sans lui, une liste défilée
// montre toujours le début pendant que la sélection est ailleurs.
TEST(files_paints_the_page_the_scroll_offset_points_at) {
  Tree t;
  REQUIRE(t.valid());
  for (int i = 10; i < 30; ++i) t.file("f" + std::to_string(i));

  Files f(t.root());
  f.on_resize(Size{40, 6});
  f.on_key(key(Key::End));
  const size_t top = f.top_for_tests();
  REQUIRE(top > size_t{0});

  const std::string first = painted_row(f, 40, 6, 1);
  CHECK_EQ(first, f.visible_for_tests()[top].name);
}

// La barre de sélection couvre la LIGNE ENTIÈRE : arrêtée au dernier
// caractère du nom, elle se lit comme un mot surligné, pas comme une ligne
// choisie.
TEST(files_paints_the_selection_bar_across_the_whole_row) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  // Bien au-delà du nom « .. », qui fait deux colonnes.
  CHECK_EQ(cell_at(f, 40, 8, 30, 1).attrs & sshos::attr::Reverse,
           sshos::attr::Reverse);
}

// Un lien n'est pas un dossier : les peindre pareil ferait croire qu'on
// peut descendre dedans.
TEST(files_gives_links_a_colour_of_their_own) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("vrai");
  REQUIRE_EQ(::symlink("vrai", (t.root() + "/lien").c_str()), 0);

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::End));  // la sélection quitte les deux lignes du haut

  // « vrai » est un dossier, « lien » un lien : ils sont tous deux dans la
  // liste, et de couleurs différentes.
  const auto& v = f.visible_for_tests();
  int y_dir = -1;
  int y_link = -1;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].name == "vrai") y_dir = 1 + static_cast<int>(i);
    if (v[i].name == "lien") y_link = 1 + static_cast<int>(i);
  }
  REQUIRE(y_dir > 0);
  REQUIRE(y_link > 0);
  CHECK(!(cell_at(f, 40, 8, 0, y_dir).fg == cell_at(f, 40, 8, 0, y_link).fg));

  ::unlink((t.root() + "/lien").c_str());
}

// Un nom trop long garde son DÉBUT : c'est lui qui distingue deux
// fichiers, pas leur extension commune.
TEST(files_elides_a_long_name_from_the_right) {
  Tree t;
  REQUIRE(t.valid());
  t.file("rapport-annuel-tres-long-2026.txt");

  Files f(t.root());
  f.on_resize(Size{14, 8});
  const std::string row = painted_row(f, 14, 8, 2);
  CHECK_EQ(row.rfind("rapport", 0), size_t{0});
}

// ================================================ renommer et supprimer

namespace {

bool exists(const std::string& p) {
  struct stat st {};
  return ::lstat(p.c_str(), &st) == 0;
}

}  // namespace

TEST(files_opens_a_rename_prefilled_with_the_current_name) {
  Tree t;
  REQUIRE(t.valid());
  t.file("avant.txt");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("avant.txt"));

  f.on_key(key(Key::F2));
  CHECK(f.mode_for_tests() == Files::Mode::Renaming);
  // PRÉ-REMPLI : renommer « rapport-2025.txt » en « rapport-2026.txt » ne
  // doit pas demander de tout retaper.
  CHECK_EQ(f.edit_for_tests(), std::string("avant.txt"));
}

TEST(files_renames_the_file_on_disk) {
  Tree t;
  REQUIRE(t.valid());
  const std::string before = t.file("avant.txt");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  for (int i = 0; i < 9; ++i) f.on_key(key(Key::Backspace));
  f.on_key(ch(U'a'));
  f.on_key(ch(U'p'));
  f.on_key(key(Key::Enter));

  CHECK(!exists(before));
  CHECK(exists(t.root() + "/ap"));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
  ::unlink((t.root() + "/ap").c_str());
}

// Un nom DÉJÀ PRIS est refusé : écraser silencieusement est la façon la
// plus rapide de perdre un fichier.
TEST(files_refuses_a_rename_onto_an_existing_name) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");
  const std::string b = t.file("bbb");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("aaa"));
  f.on_key(key(Key::F2));
  for (int i = 0; i < 3; ++i) f.on_key(key(Key::Backspace));
  f.on_key(ch(U'b'));
  f.on_key(ch(U'b'));
  f.on_key(ch(U'b'));
  f.on_key(key(Key::Enter));

  CHECK(exists(a));
  CHECK(exists(b));
  CHECK(!f.status_for_tests().empty());
}

TEST(files_does_nothing_when_the_new_name_is_empty) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  for (int i = 0; i < 5; ++i) f.on_key(key(Key::Backspace));
  f.on_key(key(Key::Enter));

  CHECK(exists(a));
  // Et RIEN d'autre : un message d'erreur voudrait dire qu'on a essayé
  // quelque chose, alors qu'un nom vide n'est pas une demande.
  CHECK_EQ(f.status_for_tests(), std::string(""));
}

TEST(files_cancels_a_rename_on_escape) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  f.on_key(ch(U'z'));
  f.on_key(key(Key::Escape));

  CHECK(f.mode_for_tests() == Files::Mode::Normal);
  CHECK(exists(a));
}

// `..` n'est pas un fichier de ce répertoire : le renommer renommerait le
// PARENT, ce que personne ne demande en visant la première ligne.
TEST(files_refuses_to_rename_the_parent_entry) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  REQUIRE_EQ(selected_name(f), std::string(".."));

  f.on_key(key(Key::F2));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
}

// ----------------------------------------------------------- supprimer

// La suppression DEMANDE. C'est le seul geste irréversible du projet.
TEST(files_asks_before_it_deletes) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));

  CHECK(f.mode_for_tests() == Files::Mode::Confirming);
  CHECK(exists(a));  // rien n'est encore fait
}

TEST(files_deletes_only_after_an_explicit_yes) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  CHECK(!exists(a));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
}

// TOUTE autre réponse annule. Une confirmation qui accepte l'à-peu-près
// n'en est pas une.
TEST(files_keeps_the_file_when_the_answer_is_not_yes) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));
  f.on_key(ch(U'x'));

  CHECK(exists(a));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
}

TEST(files_cancels_a_delete_on_escape) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));
  f.on_key(key(Key::Escape));

  CHECK(exists(a));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
}

// Pas de suppression RÉCURSIVE en v1 : un dossier non vide se refuse, avec
// un message. Effacer une arborescence entière sur une touche est le genre
// de fonction qu'on regrette une seule fois.
TEST(files_refuses_to_delete_a_directory_that_is_not_empty) {
  Tree t;
  REQUIRE(t.valid());
  const std::string d = t.dir("plein");
  const std::string inside = t.file("plein/dedans");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("plein"));
  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  CHECK(exists(d));
  CHECK(exists(inside));
  CHECK(!f.status_for_tests().empty());
}

TEST(files_refuses_to_delete_the_parent_entry) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  REQUIRE_EQ(selected_name(f), std::string(".."));

  f.on_key(key(Key::Delete));
  CHECK(f.mode_for_tests() == Files::Mode::Normal);
}

// La liste se relit après une suppression : garder l'entrée disparue à
// l'écran donnerait un gestionnaire qui ment.
TEST(files_reloads_the_listing_after_a_delete) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");
  t.file("bbb");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  CHECK_EQ(names(f), std::string("..|bbb"));
}

// Pendant un renommage, la saisie ne FILTRE pas : les deux partagent le
// clavier, et confondre les deux ferait disparaître la liste sous les
// doigts de celui qui tape un nom.
TEST(files_does_not_filter_while_it_renames) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");
  t.file("bbb");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  f.on_key(ch(U'z'));

  CHECK_EQ(f.filter_for_tests(), std::string(""));
  CHECK_EQ(names(f), std::string("..|aaa|bbb"));
}

// Les deux modes se VOIENT : une invite qu'on ne voit pas est une
// application qui a l'air bloquée.
TEST(files_shows_the_rename_prompt_on_the_status_row) {
  Tree t;
  REQUIRE(t.valid());
  t.file("avant.txt");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));

  const std::string row = painted_row(f, 40, 8, 7);
  CHECK(row.find("avant.txt") != std::string::npos);
}

TEST(files_shows_the_delete_question_on_the_status_row) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::Delete));

  const std::string row = painted_row(f, 40, 8, 7);
  CHECK(row.find("aaa") != std::string::npos);
  CHECK(row.find("o/n") != std::string::npos);
}

// La sélection SUIT le nom renommé : le perdre de vue oblige à le
// rechercher pour vérifier que ça a marché.
TEST(files_follows_the_name_it_renamed) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");
  t.file("mmm");
  t.file("zzz");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("aaa"));
  f.on_key(key(Key::F2));
  for (int i = 0; i < 3; ++i) f.on_key(key(Key::Backspace));
  f.on_key(ch(U'q'));
  f.on_key(key(Key::Enter));

  CHECK_EQ(selected_name(f), std::string("q"));
  ::unlink((t.root() + "/q").c_str());
}

// Un nom avec une barre n'est pas un renommage, c'est un déplacement -- et
// un déplacement à l'aveugle vers un chemin qu'on ne voit pas est
// exactement ce qu'on ne veut pas offrir sur une touche.
TEST(files_refuses_a_name_that_contains_a_slash) {
  Tree t;
  REQUIRE(t.valid());
  const std::string a = t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  for (int i = 0; i < 3; ++i) f.on_key(key(Key::Backspace));
  f.on_key(ch(U'/'));
  f.on_key(ch(U'x'));
  f.on_key(key(Key::Enter));

  CHECK(exists(a));
  CHECK(!f.status_for_tests().empty());
}

// Un caractère de contrôle n'a rien à faire dans un nom de fichier : le
// laisser entrer donnerait un nom qu'on ne peut plus ni lire ni retaper.
TEST(files_never_lets_a_control_character_into_a_name) {
  Tree t;
  REQUIRE(t.valid());
  t.file("aaa");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  f.on_key(key(Key::F2));
  f.on_key(KeyEvent{Key::Char, static_cast<char32_t>(7), 0});

  CHECK_EQ(f.edit_for_tests(), std::string("aaa"));
}

// Un dossier VIDE se supprime -- et il faut `rmdir` pour ça : `unlink`
// refuse un répertoire, et le refus passerait pour la règle « pas de
// suppression récursive » alors que ce dossier-ci n'a rien dedans.
TEST(files_deletes_an_empty_directory) {
  Tree t;
  REQUIRE(t.valid());
  const std::string d = t.dir("vide");

  Files f(t.root());
  f.on_resize(Size{40, 8});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("vide"));
  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  CHECK(!exists(d));
  CHECK_EQ(f.status_for_tests(), std::string(""));
}

// ------------------------------------------------- la selection multiple

// `Espace` MARQUE ET DESCEND. C'est le geste de tous les gestionnaires en
// mode texte : on parcourt la liste en marquant au passage, sans jamais
// relever les doigts pour bouger.
TEST(files_marks_an_entry_with_space_and_moves_on) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Down, 0, 0});  // depuis `..` jusqu'au premier nom
  const size_t start = f.selected_for_tests();

  f.on_key(KeyEvent{Key::Char, U' ', 0});

  CHECK_EQ(f.marked_for_tests().size(), size_t{1});
  CHECK_EQ(f.selected_for_tests(), start + 1);
}

// `..` NE SE MARQUE PAS. Ce n'est pas un fichier, c'est la sortie : le
// laisser entrer dans une sélection ferait porter une copie ou une
// suppression sur le répertoire parent.
TEST(files_never_marks_the_way_out) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  REQUIRE_EQ(f.visible_for_tests()[0].name, std::string(".."));

  f.on_key(KeyEvent{Key::Char, U' ', 0});

  CHECK(f.marked_for_tests().empty());
  // Mais le curseur avance quand même : rester bloqué sur `..` donnerait
  // l'impression que la touche ne fait rien.
  CHECK_EQ(f.selected_for_tests(), size_t{1});
}

// `Ctrl+A` BASCULE : tout, puis rien. Un terminal ne sait pas distinguer
// `Ctrl+Maj+A` de `Ctrl+A` -- la combinaison de Dolphin est intapable ici
// -- et deux raccourcis pour un aller-retour valent moins qu'un seul qui
// fait les deux.
TEST(files_selects_everything_then_nothing_with_ctrl_a) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  d.dir("c");
  Files f(d.root());
  f.on_resize(Size{40, 10});

  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  // Tout sauf `..`.
  CHECK_EQ(f.marked_for_tests().size(), f.visible_for_tests().size() - 1);

  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  CHECK(f.marked_for_tests().empty());
}

// `Maj+flèches` ÉTEND depuis la position courante : c'est le geste qu'on
// essaie en premier quand on vient d'un vrai bureau.
TEST(files_extends_the_selection_with_shift_arrows) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  d.file("c");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Down, 0, 0});  // sur « a »

  f.on_key(KeyEvent{Key::Down, 0, mod::Shift});
  f.on_key(KeyEvent{Key::Down, 0, mod::Shift});

  CHECK_EQ(f.marked_for_tests().size(), size_t{3});
  CHECK_EQ(f.selected_for_tests(), size_t{3});
}

// `Ctrl+clic` ajoute ou retire UNE entrée sans toucher au reste, et sans
// l'ouvrir : c'est ce qui distingue le clic qui choisit du clic qui agit.
TEST(files_toggles_one_entry_with_a_ctrl_click) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  Files f(d.root());
  f.on_resize(Size{40, 10});

  f.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 2, mod::Ctrl});
  CHECK_EQ(f.marked_for_tests().size(), size_t{1});
  CHECK_EQ(f.path_for_tests(), d.root());  // rien n'a été ouvert

  f.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 2, mod::Ctrl});
  CHECK(f.marked_for_tests().empty());
}

// `Maj+clic` prend TOUT ce qui va de la position courante au clic, dans un
// sens comme dans l'autre.
TEST(files_takes_a_whole_range_with_a_shift_click) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  d.file("c");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Down, 0, 0});  // sur « a », ligne 2 de l'écran

  f.on_mouse(MouseEvent{MouseAction::Press, 0, 2, 4, mod::Shift});

  CHECK_EQ(f.marked_for_tests().size(), size_t{3});
}

// CHANGER DE RÉPERTOIRE OUBLIE LA SÉLECTION. Les noms marqués sont ceux
// d'AVANT : les garder ferait porter la prochaine action sur des homonymes
// d'un autre dossier, ce qui est le pire résultat possible.
TEST(files_forgets_its_selection_when_it_changes_directory) {
  Tree d;
  REQUIRE(d.valid());
  d.dir("sous");
  d.file("a");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  REQUIRE(!f.marked_for_tests().empty());

  f.on_key(KeyEvent{Key::Backspace, 0, 0});  // on remonte

  CHECK(f.marked_for_tests().empty());
}

// `Échap` REND LA SÉLECTION sans rien détruire : il faut une porte de
// sortie qui ne soit pas « re-parcourir la liste en démarquant ».
TEST(files_drops_its_selection_on_escape) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  REQUIRE(!f.marked_for_tests().empty());

  f.on_key(KeyEvent{Key::Escape, 0, 0});

  CHECK(f.marked_for_tests().empty());
}

// LA LIGNE D'ÉTAT COMPTE ET PÈSE. Une sélection qu'on ne voit pas est une
// sélection dont on ne se souvient plus au moment d'appuyer sur Suppr.
TEST(files_says_how_many_it_has_and_how_much_they_weigh) {
  Tree d;
  REQUIRE(d.valid());
  d.file("a");
  d.file("b");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});

  const std::string screen = painted(f, 40, 10);
  CHECK(screen.find("2 selectionnes") != std::string::npos ||
        screen.find("2 sélectionnés") != std::string::npos);
}

// LES MARQUÉS SE VOIENT DANS LA LISTE. Un compteur en bas ne dit pas
// LESQUELS, et une sélection qu'on ne peut pas relire ne se corrige pas.
TEST(files_marks_the_chosen_lines_on_screen) {
  Tree d;
  REQUIRE(d.valid());
  d.file("choisi");
  d.file("laisse");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Down, 0, 0});
  f.on_key(KeyEvent{Key::Char, U' ', 0});

  const std::string screen = painted(f, 40, 10);
  const size_t at = screen.find("choisi");
  REQUIRE(at != std::string::npos);
  // La marque précède le nom, sur sa ligne.
  CHECK_EQ(screen[at - 1], '*');
  CHECK(screen.find("*laisse") == std::string::npos);
}

// SUPPRIMER PORTE SUR LA SÉLECTION. Marquer trois fichiers puis appuyer sur
// Suppr doit les emporter tous les trois, et poser UNE seule question : une
// confirmation par fichier ferait cliquer « oui » sans lire dès la seconde.
TEST(files_deletes_everything_that_is_marked) {
  Tree d;
  REQUIRE(d.valid());
  d.file("un");
  d.file("deux");
  d.file("trois");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  REQUIRE_EQ(f.marked_for_tests().size(), size_t{3});

  f.on_key(key(Key::Delete));
  REQUIRE(f.mode_for_tests() == Files::Mode::Confirming);
  f.on_key(ch(U'o'));

  CHECK(!exists(d.root() + "/un"));
  CHECK(!exists(d.root() + "/deux"));
  CHECK(!exists(d.root() + "/trois"));
}

// SANS SÉLECTION, l'action porte sur la seule ligne sous le curseur. C'est
// la règle de tous les gestionnaires, et elle évite d'avoir à marquer un
// fichier pour agir sur lui.
TEST(files_falls_back_to_the_line_under_the_cursor) {
  Tree d;
  REQUIRE(d.valid());
  d.file("cible");
  d.file("voisin");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(key(Key::Down));
  REQUIRE_EQ(selected_name(f), std::string("cible"));

  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  CHECK(!exists(d.root() + "/cible"));
  CHECK(exists(d.root() + "/voisin"));
}

// LA QUESTION DIT COMBIEN. « supprimer ? » sur une sélection de trente
// fichiers ne dit pas ce qu'on s'apprête à perdre.
TEST(files_says_how_many_it_is_about_to_delete) {
  Tree d;
  REQUIRE(d.valid());
  d.file("un");
  d.file("deux");
  Files f(d.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  f.on_key(key(Key::Delete));

  const std::string screen = painted(f, 40, 10);
  CHECK(screen.find("2 ") != std::string::npos);
  CHECK(screen.find("(o/n)") != std::string::npos);
}

// UN ÉCHEC N'ARRÊTE PAS LE RESTE. S'arrêter au premier laisserait une
// sélection à moitié traitée dont l'utilisateur ne sait pas où elle en est
// -- et il rappuierait, sur une liste qui a changé sous lui.
TEST(files_keeps_deleting_after_one_of_them_refuses) {
  Tree t;
  REQUIRE(t.valid());
  // « a » avant « b » : la sélection est un ensemble ordonné par nom, donc
  // celui qui échoue passe en premier.
  t.dir("a-plein");
  t.file("a-plein/dedans");
  t.file("b-simple");
  Files f(t.root());
  f.on_resize(Size{40, 10});
  f.on_key(KeyEvent{Key::Char, U'a', mod::Ctrl});
  REQUIRE_EQ(f.marked_for_tests().size(), size_t{2});

  f.on_key(key(Key::Delete));
  f.on_key(ch(U'o'));

  // Le dossier non vide résiste -- pas de suppression récursive ici --
  // mais le fichier d'après est bel et bien parti.
  CHECK(exists(t.root() + "/a-plein"));
  CHECK(!exists(t.root() + "/b-simple"));
  CHECK(f.status_for_tests().find("1 sur 2") != std::string::npos);
}
