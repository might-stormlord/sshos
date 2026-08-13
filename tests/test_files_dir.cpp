#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/files/dir.hpp"
#include "harness.hpp"

using sshos::DirEntry;
using sshos::DirListing;
using sshos::EntryKind;
using sshos::filter_entries;
using sshos::join_path;
using sshos::parent_path;
using sshos::read_dir;
using sshos::sort_entries;

namespace {

// Un répertoire fabriqué de toutes pièces, détruit à la sortie du cas.
// RAII plutôt qu'un nettoyage en fin de fonction : le harnais rend la main
// par un `return` nu sur un REQUIRE raté, et tout nettoyage écrit à la
// main serait sauté ce jour-là.
class TempDir {
 public:
  TempDir() {
    char tpl[] = "/tmp/sshos-files-XXXXXX";
    const char* made = ::mkdtemp(tpl);
    if (made != nullptr) path_ = made;
  }
  ~TempDir() {
    if (path_.empty()) return;
    // Un seul niveau : c'est tout ce que ces cas fabriquent.
    for (const std::string& n : made_) {
      const std::string p = path_ + "/" + n;
      ::rmdir(p.c_str());
      ::unlink(p.c_str());
    }
    ::rmdir(path_.c_str());
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

  void file(const std::string& name) {
    const std::string p = path_ + "/" + name;
    const int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) ::close(fd);
    made_.push_back(name);
  }
  void dir(const std::string& name) {
    ::mkdir((path_ + "/" + name).c_str(), 0700);
    made_.push_back(name);
  }

