#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sshos {

// L'état de la mise à jour, tel que les scripts l'écrivent et que le démon
// le lit. Le C++ ne l'écrit JAMAIS : c'est ce qui garde `git`, `cmake` et le
// réseau hors du fil unique du démon.
enum class UpdateStatus {
  Idle,
  Checking,
  Applying,
  UpToDate,
  Available,
  RestartPending,
  CheckFailed,
  ApplyFailed,
  HistoryRewritten,
  UpdatesDisabled,
};

struct UpdateState {
  UpdateStatus status = UpdateStatus::Idle;
  std::string prefix;
  std::string source;
  std::string installed_commit;
  std::string previous_commit;
  std::string remote_commit;
  // DES VERSIONS, ET NON DES EMPREINTES. « cce9d11 -> 3512ffe » ne dit rien
  // a personne ; « 1.12 -> 1.13 » se lit. Le majeur est declare dans le
  // fichier VERSION du depot, le mineur est compte depuis ce jour-la (voir
  // tools/version.sh). Vide veut dire « inconnue » : on n'invente pas un
  // numero a afficher.
  //
  // Le C++ ne les CALCULE pas -- c'est du domaine de git -- il les lit.
  std::string installed_version;
  std::string remote_version;
  // Combien de commits separent les deux. Zero veut dire « on ne sait pas ».
  int commits_ahead = 0;
  std::int64_t checked_at = 0;
  // -1 quand aucun travail ne court. Renseigné seulement pour `checking` et
  // `applying`, et c'est ce qui permet de distinguer un travail en cours
  // d'un travail interrompu par la mort du démon.
  pid_t pid = -1;
  // Ou en est le travail : « compilation », « suite de tests »... Vide quand
  // rien ne court. Une fenetre qui dit « en cours » pendant deux minutes
  // sans rien preciser laisse croire a un blocage.
  std::string stage;
  // OU EN EST LE TRAVAIL, EN POUR CENT. -1 quand on ne sait pas -- et c'est
  // le cas normal, pas un cas de bord : une installation mise a jour par un
  // script plus ancien n'en depose aucune, et une barre a zero laisserait
  // croire qu'il ne se passe rien.
  //
  // Le C++ ne la CALCULE pas, comme il ne calcule pas les numeros de
  // version : cmake ecrit deja son propre pourcentage et la suite de tests
  // une ligne par cas, or ni l'un ni l'autre n'est a portee du demon. Le
  // script compte, le demon lit, et refuse tout ce qui n'est pas un
  // pourcentage.
  int progress = -1;
  std::string message;

  // CE QUI A CHANGE, et non seulement combien. « 5 nouveautes » ne dit pas
  // LESQUELLES : le script extrait les sujets de commit -- il a git sous la
  // main, le C++ ne l'aura jamais -- et les depose numerotes, `note_1` a
  // `note_N`, du plus recent au plus ancien.
  //
  // Bornees en nombre ET en longueur, et assainies : la valeur vient d'un
  // script, passe par un fichier editable a la main, et finit DESSINEE dans
  // une modale.
  std::vector<std::string> notes;
};

// Le fichier est lu dans le fil UNIQUE du démon. Un résumé de compilation de
// plusieurs mégaoctets y ferait une lecture, une allocation et une analyse
// synchrones dans la boucle d'affichage, à chaque relecture -- exactement ce
// que la contrainte « le démon ne bloque jamais » interdit. Le projet sait
// plafonner ailleurs (kMaxMessageBytes de proto.hpp) ; ici aussi. Le nom
// diffère du sien pour la même raison : les deux vivent dans le namespace
// sshos, et deux constantes homonymes ne compileraient pas.
inline constexpr std::size_t kMaxStateBytes = 4096;
inline constexpr std::size_t kMaxStateMessageBytes = 200;

// Le seul schéma connu. Une valeur différente est traitée comme un fichier
// absent : mieux vaut ne rien afficher que de deviner un format.
inline constexpr int kUpdateStateSchema = 1;

// Combien de notes de version on garde, et combien de caracteres chacune.
// Six tiennent dans une modale sans la faire deborder ; au-dela, le script
// ecrit lui-meme une derniere ligne « ... et N autres ». La longueur suit
// celle des sujets de commit du projet, qui tiennent en 72 colonnes.
inline constexpr std::size_t kMaxUpdateNotes = 6;
inline constexpr std::size_t kMaxUpdateNoteChars = 76;

// Analyse PURE : ni disque, ni horloge. `now_epoch` sert uniquement à borner
// `checked_at`, et il est passé plutôt que lu pour que le cas d'une horloge
// reculée soit reproductible en test.
//
// Tolérante, jamais devineresse :
//   - clé inconnue ignorée, ligne sans `=` ignorée ;
//   - découpe au PREMIER `=`, et la PREMIÈRE occurrence d'une clé gagne --
//     ainsi une ligne ajoutée après coup, par un message mal assaini qui
//     contiendrait un retour à la ligne, ne peut pas écraser une valeur déjà
//     lue ;
//   - valeur illisible ramenée au défaut du champ ;
//   - `checked_at` hors de [0, now_epoch] ramené à 0, ce qui traite d'un
//     seul geste l'horloge reculée et le débordement de checked_at + 86400 ;
//   - fichier absent, vide, de schéma inconnu ou plus grand que
//     kMaxStateBytes rendu comme un état vierge.
// Aucun cas ne lève.
UpdateState parse_update_state(std::string_view raw, std::int64_t now_epoch);

}  // namespace sshos
