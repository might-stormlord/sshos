#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace sshos {

// Double fork + setsid. L'intermédiaire appelle setsid() et devient chef
// de session ; le petit-enfant, forké ensuite, appartient à cette session
// SANS en être le chef — et c'est tout l'intérêt du second fork, car un
// processus qui n'est pas chef de session ne peut jamais acquérir de
// terminal de contrôle. Il est en outre réparenté à init.
//
// Aucun PR_SET_PDEATHSIG — il tuerait le démon à la mort du client, ce qui
// est exactement l'inverse de la fonctionnalité recherchée.
//
// Rend le pid de l'enfant intermédiaire ; l'appelant DOIT le récolter,
// sinon il reste zombie.
//
// Échec : rend -1, sans lever. Deux situations seulement produisent -1 --
// argv vide (rien à exécuter) et l'échec du premier fork() (EAGAIN/ENOMEM,
// pression de ressources). Aucun autre échec de la séquence (setsid,
// second fork, chdir, execv -- tous dans un processus déjà détaché de
// l'appelant) n'est observable par un code de retour ou une exception :
// il ne se manifeste que dans le code de sortie de l'intermédiaire (127),
// que l'appelant récolte de toute façon par waitpid. Faire lever une
// exception pour le seul échec du premier fork() diviserait la surface
// d'erreur de cette fonction en deux idiomes que l'appelant devrait gérer
// (try/catch ET inspection du code de sortie) là où waitpid seul suffit
// déjà pour tout le reste ; -1 reste donc l'unique signal, cohérent avec
// le code de sortie 127 des échecs situés plus bas dans la séquence, et
// avec la convention native de fork() dont cette fonction n'est qu'une
// fine enveloppe. Voir le message de commit pour la discussion complète de
// ce choix face à l'alternative (lever comme les voisins de net.hpp).
pid_t spawn_detached(const std::vector<std::string>& argv);

// À appeler en tête du mode --daemon, dans le processus déjà détaché.
void become_daemon();

}  // namespace sshos
