#include <unistd.h>

#include <cstdlib>
#include <string>

#include "daemon/config.hpp"
#include "harness.hpp"

// LE FICHIER DE REGLAGES DU BUREAU. Une seule cle aujourd'hui, et un format
// qui doit rester lisible et modifiable a la main : c'est un fichier de
// configuration, pas un etat interne.

TEST(config_reads_the_start_directory) {
  const sshos::DesktopConfig c = sshos::parse_config("start_dir = /home/moi/dev\n");
  CHECK_EQ(c.start_dir, std::string("/home/moi/dev"));
}

// LES ESPACES NE COMPTENT PAS. Quelqu'un qui edite ce fichier a la main les
// mettra ou il voudra, et un reglage perdu pour une espace serait une
// enigme.
TEST(config_does_not_care_about_spacing) {
  CHECK_EQ(sshos::parse_config("start_dir=/a").start_dir, std::string("/a"));
  CHECK_EQ(sshos::parse_config("  start_dir   =   /b  \n").start_dir,
           std::string("/b"));
}

// CE QU'ON NE COMPREND PAS EST IGNORE, ligne par ligne : un fichier ecrit
// par une version FUTURE ne doit pas faire perdre les cles d'aujourd'hui.
TEST(config_ignores_what_it_does_not_know) {
  const sshos::DesktopConfig c = sshos::parse_config(
      "# un commentaire\n"
      "une ligne sans egal\n"
      "cle_de_demain = 3\n"
      "start_dir = /garde\n"
      " = valeur sans cle\n");
  CHECK_EQ(c.start_dir, std::string("/garde"));
}

// UN FICHIER ABSENT OU VIDE N'EST PAS UNE ERREUR : c'est le cas normal
// d'une installation neuve, et le defaut s'applique alors.
TEST(config_of_an_empty_file_is_empty) {
  CHECK(sshos::parse_config("").start_dir.empty());
  CHECK(sshos::parse_config("\n\n#rien\n").start_dir.empty());
}

// LA DERNIERE LIGNE GAGNE. Un fichier reecrit a la main peut porter deux
// fois la meme cle ; en garder la premiere donnerait l'impression que
// l'edition n'a servi a rien.
TEST(config_keeps_the_last_value_of_a_repeated_key) {
  CHECK_EQ(sshos::parse_config("start_dir = /un\nstart_dir = /deux\n").start_dir,
           std::string("/deux"));
}

// ALLER-RETOUR. Ce qu'on ecrit doit se relire a l'identique, sinon le
// reglage se perd au redemarrage suivant -- silencieusement.
TEST(config_survives_a_round_trip) {
  sshos::DesktopConfig c;
  c.start_dir = "/home/moi/mes projets";
  CHECK_EQ(sshos::parse_config(sshos::render_config(c)).start_dir, c.start_dir);
}

// UN REGLAGE VIDE N'ECRIT PAS DE LIGNE VIDE : relu, « start_dir = » rendrait
// une chaine vide, ce qui est deja le defaut -- autant ne rien poser.
TEST(config_writes_nothing_for_an_unset_value) {
  CHECK(sshos::render_config(sshos::DesktopConfig{}).find("start_dir") ==
        std::string::npos);
}

namespace {

// Un repertoire a soi, efface a la fin du cas.
class Bac {
 public:
  Bac() {
    char tpl[] = "/tmp/sshos-config-XXXXXX";
    const char* fait = ::mkdtemp(tpl);
    if (fait != nullptr) dir_ = fait;
  }
  ~Bac() {
    if (dir_.empty()) return;
    ::unlink((dir_ + "/config").c_str());
    ::rmdir(dir_.c_str());
  }
  Bac(const Bac&) = delete;
  Bac& operator=(const Bac&) = delete;
  std::string fichier() const { return dir_ + "/config"; }
  bool ok() const { return !dir_.empty(); }

 private:
  std::string dir_;
};

}  // namespace

// SUR DISQUE AUSSI, l'aller-retour doit etre fidele : c'est la seule chose
// qui fait qu'un reglage survit au redemarrage.
TEST(config_survives_a_round_trip_through_a_real_file) {
  Bac bac;
  REQUIRE(bac.ok());

  sshos::DesktopConfig c;
  c.start_dir = "/home/moi/dev";
  REQUIRE(sshos::save_config(bac.fichier(), c));

  CHECK_EQ(sshos::load_config(bac.fichier()).start_dir, std::string("/home/moi/dev"));
}

