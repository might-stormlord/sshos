#pragma once

#include <string>

#include "render/cell.hpp"

namespace sshos {

// UNE BARRE DE PROGRESSION, ET UNE SEULE DANS LE PROJET.
//
// Elle vivait dans l'anonymat de `shell/sysinfo.cpp`, pour les jauges du
// fond d'écran. La fenêtre de mise à jour en voulait une aussi ; la
// recopier aurait refait le défaut n° 9 du dossier de reprise --
// `Files::display_label()`, devenue une duplication silencieuse d'une règle
// qui vivait déjà ailleurs, et qui a divergé.
//
// Le glyphe suit la BORDURE : c'est elle qui porte, dans tout le projet, la
// réponse à « ce client accepte-t-il l'UTF-8 ? ». Personne d'autre ne le
// sait ici.
//
// `percent` est ramené dans [0, 100] : cette fonction DESSINE, elle ne juge
// pas ce qu'on lui donne. Ceux qui lisent une valeur venue d'un script la
// refusent en amont (update_state.cpp), là où refuser veut encore dire
// quelque chose.
std::string gauge_bar(int percent, int width, Border b);

}  // namespace sshos
