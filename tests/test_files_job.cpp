#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "apps/files/job.hpp"
#include "harness.hpp"

using sshos::FileJob;
using sshos::FileOp;

namespace {

// Un arbre fabriqué de toutes pièces, détruit à la sortie du cas. RAII :
// le harnais rend la main par un `return` nu sur un REQUIRE raté, et tout
// nettoyage écrit à la main serait sauté ce jour-là.
class Tree {
 public:
  Tree() {
    char tpl[] = "/tmp/sshos-copy-XXXXXX";
    const char* made = ::mkdtemp(tpl);
    if (made != nullptr) root_ = made;
  }
  ~Tree() {
    // Deux passes : les fichiers d'abord, puis les répertoires du plus
    // profond au moins profond.
    for (auto it = made_.rbegin(); it != made_.rend(); ++it) {
      ::unlink(it->c_str());
    }
    for (auto it = made_.rbegin(); it != made_.rend(); ++it) {
      ::rmdir(it->c_str());
    }
    ::rmdir(root_.c_str());
  }
  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;

  const std::string& root() const { return root_; }
  bool valid() const { return !root_.empty(); }

  std::string dir(const std::string& rel) {
    const std::string p = root_ + "/" + rel;
    ::mkdir(p.c_str(), 0755);
    made_.push_back(p);
    return p;
  }
  std::string file(const std::string& rel, const std::string& body = {}) {
    const std::string p = root_ + "/" + rel;
    const int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd >= 0) {
      if (!body.empty()) {
        const ssize_t n = ::write(fd, body.data(), body.size());
        (void)n;
      }
      ::close(fd);
    }
    made_.push_back(p);
    return p;
  }

 private:
  std::string root_;
  std::vector<std::string> made_;
};

std::string read_all(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return out;
}

bool exists(const std::string& p) {
  struct stat st {};
  return ::lstat(p.c_str(), &st) == 0;
}

// Fait tourner le travail jusqu'au bout, en comptant les tranches. Bornée :
// un travail qui n'avance pas doit faire échouer le cas, pas le figer.
int run_to_end(FileJob& job, size_t budget) {
  int steps = 0;
  while (job.step(budget) && steps < 100000) ++steps;
  return steps;
}

}  // namespace

TEST(copy_moves_the_bytes_of_a_file) {
  Tree t;
  REQUIRE(t.valid());
  t.file("source", "bonjour");
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/source"}, t.root() + "/cible", FileOp::Copy);
  run_to_end(job, 1024);

  CHECK_EQ(read_all(t.root() + "/cible/source"), std::string("bonjour"));
  // Une copie laisse l'original en place.
  CHECK(exists(t.root() + "/source"));
  CHECK_EQ(job.failed(), 0);
}

// LA TRANCHE EST BORNÉE, et c'est toute la raison d'être de cette classe :
// le démon est mono-thread, et une copie qui ne rendrait la main qu'à la
// fin gèlerait toutes les fenêtres et tous les clients pendant sa durée.
TEST(copy_never_moves_more_than_its_budget_in_one_step) {
  Tree t;
  REQUIRE(t.valid());
  t.file("gros", std::string(50000, 'x'));
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/gros"}, t.root() + "/cible", FileOp::Copy);
  // Une tranche ouvre le fichier, la suivante en copie 1000 octets : après
  // deux tranches, il en reste largement à faire.
  job.step(1000);
  job.step(1000);
  CHECK(job.active());

  const int steps = run_to_end(job, 1000);
  CHECK(steps > 10);
  CHECK_EQ(read_all(t.root() + "/cible/gros").size(), size_t{50000});
}

TEST(copy_walks_a_whole_tree) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("arbre");
  t.dir("arbre/branche");
  t.file("arbre/a", "un");
  t.file("arbre/branche/b", "deux");
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/arbre"}, t.root() + "/cible", FileOp::Copy);
  run_to_end(job, 4096);

  CHECK_EQ(read_all(t.root() + "/cible/arbre/a"), std::string("un"));
  CHECK_EQ(read_all(t.root() + "/cible/arbre/branche/b"), std::string("deux"));
  CHECK_EQ(job.failed(), 0);
  ::unlink((t.root() + "/cible/arbre/branche/b").c_str());
  ::unlink((t.root() + "/cible/arbre/a").c_str());
  ::rmdir((t.root() + "/cible/arbre/branche").c_str());
  ::rmdir((t.root() + "/cible/arbre").c_str());
}