// UN FICHIER ABSENT DONNE LE DEFAUT, sans bruit : c'est l'installation
// neuve, pas une panne.
TEST(config_of_a_missing_file_is_the_default) {
  CHECK(sshos::load_config("/tmp/sshos-nexiste-pas-du-tout/config").start_dir.empty());
}

// ECRIRE LA OU L'ON NE PEUT PAS le dit, plutot que de faire croire que le
// reglage est pose.
TEST(config_says_when_it_cannot_write) {
  CHECK(!sshos::save_config("/proc/nexiste-pas/config", sshos::DesktopConfig{}));
}

// PAS DE FICHIER A MOITIE ECRIT. On passe par un temporaire puis rename(),
// qui est atomique : une coupure laisse l'ANCIEN reglage, jamais un fichier
// tronque qu'on relirait comme « aucun reglage ».
TEST(config_never_leaves_a_half_written_file) {
  Bac bac;
  REQUIRE(bac.ok());

  sshos::DesktopConfig c;
  c.start_dir = "/premier";
  REQUIRE(sshos::save_config(bac.fichier(), c));
  c.start_dir = "/second";
  REQUIRE(sshos::save_config(bac.fichier(), c));

  CHECK_EQ(sshos::load_config(bac.fichier()).start_dir, std::string("/second"));
  // Et le temporaire ne traine pas a cote.
  CHECK(::access((bac.fichier() + ".tmp").c_str(), F_OK) != 0);
}

// SANS RIEN DE REGLE, c'est le dossier de l'utilisateur -- pas « / », qui
// etait justement le defaut subi.
TEST(settings_fall_back_to_the_home_directory) {
  const sshos::Settings s("", "/home/moi");
  CHECK_EQ(s.start_dir(), std::string("/home/moi"));
  CHECK(s.configured().empty());
}

// LE TILDE EST DEVELOPPE. Sans ca, « ~/dev » tape dans la barre echouerait
// bêtement au chdir -- et l'utilisateur n'aurait aucune facon de le savoir,
// puisqu'un chdir rate est silencieux par choix.
TEST(settings_expand_a_leading_tilde) {
  sshos::Settings s("", "/home/moi");
  s.set_start_dir("~/dev/projet");
  CHECK_EQ(s.start_dir(), std::string("/home/moi/dev/projet"));

  s.set_start_dir("~");
  CHECK_EQ(s.start_dir(), std::string("/home/moi"));
}

// MAIS SEULEMENT EN TETE, et seulement le sien. « ~autre » est le home d'un
// AUTRE compte -- que le shell sait resoudre et pas nous -- et un tilde au
// milieu est un nom de fichier parfaitement legitime.
TEST(settings_only_expand_their_own_leading_tilde) {
  sshos::Settings s("", "/home/moi");
  s.set_start_dir("~autre/dev");
  CHECK_EQ(s.start_dir(), std::string("~autre/dev"));

  s.set_start_dir("/a/~/b");
  CHECK_EQ(s.start_dir(), std::string("/a/~/b"));
}

// ON LUI REMONTRE CE QU'IL A TAPE. Rouvrir la saisie sur « /home/moi/dev »
// alors qu'il avait ecrit « ~/dev » donnerait l'impression que le bureau a
// change son choix dans son dos.
TEST(settings_show_back_what_was_typed) {
  sshos::Settings s("", "/home/moi");
  s.set_start_dir("~/dev");
  CHECK_EQ(s.configured(), std::string("~/dev"));
}

// ET CA SURVIT AU REDEMARRAGE : c'est tout l'interet du fichier.
TEST(settings_survive_a_new_daemon) {
  Bac bac;
  REQUIRE(bac.ok());
  {
    sshos::Settings s(bac.fichier(), "/home/moi");
    REQUIRE(s.set_start_dir("~/dev"));
  }
  const sshos::Settings relu(bac.fichier(), "/home/moi");
  CHECK_EQ(relu.configured(), std::string("~/dev"));
  CHECK_EQ(relu.start_dir(), std::string("/home/moi/dev"));
}

// UN FICHIER QU'ON NE PEUT PAS ECRIRE NE FAIT PAS PERDRE LE REGLAGE de la
// session en cours : l'utilisateur a demande quelque chose, il doit
// l'obtenir tout de suite meme si demain l'oubliera.
TEST(settings_keep_the_value_even_when_the_file_cannot_be_written) {
  sshos::Settings s("/proc/nexiste-pas/config", "/home/moi");
  CHECK(!s.set_start_dir("~/dev"));
  CHECK_EQ(s.start_dir(), std::string("/home/moi/dev"));
}

