#pragma once

#include <string>

namespace sshos {

// LE REPERTOIRE DE DONNEES DE L'UTILISATEUR, `<...>/sshos`. Il vit sous son
// home quel que soit le prefixe d'installation choisi : l'etat des mises a
// jour et le journal du demon sont propres a la PERSONNE, pas a
// l'installation.
//
// Rend une chaine VIDE quand ni XDG_DATA_HOME ni HOME ne sont exploitables
// -- l'appelant decide alors quoi faire, plutot que d'ecrire dans un chemin
// invente. Un SEUL endroit calcule ce chemin : deux copies finiraient par
// diverger, et l'une des deux chercherait un fichier que l'autre ecrit
// ailleurs.
std::string user_data_dir();

}  // namespace sshos
