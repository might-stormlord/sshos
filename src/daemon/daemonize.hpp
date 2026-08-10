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
// argv est arbitraire, mais le petit-enfant ne démarre PAS avec les fds
// hérités de l'appelant dans le cas courant : 0/1/2 sont redirigés vers
// /dev/null juste avant l'execv, dès que /dev/null est ouvrable. Un
// appelant qui compterait faire hériter un fd précis à argv (tube de
// disponibilité, journal) via 0/1/2 se le verra donc, dans ce cas, retiré
// et remplacé -- et si l'appelant garde de son côté l'autre extrémité d'un
// tube dont la lecture vient d'être ainsi fermée, un write() ultérieur sur
// cette extrémité lève SIGPIPE.
//
// Repli si /dev/null est indisponible (chroot dégradé, par exemple) : voir
// redirect_std_to_devnull() dans daemonize.cpp. Les fds 0/1/2 restent alors
// simplement ceux hérités de l'appelant -- ni redirection ni fermeture --
// donc le risque de SIGPIPE ci-dessus ne se matérialise pas non plus dans
// ce cas précis (rien n'a été fermé côté petit-enfant). Ce repli est un
// sans-conséquence de dernier recours, pas une garantie : un appelant ne
// doit pas en déduire qu'il peut compter sur l'un ou l'autre comportement,
// seulement que /dev/null indisponible ne fait pas échouer le démon.
//
// Rend le pid de l'enfant intermédiaire ; l'appelant DOIT le récolter,
// sinon il reste zombie. Attention à waitpid(-1, ...) : ce n'est pas une
// erreur, c'est une attente sur N'IMPORTE QUEL enfant de l'appelant — un
// pid non vérifié (notamment le -1 que cette fonction elle-même peut
// rendre, voir plus bas) qui atteindrait waitpid ferait récolter en
// silence un processus sans rapport plutôt que de signaler l'échec.
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
