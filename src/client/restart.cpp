#include "client/restart.hpp"

namespace sshos {

RestartBudget::RestartBudget(int max_sterile) : max_(max_sterile) {}

bool RestartBudget::allow(bool fruitful) {
  // UNE SESSION QUI A SERVI N'EST JAMAIS RATIONNÉE. C'est tout le correctif :
  // le redémarrage suivant repart d'un compte neuf, parce qu'il est demandé
  // par un utilisateur qui a travaillé entre les deux, pas par une boucle.
  if (fruitful) {
    sterile_ = 0;
    return true;
  }
  // Un plafond nul ou négatif refuse ici même : `++sterile_` vaut au moins 1,
  // donc la comparaison est fausse d'emblée. Lecture sûre d'un réglage
  // absurde -- refuser tout de suite plutôt que de tourner.
  ++sterile_;
  return sterile_ < max_;
}

}  // namespace sshos