// DÉPLACER, C'EST RENOMMER quand c'est possible : recopier deux
// gigaoctets pour les remettre trois centimètres plus loin sur le même
// disque serait absurde.
TEST(copy_moves_a_file_without_copying_it_when_it_can) {
  Tree t;
  REQUIRE(t.valid());
  t.file("source", "contenu");
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/source"}, t.root() + "/cible", FileOp::Move);
  // UNE SEULE tranche suffit : `rename()` ne copie rien.
  job.step(1);
  job.step(1);

  CHECK(!exists(t.root() + "/source"));
  CHECK_EQ(read_all(t.root() + "/cible/source"), std::string("contenu"));
}

// UNE COPIE N'ÉCRASE JAMAIS. Écraser sans le dire est la pire chose qu'un
// gestionnaire de fichiers puisse faire.
TEST(copy_refuses_to_overwrite_and_says_so) {
  Tree t;
  REQUIRE(t.valid());
  t.file("source", "neuf");
  t.dir("cible");
  t.file("cible/source", "ancien");

  FileJob job;
  job.start({t.root() + "/source"}, t.root() + "/cible", FileOp::Copy);
  run_to_end(job, 1024);

  CHECK_EQ(read_all(t.root() + "/cible/source"), std::string("ancien"));
  CHECK_EQ(job.failed(), 1);
  CHECK(!job.error().empty());
}

// UN ÉCHEC N'ARRÊTE PAS LE RESTE : s'arrêter au premier laisserait une
// copie à moitié faite dont l'utilisateur ne saurait pas où elle en est.
TEST(copy_keeps_going_after_one_of_them_fails) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a", "un");
  t.file("b", "deux");
  t.dir("cible");
  t.file("cible/a", "deja la");

  FileJob job;
  job.start({t.root() + "/a", t.root() + "/b"}, t.root() + "/cible",
            FileOp::Copy);
  run_to_end(job, 1024);

  CHECK_EQ(job.failed(), 1);
  CHECK_EQ(read_all(t.root() + "/cible/b"), std::string("deux"));
}

// UNE SOURCE QUI N'EXISTE PLUS EST UNE ERREUR, pas un plantage : la liste
// a pu être choisie avant qu'un autre programme n'efface le fichier.
TEST(copy_reports_a_source_that_vanished) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/jamais-existe"}, t.root() + "/cible", FileOp::Copy);
  run_to_end(job, 1024);

  CHECK_EQ(job.failed(), 1);
  CHECK(!job.error().empty());
}

// UN TRAVAIL ANNULÉ S'ARRÊTE TOUT DE SUITE, et ne laisse aucun descripteur
// derrière lui -- c'est la contrepartie du découpage en tranches.
TEST(copy_stops_dead_when_it_is_cancelled) {
  Tree t;
  REQUIRE(t.valid());
  t.file("gros", std::string(50000, 'x'));
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/gros"}, t.root() + "/cible", FileOp::Copy);
  job.step(100);
  job.step(100);
  REQUIRE(job.active());

  job.cancel();

  CHECK(!job.active());
  CHECK(!job.step(1000));
  CHECK(read_all(t.root() + "/cible/gros").size() < size_t{50000});
  ::unlink((t.root() + "/cible/gros").c_str());
}

// L'AVANCEMENT SE COMPTE. Une copie de deux minutes sans rien à l'écran
// passe pour un blocage.
TEST(copy_counts_what_it_has_done) {
  Tree t;
  REQUIRE(t.valid());
  t.file("a", "un");
  t.file("b", "deux");
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/a", t.root() + "/b"}, t.root() + "/cible",
            FileOp::Copy);
  CHECK_EQ(job.done(), 0);
  run_to_end(job, 1024);
  CHECK_EQ(job.done(), 2);
}

// LE NOM EN COURS EST LISIBLE PENDANT LA COPIE : « copie en cours » sans
// dire de quoi ne renseigne personne.
TEST(copy_names_the_file_it_is_working_on) {
  Tree t;
  REQUIRE(t.valid());
  t.file("le-gros-fichier", std::string(50000, 'x'));
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/le-gros-fichier"}, t.root() + "/cible",
            FileOp::Copy);
  job.step(100);

  CHECK_EQ(job.current(), std::string("le-gros-fichier"));
  job.cancel();
  ::unlink((t.root() + "/cible/le-gros-fichier").c_str());
}

