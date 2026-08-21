#pragma once

namespace sshos {

// COMBIEN DE FOIS UN CLIENT REJOUE SON DÉMARRAGE APRÈS « REDEMARRER POUR
// TERMINER », ET CE QUE CE COMPTE BORNE VRAIMENT.
//
// POURQUOI CE FICHIER EXISTE. Le compte vivait dans `src/main.cpp`, sous la
// forme `for (int attempt = 0; attempt < 2; ++attempt)`. Il comptait les
// redémarrages de TOUTE LA VIE DU CLIENT, alors qu'il croyait borner une
// boucle. Conséquence exacte, mesurée le 21 août 2026 : le premier
// redémarrage d'une session passait, le second était refusé **sans même
// essayer de relancer un démon** -- « sshos: le redemarrage n'a pas abouti »,
// bureau perdu, retour au shell, alors que rien n'était cassé. Un
// utilisateur qui retapait `sshos` repartait avec un compteur neuf : d'où le
// « une fois sur deux » exact que le défaut présentait.
//
// CE QUI MÉRITE D'ÊTRE BORNÉ N'EST PAS LE NOMBRE DE REDÉMARRAGES. Un
// redémarrage pour mise à jour ne s'arme QUE sur une confirmation explicite
// -- `answer_modal(true)` avec `ModalKind::RestartForUpdate`, qui appelle
// `UpdateService::run("update:restart")` (src/shell/update_service.cpp).
// Aucun chemin ne l'arme tout seul : chaque tour coûte un geste de
// l'utilisateur, et rationner ces gestes n'a aucun sens. Ce qu'il faut
// empêcher, c'est l'aller-retour STÉRILE -- un démon qui se détache pour se
// mettre à jour sans avoir jamais servi de bureau --, parce que celui-là,
// enchaîné, ferait tourner le client pour rien.
//
// Ici plutôt que dans main.cpp pour la même raison que `launch_daemon`
// (src/client/launch.hpp) : CMakeLists.txt retire main.cpp de `sshos_core`,
// donc rien de ce qui y vit n'est atteignable par la suite de tests. C'est
// exactement là que le budget d'attente trop court s'était déjà caché en
// août. Sortir la décision n'est pas un rangement, c'est la moitié du
// correctif -- voir tests/test_restart.cpp.
class RestartBudget {
 public:
  // `max_sterile` borne les allers-retours stériles CONSÉCUTIFS. Deux :
  // c'est ce que l'ancienne boucle accordait au cas pathologique, et il n'y
  // a aucune raison de changer ce chiffre-là -- c'est l'autre cas, le cas
  // fructueux, qui n'aurait jamais dû être compté.
  explicit RestartBudget(int max_sterile = 2);

  // Le démon vient de nous détacher pour se mettre à jour. `fruitful` dit si
  // la session qui vient de finir avait SERVI : un bureau affiché et un
  // utilisateur qui a agi. Rend vrai s'il faut rejouer le démarrage, faux
  // s'il faut rendre la main au shell.
  //
  // Une session fructueuse remet le compte à zéro : sans cela, un unique
  // aller-retour stérile -- suivi de mois de bureau normal -- laisserait le
  // client avec un seul redémarrage en réserve pour toujours.
  bool allow(bool fruitful);

 private:
  int max_;
  int sterile_ = 0;
};

}  // namespace sshos
