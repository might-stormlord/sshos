#pragma once

#include <string>
#include <string_view>

namespace sshos {

// LES REGLAGES DU BUREAU, ceux que l'utilisateur choisit et qui doivent
// survivre au redemarrage du demon.
//
// A ne pas confondre avec `shell/update_state.hpp` : celui-la est un ETAT
// que des scripts ecrivent et que le C++ ne fait que lire. Ici c'est
// l'inverse -- l'utilisateur regle depuis le bureau, le demon ecrit, et le
// fichier reste lisible et modifiable a la main.
//
// Format : `cle = valeur`, une par ligne, `#` en commentaire. Ce qu'on ne
// comprend pas est ignore LIGNE PAR LIGNE : un fichier ecrit par une
// version future ne doit pas faire perdre les cles d'aujourd'hui.
struct DesktopConfig {
  // Ou s'ouvre un nouveau terminal. Vide veut dire « le defaut », c'est a
  // dire le dossier de l'utilisateur -- ce n'est PAS au fichier de le
  // repeter, sans quoi un changement de compte laisserait un chemin mort.
  std::string start_dir;
};

DesktopConfig parse_config(std::string_view texte);
std::string render_config(const DesktopConfig& c);

// `<repertoire de donnees>/config`, ou vide si on ne sait pas ou.
std::string desktop_config_path();

// Lit le fichier. Absent, illisible ou vide rend simplement les defauts :
// une installation neuve n'a rien a expliquer.
DesktopConfig load_config(const std::string& path);

// Ecrit le fichier, par temporaire puis rename() -- qui est atomique. Une
// coupure au milieu laisse donc l'ANCIEN reglage en place, jamais un
// fichier tronque qu'on relirait comme « aucun reglage ». Rend false si
// rien n'a pu etre ecrit, pour que l'appelant ne fasse pas croire que le
// reglage est pose.
bool save_config(const std::string& path, const DesktopConfig& c);

// LE REGLAGE VIVANT : le fichier charge une fois, et reecrit des qu'on
// change quelque chose. Un seul exemplaire par demon, tenu par la session ;
// les applications y accedent par leur Host, jamais directement.
//
// `home` est INJECTE plutot que lu : c'est ce qui rend les regles de
// developpement du `~` testables sans dependre du compte qui fait tourner
// les tests.
class Settings {
 public:
  // Un `path` vide desactive la persistance sans rien casser -- le cas d'un
  // environnement sans HOME, et celui des tests.
  Settings(std::string path, std::string home);

  // OU S'OUVRE UN NOUVEAU TERMINAL, pour de vrai : le reglage avec son `~`
  // developpe, ou le dossier de l'utilisateur quand rien n'est regle.
  std::string start_dir() const;

  // CE QUE L'UTILISATEUR A TAPE, tel quel. C'est ce qu'on lui remontre quand
  // il rouvre la saisie : lui rendre `/home/moi/dev` quand il avait ecrit
  // `~/dev` donnerait l'impression que le bureau a change son choix.
  const std::string& configured() const { return config_.start_dir; }

  // Rend false si le fichier n'a pas pu etre ecrit. Le reglage vaut malgre
  // tout pour la session en cours : perdre le fichier ne doit pas aussi
  // faire perdre ce que l'utilisateur vient de demander.
  bool set_start_dir(std::string dir);

 private:
  std::string path_;
  std::string home_;
  DesktopConfig config_;
};

}  // namespace sshos