// DÉPLACER ENTRE DEUX SYSTÈMES DE FICHIERS : `rename()` rend `EXDEV`, et
// il faut alors copier PUIS effacer. C'est le seul chemin où la source
// disparaît par notre fait, et il ne s'exerce qu'en franchissant un point
// de montage -- ici `/dev/shm`, qui est un tmpfs distinct de `/tmp`.
TEST(copy_moves_across_a_mount_point_by_copying_then_deleting) {
  Tree t;
  REQUIRE(t.valid());
  t.file("voyageur", "contenu");

  // Le cas ne vaut que si les deux sont bien sur des systèmes différents ;
  // sinon `rename()` suffirait et on ne testerait pas ce qu'on croit.
  struct stat a {};
  struct stat b {};
  REQUIRE_EQ(::stat(t.root().c_str(), &a), 0);
  if (::stat("/dev/shm", &b) != 0 || a.st_dev == b.st_dev) return;

  char tpl[] = "/dev/shm/sshos-xdev-XXXXXX";
  const char* dest = ::mkdtemp(tpl);
  REQUIRE(dest != nullptr);

  FileJob job;
  job.start({t.root() + "/voyageur"}, dest, FileOp::Move);
  run_to_end(job, 1024);

  const std::string arrived = std::string(dest) + "/voyageur";
  CHECK_EQ(read_all(arrived), std::string("contenu"));
  CHECK(!exists(t.root() + "/voyageur"));
  CHECK_EQ(job.failed(), 0);

  ::unlink(arrived.c_str());
  ::rmdir(dest);
}

// UNE SOURCE MANQUANTE N'ARRÊTE PAS CELLES D'APRÈS. C'est le même
// principe que la suppression : s'arrêter au premier laisserait le travail
// à moitié fait sans que personne sache où.
TEST(copy_keeps_going_after_a_source_that_does_not_exist) {
  Tree t;
  REQUIRE(t.valid());
  t.file("present", "la");
  t.dir("cible");

  FileJob job;
  job.start({t.root() + "/absent", t.root() + "/present"}, t.root() + "/cible",
            FileOp::Copy);
  run_to_end(job, 1024);

  CHECK_EQ(job.failed(), 1);
  CHECK_EQ(read_all(t.root() + "/cible/present"), std::string("la"));
  ::unlink((t.root() + "/cible/present").c_str());
}

// ANNULER REND LES DESCRIPTEURS. Une copie interrompue en garde deux
// ouverts ; les abandonner à chaque annulation finirait par épuiser la
// table du démon, qui ne pourrait plus accepter un seul client.
TEST(copy_gives_its_descriptors_back_when_cancelled) {
  Tree t;
  REQUIRE(t.valid());
  t.file("gros", std::string(50000, 'x'));
  t.dir("cible");

  const auto open_fds = []() {
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n;
  };
  const int before = open_fds();
  REQUIRE(before > 0);

  for (int i = 0; i < 8; ++i) {
    FileJob job;
    job.start({t.root() + "/gros"}, t.root() + "/cible", FileOp::Copy);
    job.step(100);
    job.cancel();
    ::unlink((t.root() + "/cible/gros").c_str());
  }

  CHECK_EQ(open_fds(), before);
}

// ------------------------------------------------- la suppression recursive

// UN DOSSIER PLEIN SE SUPPRIME. `rmdir` le refuse, et il n'y avait pas de
// descente : un dossier non vide était **insupprimable** depuis
// l'application, ce qui obligeait à sortir dans un terminal pour la moitié
// du ménage.
TEST(job_deletes_a_whole_tree) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("arbre");
  t.dir("arbre/branche");
  t.file("arbre/a", "un");
  t.file("arbre/branche/b", "deux");

  FileJob job;
  job.start({t.root() + "/arbre"}, std::string(), FileOp::Delete);
  run_to_end(job, 4096);

  CHECK(!exists(t.root() + "/arbre"));
  CHECK_EQ(job.failed(), 0);
}

// ELLE AVANCE PAR TRANCHES, comme la copie et pour la même raison : le
// démon est mono-thread, et effacer une arborescence de cent mille fichiers
// d'un seul appel gèlerait toutes les fenêtres et tous les clients.
TEST(job_deletes_lazily_without_walking_everything_first) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("gros");
  for (int i = 0; i < 40; ++i) {
    t.file("gros/f" + std::to_string(i), "x");
  }

  FileJob job;
  job.start({t.root() + "/gros"}, std::string(), FileOp::Delete);
  // Une tranche ne traite qu'un élément : après deux, il reste tout le
  // reste, et le dossier est donc toujours là.
  job.step(4096);
  job.step(4096);
  CHECK(job.active());
  CHECK(exists(t.root() + "/gros"));

  run_to_end(job, 4096);
  CHECK(!exists(t.root() + "/gros"));
}

