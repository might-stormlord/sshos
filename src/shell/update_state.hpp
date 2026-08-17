#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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
  std::int64_t checked_at = 0;
  // -1 quand aucun travail ne court. Renseigné seulement pour `checking` et
  // `applying`, et c'est ce qui permet de distinguer un travail en cours
  // d'un travail interrompu par la mort du démon.
  pid_t pid = -1;
  std::string message;
};

// Le fichier est lu dans le fil UNIQUE du démon. Un résumé de compilation de
// plusieurs mégaoctets y ferait une lecture, une allocation et une analyse
// synchrones dans la boucle d'affichage, à chaque relecture -- exactement ce
// que la contrainte « le démon ne bloque jamais » interdit. Le projet sait
// plafonner ailleurs (kMaxMessageBytes de proto.hpp) ; ici aussi.
inline constexpr std::size_t kMaxStateBytes = 4096;
inline constexpr std::size_t kMaxMessageBytes = 200;

// Le seul schéma connu. Une valeur différente est traitée comme un fichier
// absent : mieux vaut ne rien afficher que de deviner un format.
inline constexpr int kUpdateStateSchema = 1;

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
