#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "common/paths.hpp"
#include "daemon/journal.hpp"
#include "harness.hpp"

namespace {

struct HorlogeFigee : sshos::Platform {
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(1786370700));
  }
  std::chrono::steady_clock::time_point steady_now() const override { return {}; }
  std::string read_file(std::string_view) const override { return {}; }
};

std::string lire(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  std::string out;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  ::close(fd);
  return out;
}

// Un repertoire a soi, efface a la fin du cas.
class Bac {
 public:
  Bac() {
    char tpl[] = "/tmp/sshos-journal-XXXXXX";
    const char* fait = ::mkdtemp(tpl);
    if (fait != nullptr) dir_ = fait;
  }
  ~Bac() {
    if (dir_.empty()) return;
    ::unlink((dir_ + "/journal.log").c_str());
    ::unlink((dir_ + "/share/termos/journal.log").c_str());
    ::rmdir((dir_ + "/share/termos").c_str());
    ::rmdir((dir_ + "/share").c_str());
    ::rmdir(dir_.c_str());
  }
  Bac(const Bac&) = delete;
  Bac& operator=(const Bac&) = delete;
  const std::string& dir() const { return dir_; }
  std::string fichier() const { return dir_ + "/journal.log"; }

 private:
  std::string dir_;
};

size_t lignes(const std::string& texte) {
  size_t n = 0;
  for (char c : texte) {
    if (c == '\n') ++n;
  }
  return n;
}

}  // namespace

// UNE LIGNE DATEE, ET L'EVENEMENT TEL QUEL. La date est ce qui permet de
// rapprocher la mort du bureau de ce qui tournait a ce moment-la.
TEST(journal_writes_a_dated_line_for_the_event) {
  Bac bac;
  REQUIRE(!bac.dir().empty());
  HorlogeFigee horloge;

  sshos::Journal j(horloge, bac.fichier());
  j.note("demarrage pid=7");

  const std::string texte = lire(bac.fichier());
  REQUIRE_EQ(lignes(texte), size_t{1});
  // « AAAA-MM-JJ HH:MM:SS » puis une espace : 20 caracteres avant
  // l'evenement. On verifie la FORME, pas les chiffres -- l'heure est
  // locale, et le fuseau de la machine n'est pas au test de le decider.
  REQUIRE(texte.size() > 20);
  CHECK_EQ(texte.substr(20), std::string("demarrage pid=7\n"));
  CHECK_EQ(texte[4], '-');
  CHECK_EQ(texte[7], '-');
  CHECK_EQ(texte[10], ' ');
  CHECK_EQ(texte[13], ':');
  CHECK_EQ(texte[16], ':');
  CHECK_EQ(texte[19], ' ');
}

// IL AJOUTE, il ne remplace pas : la vie precedente du demon est justement
// ce qu'on vient lire apres un incident.
TEST(journal_adds_to_what_is_already_there) {
  Bac bac;
  REQUIRE(!bac.dir().empty());
  HorlogeFigee horloge;

  sshos::Journal j(horloge, bac.fichier());
  j.note("demarrage pid=7");
  j.note("arret sur SIGTERM");

  const std::string texte = lire(bac.fichier());
  CHECK_EQ(lignes(texte), size_t{2});
  CHECK(texte.find("demarrage pid=7") != std::string::npos);
  CHECK(texte.find("arret sur SIGTERM") != std::string::npos);
}

// BORNE HAUTE. Un demon qui redemarre en boucle ferait sinon grossir ce
// fichier sans fin, et personne ne surveille sa taille.
TEST(journal_starts_over_when_it_gets_too_big) {
  Bac bac;
  REQUIRE(!bac.dir().empty());
  HorlogeFigee horloge;

  sshos::Journal j(horloge, bac.fichier(), 100);
  for (int i = 0; i < 6; ++i) j.note("un evenement de longueur bien suffisante");

  const std::string texte = lire(bac.fichier());
  CHECK(texte.find("(journal remis a zero)") != std::string::npos);
  CHECK(texte.size() <= size_t{300});
}

// UN JOURNAL N'EMPORTE JAMAIS LE BUREAU. Disque plein, chemin impossible,
// droits absents : il se tait, il ne leve pas.
TEST(journal_stays_silent_on_an_unwritable_path) {
  HorlogeFigee horloge;
  sshos::Journal j(horloge, "/proc/nexiste-pas-du-tout/journal.log");
  j.note("demarrage pid=7");  // ne doit ni lever ni planter
  CHECK(lire("/proc/nexiste-pas-du-tout/journal.log").empty());
}

// SANS CHEMIN, PAS DE JOURNAL -- et le demon demarre quand meme. C'est le
// cas d'un environnement sans HOME.
TEST(journal_does_nothing_without_a_path) {
  HorlogeFigee horloge;
  sshos::Journal j(horloge, "");
  j.note("demarrage pid=7");
  CHECK(j.path().empty());
}

// UNE INSTALLATION NEUVE n'a jamais rien ecrit sous ~/.local/share : le
// repertoire n'existe pas encore, et un journal qui abandonnerait la ne
// servirait justement qu'aux machines qui ont deja servi.
TEST(journal_creates_the_data_directory_if_it_is_missing) {
  Bac bac;
  REQUIRE(!bac.dir().empty());
  HorlogeFigee horloge;

  // DEUX niveaux manquants, pas un : sur une machine neuve, ce n'est pas
  // seulement `~/.local/share/termos` qui n'existe pas encore, c'est
  // `~/.local/share` -- et un mkdir() du seul dernier segment echoue alors
  // sur ENOENT sans rien dire. Constate le 18 aout 2026 par une sonde, que
  // la version d'un seul niveau de ce cas laissait passer.
  sshos::Journal j(horloge, bac.dir() + "/share/termos/journal.log");
  j.note("demarrage pid=7");

  CHECK(lire(bac.dir() + "/share/termos/journal.log").find("demarrage pid=7") !=
        std::string::npos);
}

// LE CHEMIN EST CELUI DE L'UTILISATEUR, et un seul endroit le calcule.
TEST(the_journal_lives_beside_the_update_state) {
  const char* avant = std::getenv("XDG_DATA_HOME");
  const std::string sauve = avant != nullptr ? avant : "";
  ::setenv("XDG_DATA_HOME", "/tmp/sshos-essai-xdg", 1);

  CHECK_EQ(sshos::user_data_dir(), std::string("/tmp/sshos-essai-xdg/termos"));
  CHECK_EQ(sshos::daemon_journal_path(),
           std::string("/tmp/sshos-essai-xdg/termos/journal.log"));

  if (avant != nullptr) {
    ::setenv("XDG_DATA_HOME", sauve.c_str(), 1);
  } else {
    ::unsetenv("XDG_DATA_HOME");
  }
}
