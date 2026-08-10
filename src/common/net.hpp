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

// Lit /proc/sys/kernel/random/boot_id (uuid regenere a chaque demarrage du
// noyau). Si indisponible, se rabat sur `proc_stat_path` (repli documente
// dans net.cpp : c'est le "btime" de /proc/stat). Si aucune des deux
// sources n'est exploitable, leve std::runtime_error plutot que d'inventer
// une constante -- voir net.cpp pour la justification.
//
// Les deux paramètres ont une valeur par défaut pointant vers les vrais
// chemins système ; ils n'existent que pour permettre aux tests de
// substituer des chemins de repli/absence sans dépendre de l'état réel de
// la machine qui exécute la suite.
std::string read_boot_id(std::string_view boot_id_path = "/proc/sys/kernel/random/boot_id",
                          std::string_view proc_stat_path = "/proc/stat");
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
//  - Empty    : rien n'attendait (EAGAIN/EWOULDBLOCK). Le cas normal d'une
//               boucle non bloquante ; ne mérite aucune ligne de journal.
//  - Accepted : un pair a été accepté et son uid correspond à
//               `expected_uid` ; `fd` est valide et `cred` renseigné.
//  - Rejected : un pair a été accepté puis refermé car son uid ne
//               correspondait pas ; `cred` est renseigné pour journaliser
//               qui a essayé (c'est exactement l'événement que cette
//               couche existe pour surveiller).
//  - Error    : accept4() ou getsockopt() a échoué pour une raison qui
//               n'est ni l'absence de connexion ni un refus d'uid (ex:
//               EMFILE) ; `err` porte l'errno d'origine.
//
// EINTR est absorbé en interne par une nouvelle tentative : ce n'est ni un
// backlog vide ni une erreur, seulement un accept4() à refaire, et le
// confondre avec "rien en attente" ferait sortir trop tôt une boucle de
// vidage du backlog.
enum class AcceptOutcome { Empty, Accepted, Rejected, Error };

struct AcceptResult {
  AcceptOutcome outcome = AcceptOutcome::Empty;
  Fd fd;                   // valide seulement si outcome == Accepted
  PeerCredentials cred{};  // renseigné si outcome == Accepted ou Rejected
  int err = 0;             // errno d'origine, renseigné seulement si outcome == Error
};

AcceptResult accept_peer(int listen_fd, uid_t expected_uid);

}  // namespace sshos
