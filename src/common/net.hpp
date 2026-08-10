#pragma once

#include <sys/types.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "common/fd.hpp"

namespace sshos {

struct AddressInUse : std::runtime_error {
  AddressInUse() : std::runtime_error("adresse deja utilisee") {}
};

// Nom de la variable d'environnement consultée en priorité par
// read_boot_id(), avant toute lecture de fichier. Exposée ici (plutôt que
// comme littéral dupliqué dans net.cpp et dans les tests) pour qu'il n'y ait
// qu'un seul endroit où ce nom est écrit.
//
// Son contrat est entièrement porté par qui l'exporte : le démon et chaque
// client recalculent le nom du socket indépendamment, donc si la variable
// est utilisée, l'opérateur qui la définit doit lui donner la même valeur
// pour le démon et pour tous les clients censés pouvoir s'y rattacher. Rien
// dans le code ne vérifie ni ne peut vérifier cette identité -- c'est
// exactement pourquoi ce n'est qu'une échappatoire explicite, pas une
// source ambiante lue automatiquement comme boot_id.
inline constexpr const char* kBootIdEnvVar = "SSHOS_BOOT_ID";

// Lit, dans l'ordre : la variable d'environnement kBootIdEnvVar (si elle est
// définie et non vide), puis /proc/sys/kernel/random/boot_id (uuid
// régénéré à chaque démarrage du noyau). Si aucune des deux n'est
// exploitable (conteneur assez restreint pour masquer /proc/sys, et aucune
// variable fournie), lève std::runtime_error plutôt que d'inventer une
// valeur qui a l'air stable mais ne l'est pas -- voir net.cpp pour la
// justification, notamment vis-à-vis d'un repli sur /proc/stat qui a été
// retiré : "btime" n'est pas une constante par boot, le noyau le recalcule
// à chaque lecture à partir de l'horloge murale courante, donc tout ce qui
// fait avancer cette horloge entre le démarrage du démon et l'attache d'un
// client (une resynchronisation NTP, une horloge matérielle absente sur un
// premier démarrage...) fait échouer le rattachement en silence.
//
// boot_id_path a une valeur par défaut pointant vers le vrai chemin
// système ; il n'existe que pour permettre aux tests de substituer un
// chemin absent sans dépendre de l'état réel de la machine qui exécute la
// suite.
std::string read_boot_id(std::string_view boot_id_path = "/proc/sys/kernel/random/boot_id");
std::string socket_name(uid_t uid, std::string_view boot_id);

// Adresse abstraite : sun_path[0] == '\0'. Rien sur le système de fichiers,
// donc rien à nettoyer et rien que logind puisse effacer.
Fd bind_abstract(std::string_view name);
Fd connect_abstract(std::string_view name);

// Identité du pair d'une connexion (SO_PEERCRED). Utile aussi bien pour un
// pair accepté (qui a la main) que refusé (qui a essayé) : dans les deux
// cas c'est ce dont un journal a besoin.
struct PeerCredentials {
  pid_t pid = -1;
  uid_t uid = static_cast<uid_t>(-1);
  gid_t gid = static_cast<gid_t>(-1);
};

// Issue d'un essai d'acceptation. Une adresse abstraite n'a pas de
// permissions : tout processus de la machine peut se placer dans le
// backlog, et le contrôle d'uid n'intervient qu'après l'accept(). Un
// appelant qui construit une boucle d'acceptation non bloquante par-dessus
// doit donc pouvoir distinguer, au minimum :
//  - Empty          : rien n'attendait (EAGAIN/EWOULDBLOCK). Le cas normal
//                      d'une boucle non bloquante ; ne mérite aucune ligne
//                      de journal.
//  - Accepted       : un pair a été accepté et son uid correspond à
//                      `expected_uid` ; `fd` est valide et `cred` renseigné.
//  - Rejected       : un pair a été accepté puis refermé car son uid ne
//                      correspondait pas ; `cred` est renseigné pour
//                      journaliser qui a essayé (c'est exactement
//                      l'événement que cette couche existe pour
//                      surveiller).
//  - TransientError : accept4() ou getsockopt() a échoué pour une raison
//                      qui a de bonnes chances de se résorber seule --
//                      pression sur les descripteurs ou la mémoire
//                      (EMFILE, ENFILE, ENOBUFS, ENOMEM). Vaut la peine
//                      d'être retenté, avec un recul (backoff) pour ne pas
//                      transformer l'attente en boucle chaude ; `err`
//                      porte l'errno d'origine.
//  - FatalError     : accept4() ou getsockopt() a échoué pour une raison
//                      que retenter ne corrigera jamais -- l'écouteur
//                      lui-même est mal formé ou fermé (ENOTSOCK, EBADF,
//                      EINVAL) ; toute valeur d'errno non explicitement
//                      reconnue comme transitoire atterrit ici par défaut,
//                      voir net.cpp:is_transient_errno. `err` porte
//                      l'errno d'origine.
//
// Distinguer ces deux dernières valeurs dans le type, plutôt que de laisser
// l'appelant relire `err` pour savoir laquelle retenter, est volontaire :
// une seule valeur Error pour les deux invite à écrire "boucle jusqu'à
// Empty, journalise et continue sinon" -- l'idiome édicté ci-dessus pour
// Empty -- qui tourne alors à froid sur un écouteur cassé (mesuré : ~144k
// appels/s en continu sur ENOTSOCK) et affame un accept qui aurait pu
// réussir sous pression passagère (mesuré : ~48k appels/s, zéro
// acceptation, sous EMFILE avec un client réellement en attente). Avec
// `enum class` et -Wswitch -Werror, un futur appelant qui fait un switch
// sur AcceptOutcome sans les deux valeurs ne compile pas.
//
// EINTR est absorbé en interne par une nouvelle tentative : ce n'est ni un
// backlog vide ni une erreur, seulement un accept4() à refaire, et le
// confondre avec "rien en attente" ferait sortir trop tôt une boucle de
// vidage du backlog.
//
// ECONNABORTED (le pair a rompu la connexion après être entré dans la file
// d'écoute mais avant d'être accepté) est absorbé de la même façon :
// événement bénin et attendu côté accept(2), pas un signe que l'écouteur
// est cassé -- le traiter comme Empty ferait sortir une boucle de vidage
// alors que d'autres connexions peuvent être derrière dans la file. Note
// pour qui relit ce choix : pour un socket UNIX abstrait, connect() aboutit
// de façon synchrone (pas de poignée de main en trois temps comme TCP), donc
// il n'existe pas de fenêtre "en file, pas encore accepté" que le pair
// pourrait rompre après coup -- ce cas est vraisemblablement inatteignable
// ici en pratique sur Linux. Géré tout de même, pour rester correct si cette
// fonction est un jour réutilisée sur un autre type de socket, et parce que
// accept(2) le documente comme une erreur possible indépendamment du
// protocole.
enum class AcceptOutcome { Empty, Accepted, Rejected, TransientError, FatalError };

struct AcceptResult {
  AcceptOutcome outcome = AcceptOutcome::Empty;
  Fd fd;                   // valide seulement si outcome == Accepted
  PeerCredentials cred{};  // renseigné si outcome == Accepted ou Rejected
  int err = 0;  // errno d'origine, renseigné seulement si outcome == TransientError ou FatalError
};

AcceptResult accept_peer(int listen_fd, uid_t expected_uid);

// Pid du pair. C'est ainsi que `--kill` trouve le démon : pas de fichier
// de pid à maintenir, l'information est déjà dans le socket.
pid_t peer_pid(int fd);

}  // namespace sshos