// LE DOSSIER PART APRÈS SON CONTENU. L'ordre inverse ferait échouer chaque
// `rmdir` sur un dossier encore plein, et rendrait « N ont echoue » sur une
// arborescence pourtant parfaitement supprimable.
TEST(job_removes_a_directory_after_what_it_holds) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("nid");
  t.dir("nid/dedans");
  t.file("nid/dedans/oeuf", "x");

  FileJob job;
  job.start({t.root() + "/nid"}, std::string(), FileOp::Delete);
  run_to_end(job, 4096);

  CHECK(!exists(t.root() + "/nid"));
  CHECK_EQ(job.failed(), 0);
}

// UN ÉCHEC N'ARRÊTE PAS LE RESTE, et il se compte : c'est la même règle que
// la copie, et un droit refusé au milieu d'une arborescence ne doit pas
// laisser le travail à moitié fait sans que personne sache où.
TEST(job_keeps_deleting_after_one_of_them_fails) {
  Tree t;
  REQUIRE(t.valid());
  t.file("present", "la");

  FileJob job;
  job.start({t.root() + "/absent", t.root() + "/present"}, std::string(),
            FileOp::Delete);
  run_to_end(job, 4096);

  CHECK_EQ(job.failed(), 1);
  CHECK(!exists(t.root() + "/present"));
}

// UNE SUPPRESSION NE COPIE RIEN : elle n'ouvre aucun descripteur, donc une
// arborescence de deux gigaoctets s'efface aussi vite qu'une vide.
TEST(job_opens_nothing_while_deleting) {
  Tree t;
  REQUIRE(t.valid());
  t.file("gros", std::string(50000, 'x'));

  const auto open_fds = []() {
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n;
  };
  const int before = open_fds();
  REQUIRE(before > 0);

  FileJob job;
  job.start({t.root() + "/gros"}, std::string(), FileOp::Delete);
  run_to_end(job, 4096);

  CHECK_EQ(open_fds(), before);
  CHECK(!exists(t.root() + "/gros"));
}

// SUPPRIMER UN LIEN N'EFFACE PAS SA CIBLE. C'est le pire dégât qu'un
// gestionnaire de fichiers puisse faire : un `~/Documents -> /data/docs`
// effacé « pour faire de la place » emporterait /data/docs tout entier.
// La garde est `lstat`, jamais `stat` : le premier voit le LIEN, le second
// voit à travers.
TEST(job_deletes_a_symlink_without_following_it) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("precieux");
  t.file("precieux/tresor", "a garder");
  const std::string lien = t.root() + "/raccourci";
  REQUIRE_EQ(::symlink((t.root() + "/precieux").c_str(), lien.c_str()), 0);

  FileJob job;
  job.start({lien}, std::string(), FileOp::Delete);
  run_to_end(job, 4096);

  CHECK(!exists(lien));
  CHECK_EQ(job.failed(), 0);
  // LA CIBLE EST INTACTE, contenu compris.
  CHECK(exists(t.root() + "/precieux"));
  CHECK_EQ(read_all(t.root() + "/precieux/tresor"), std::string("a garder"));
}

// ET LE DÉPLACER NE LE DÉRÉFÉRENCE PAS non plus : c'est le lien qui bouge,
// pas ce qu'il désigne.
TEST(job_moves_a_symlink_as_a_symlink) {
  Tree t;
  REQUIRE(t.valid());
  t.dir("cible");
  t.dir("ailleurs");
  const std::string lien = t.root() + "/raccourci";
  REQUIRE_EQ(::symlink((t.root() + "/cible").c_str(), lien.c_str()), 0);

  FileJob job;
  job.start({lien}, t.root() + "/ailleurs", FileOp::Move);
  run_to_end(job, 4096);

  struct stat st {};
  REQUIRE_EQ(::lstat((t.root() + "/ailleurs/raccourci").c_str(), &st), 0);
  CHECK(S_ISLNK(st.st_mode));
  CHECK(exists(t.root() + "/cible"));
  ::unlink((t.root() + "/ailleurs/raccourci").c_str());
}
