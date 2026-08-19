#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sshos {

// Ce que le handshake transporte : les variables qui décrivent la session
// SSH du client, et elles seules. Même forme que `Hello::env`.
using EnvDelta = std::vector<std::pair<std::string, std::string>>;

// Les variables de session, et rien d'autre. La liste est FERMÉE : le
// handshake vient du réseau, et laisser un client y glisser PATH ou
// LD_PRELOAD reviendrait à lui donner l'exécution de code dans tous les
// processus du démon. C'est aussi la liste de ce qu'on efface quand le
// client n'en offre pas de version fraîche.
bool is_session_variable(std::string_view key);

// Le shell de l'utilisateur, lu dans getpwuid -- PAS dans $SHELL.
// L'environnement du démon est un fossile de la PREMIÈRE session SSH :
// suivre son $SHELL revient à obéir à ce que le tout premier client avait
// ce jour-là. Rend "/bin/sh" si la base de comptes ne dit rien.
std::string login_shell();

// Le dossier de l'utilisateur, lu dans getpwuid -- PAS dans $HOME, et pour
// exactement la meme raison que ci-dessus. C'est la ou s'ouvre un terminal
// quand l'utilisateur n'a rien regle : `become_daemon()` place le demon a
// « / » pour ne retenir aucun point de montage, et l'invite en heritait.
// Rend "/" si la base de comptes ne dit rien.
std::string home_dir();

// L'environnement d'un nouvel enfant : la base du démon, moins ses
// fossiles de session, plus le delta du client, plus ce que nous imposons.
//
// Fonction PURE, base injectée : sans ça il n'y a pas moyen de tester la
// règle du fossile sans salir l'environnement du processus de test.
std::vector<std::string> child_env(const std::vector<std::string>& base,
                                   const EnvDelta& delta);

// La base réelle : `environ`. Séparée de child_env pour que celle-ci reste
// pure.
std::vector<std::string> daemon_env();

}  // namespace sshos
