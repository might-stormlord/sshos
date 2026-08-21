#pragma once

#include <string_view>

#include "common/fd.hpp"

namespace sshos {

// Comme connect_abstract() (tâche 8), mais borné : une connexion contre un
// démon dont la file d'attente (`listen backlog`, 16, voir net.cpp) est
// pleine ne revient sinon jamais -- vérifié : un connect() bloquant, sur un
// socket abstrait AF_UNIX, contre un backlog plein, ne revient pas avant que
// le serveur libère de la place via accept() ; un 18e appelant concurrent ne
// revient jamais. Exposée séparément de run_client() pour être testable
// isolément (remplir un backlog puis vérifier que ceci échoue au bout du
// délai plutôt que de bloquer le test lui-même).
Fd connect_with_timeout(std::string_view socket_name, int timeout_ms);

// Se connecte, fait le handshake, relaie jusqu'à Detached ou fermeture.
// Rend le code de retour du processus.
// Ce que run_client() rend quand le démon s'est arrêté POUR SE METTRE À
// JOUR. L'appelant doit alors rejouer son chemin de démarrage -- relancer le
// démon et se rattacher -- au lieu de rendre la main au shell.
//
// Une valeur distincte plutôt qu'un booléen de sortie : main() rend déjà ce
// que run_client() lui donne, et une valeur suffit à ne rien changer au
// reste du contrat.
inline constexpr int kClientRestartRequested = 64;

// CE QU'UNE SESSION A PRODUIT, pour que l'appelant sache si le redémarrage
// qu'on lui demande fait avancer quelque chose.
//
// La distinction n'est pas cosmétique : `RestartBudget`
// (src/client/restart.hpp) borne les allers-retours STÉRILES et ne compte pas
// les autres. Sans ces deux témoins, il n'y a aucun moyen de distinguer « le
// bureau est revenu, l'utilisateur a travaillé, puis il a recliqué » de « le
// démon se détache aussitôt attaché », et c'est en les confondant qu'un
// redémarrage sur deux se perdait.
struct SessionTrace {
  // Au moins une trame reçue du démon et écrite sur la sortie : le bureau
  // s'est affiché.
  bool desktop_shown = false;
  // Au moins une entrée transmise au démon : l'utilisateur a agi. C'est ce
  // qui sépare un redémarrage demandé d'un redémarrage subi -- rien dans le
  // démon n'arme `wants_restart` sans une confirmation explicite.
  bool user_acted = false;
};

// `trace`, quand elle n'est pas nulle, est remplie au fil de la session. Un
// paramètre optionnel plutôt qu'un changement de signature : le contrat
// existant -- « rend le code de retour du processus » -- ne bouge pas, et les
// appelants qui n'en ont que faire (tests/test_session.cpp) ne changent pas.
int run_client(std::string_view socket_name, SessionTrace* trace = nullptr);

}  // namespace sshos