 private:
  std::string path_;
  std::vector<std::string> made_;
};

std::vector<std::string> names_of(const std::vector<DirEntry>& v) {
  std::vector<std::string> out;
  out.reserve(v.size());
  for (const DirEntry& e : v) out.push_back(e.name);
  return out;
}

std::string joined(const std::vector<DirEntry>& v) {
  std::string out;
  for (const DirEntry& e : v) {
    if (!out.empty()) out.push_back('|');
    out += e.name;
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------- la lecture

TEST(dir_reads_what_is_there) {
  TempDir t;
  REQUIRE(t.valid());
  t.file("alpha.txt");
  t.dir("beta");

  const DirListing l = read_dir(t.path(), false);
  CHECK_EQ(l.error, std::string(""));
  const std::vector<std::string> n = names_of(l.entries);
  CHECK(std::find(n.begin(), n.end(), "alpha.txt") != n.end());
  CHECK(std::find(n.begin(), n.end(), "beta") != n.end());
}

// `..` est TOUJOURS là, et toujours en tête : c'est la sortie, et une
// sortie qu'il faut chercher n'en est pas une.
TEST(dir_always_offers_the_parent_first) {
  TempDir t;
  REQUIRE(t.valid());
  t.file("a");

  const DirListing l = read_dir(t.path(), false);
  REQUIRE(!l.entries.empty());
  CHECK_EQ(l.entries[0].name, std::string(".."));
  CHECK(l.entries[0].kind == EntryKind::Dir);
}

// Sauf à la racine, où l'offrir ferait tourner en rond.
TEST(dir_offers_no_parent_at_the_root) {
  const DirListing l = read_dir("/", false);
  const std::vector<std::string> n = names_of(l.entries);
  CHECK(std::find(n.begin(), n.end(), "..") == n.end());
}

TEST(dir_hides_the_hidden_files_until_asked) {
  TempDir t;
  REQUIRE(t.valid());
  t.file("visible");
  t.file(".cache");

  const std::vector<std::string> without = names_of(read_dir(t.path(), false).entries);
  CHECK(std::find(without.begin(), without.end(), ".cache") == without.end());

  const std::vector<std::string> with = names_of(read_dir(t.path(), true).entries);
  CHECK(std::find(with.begin(), with.end(), ".cache") != with.end());
}

// Un répertoire illisible rend une liste vide ET un message. Ne rien dire
// laisserait l'utilisateur devant un panneau blanc sans savoir pourquoi.
TEST(dir_names_the_error_when_it_cannot_read) {
  const DirListing l = read_dir("/proc/1/fdinfo/../../../root-inexistant", false);
  CHECK(!l.error.empty());
  CHECK(l.entries.empty());
}

// ---------------------------------------------------------------- le tri

// DOSSIERS D'ABORD : c'est la convention de tous les gestionnaires, et
// mélanger les deux rend une arborescence profonde illisible.
TEST(dir_sorts_directories_before_files) {
  std::vector<DirEntry> v = {
      {"zeta.txt", EntryKind::File, 0},
      {"alpha", EntryKind::Dir, 0},
      {"beta.txt", EntryKind::File, 0},
      {"gamma", EntryKind::Dir, 0},
  };
  sort_entries(v);
  CHECK_EQ(joined(v), std::string("alpha|gamma|beta.txt|zeta.txt"));
}

TEST(dir_sorts_case_insensitively) {
  std::vector<DirEntry> v = {
      {"Banane", EntryKind::File, 0},
      {"ananas", EntryKind::File, 0},
      {"Cerise", EntryKind::File, 0},
  };
  sort_entries(v);
  CHECK_EQ(joined(v), std::string("ananas|Banane|Cerise"));
}

// STABLE : deux noms qui ne diffèrent que par la casse doivent garder un
// ordre déterministe, sinon la liste saute d'un rafraîchissement à l'autre
// sous les yeux de l'utilisateur.
TEST(dir_keeps_a_deterministic_order_between_two_spellings_of_a_name) {
  std::vector<DirEntry> v = {
      {"README", EntryKind::File, 0},
      {"readme", EntryKind::File, 0},
  };
  std::vector<DirEntry> w = v;
  sort_entries(v);
  sort_entries(w);
  CHECK_EQ(joined(v), joined(w));
}

TEST(dir_keeps_the_parent_in_front_whatever_the_sort) {
  std::vector<DirEntry> v = {
      {"alpha", EntryKind::Dir, 0},
      {"..", EntryKind::Dir, 0},
  };
  sort_entries(v);
  CHECK_EQ(v[0].name, std::string(".."));
}

// -------------------------------------------------------------- le filtre

TEST(dir_filters_on_a_substring_whatever_the_case) {
  const std::vector<DirEntry> v = {
      {"..", EntryKind::Dir, 0},
      {"Rapport.txt", EntryKind::File, 0},
      {"notes.md", EntryKind::File, 0},
  };
  CHECK_EQ(joined(filter_entries(v, "rap")), std::string("..|Rapport.txt"));
  CHECK_EQ(joined(filter_entries(v, "RAP")), std::string("..|Rapport.txt"));
}

// `..` survit TOUJOURS au filtre : c'est la sortie, pas un résultat de
// recherche. Sans cela, un filtre qui ne trouve rien enferme dans le
// répertoire.
TEST(dir_never_filters_away_the_way_out) {
  const std::vector<DirEntry> v = {
      {"..", EntryKind::Dir, 0},
      {"alpha", EntryKind::File, 0},
  };
  CHECK_EQ(joined(filter_entries(v, "zzz")), std::string(".."));
}

TEST(dir_keeps_everything_when_the_filter_is_empty) {
  const std::vector<DirEntry> v = {
      {"..", EntryKind::Dir, 0},
      {"alpha", EntryKind::File, 0},
  };
  CHECK_EQ(filter_entries(v, "").size(), size_t{2});
}

// ------------------------------------------------------------- les chemins

TEST(dir_walks_up_to_the_parent) {
  CHECK_EQ(parent_path("/home/storm/dev"), std::string("/home/storm"));
  CHECK_EQ(parent_path("/home"), std::string("/"));
}

// La racine n'a pas de parent : rendre autre chose qu'elle-même ferait
// remonter dans le vide.
TEST(dir_stays_at_the_root_when_it_walks_up_from_it) {
  CHECK_EQ(parent_path("/"), std::string("/"));
}

TEST(dir_tolerates_a_trailing_slash_when_it_walks_up) {
  CHECK_EQ(parent_path("/home/storm/"), std::string("/home"));
}

// Jamais de double barre : un chemin avec `//` s'affiche mal et se compare
// mal à celui que le noyau rendrait.
TEST(dir_never_joins_a_double_slash) {
  CHECK_EQ(join_path("/home", "storm"), std::string("/home/storm"));
  CHECK_EQ(join_path("/", "home"), std::string("/home"));
  CHECK_EQ(join_path("/home/", "storm"), std::string("/home/storm"));
}

// ------------------------------- cinq trous montrés par les mutations

// L'ordre entre deux orthographes d'un même nom est ENTIÈREMENT
// déterminé : c'est le nom brut qui départage ce que le pliage de casse
// rend égal.
TEST(dir_settles_two_spellings_by_the_raw_name) {
  std::vector<DirEntry> v = {
      {"readme", EntryKind::File, 0},
      {"README", EntryKind::File, 0},
  };
  sort_entries(v);
  CHECK_EQ(joined(v), std::string("README|readme"));
}

// Le pliage va vers le BAS de la table ASCII, et le sens SE VOIT : `_`
// (0x5F) tombe entre les majuscules et les minuscules. Plié vers le bas,
// il passe avant « a » (0x61) donc avant « Alpha » ; plié vers le haut, il
// passerait après « A » (0x41). Ce test fige le sens choisi -- aucun des
// deux n'est plus juste que l'autre, mais en changer ferait sauter l'ordre
// de tout répertoire contenant un nom en `_`.
TEST(dir_folds_towards_the_lower_case) {
  std::vector<DirEntry> v = {
      {"_ancien", EntryKind::File, 0},
      {"Alpha", EntryKind::File, 0},
  };
  sort_entries(v);
  CHECK_EQ(joined(v), std::string("_ancien|Alpha"));
}

// Un nom qui est le PRÉFIXE d'un autre passe devant lui.
TEST(dir_puts_a_prefix_before_the_name_that_extends_it) {
  std::vector<DirEntry> v = {
      {"notes", EntryKind::File, 0},
      {"note", EntryKind::File, 0},
  };
  sort_entries(v);
  CHECK_EQ(joined(v), std::string("note|notes"));
}

// `.` n'a rien à faire dans la liste : il ne mène nulle part, et il
// occuperait la première place utile.
TEST(dir_never_lists_the_current_directory) {
  TempDir t;
  REQUIRE(t.valid());
  t.file("a");

  const std::vector<std::string> n = names_of(read_dir(t.path(), true).entries);
  CHECK(std::find(n.begin(), n.end(), ".") == n.end());
}

// La liste rendue par la lecture est DÉJÀ triée : l'appelant ne doit pas
// avoir à s'en souvenir, et `readdir` ne garantit aucun ordre.
// Assez d'entrées, et créées dans le DÉSORDRE, pour que l'ordre brut de
// `readdir` -- qui n'est ni celui de création ni l'alphabétique -- ne
// puisse pas coïncider par hasard avec le résultat attendu. Une première
// version avec trois entrées y était arrivée, et ne mordait donc pas.
TEST(dir_returns_an_already_sorted_listing) {
  TempDir t;
  REQUIRE(t.valid());
  t.file("zeta");
  t.dir("sierra");
  t.file("alpha");
  t.file("mike");
  t.dir("bravo");
  t.file("kilo");
  t.dir("delta");
  t.file("echo");

  const DirListing l = read_dir(t.path(), false);
  CHECK_EQ(joined(l.entries),
           std::string("..|bravo|delta|sierra|alpha|echo|kilo|mike|zeta"));
}
