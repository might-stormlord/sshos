#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "apps/files/files.hpp"
#include "harness.hpp"

using sshos::Files;
using sshos::Key;
using sshos::KeyEvent;
using sshos::MouseAction;
using sshos::MouseEvent;
using sshos::Size;

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
