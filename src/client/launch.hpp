#pragma once

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace sshos {

// Lancer un démon et attendre qu'il écoute.
//
// POURQUOI CE FICHIER EXISTE. Ce geste vivait dans `src/main.cpp`, que
// `CMakeLists.txt` retire de `sshos_core` : aucun test de la suite ne
// pouvait donc l'atteindre, et c'est précisément là qu'un budget d'attente
// trop court a fait perdre un redémarrage de bureau sans que rien nulle
// part ne puisse le dire. Le sortir de main.cpp n'est pas un rangement,
// c'est la moitié du correctif.

// Comment l'attente s'est terminée.
enum class DaemonLaunch {
  Connected,    // le démon écoute, on peut s'y attacher
  SpawnFailed,  // le fork a échoué : rien n'a été lancé
  TimedOut,     // le budget est épuisé sans que personne n'écoute
};

// Rend le pid de l'enfant intermédiaire à récolter, ou -1. Même contrat que
// spawn_detached (daemonize.hpp), dont c'est la valeur par défaut : la
// couture n'existe que pour que les tests n'aient pas à lancer un vrai
// démon pour éprouver l'attente.
using DaemonSpawner = std::function<pid_t(const std::vector<std::string>& argv)>;

// CE QU'ON ACCORDE À UN DÉMON POUR SE METTRE À ÉCOUTER.
//
// TRENTE SECONDES, ET LE CHIFFRE A UNE HISTOIRE. Il valait une seconde
// (50 tentatives à 20 ms) : au-delà, le client rendait la main au shell et
// le bureau restait à tourner sans personne dedans. Le cas mesuré tenait en
// treize secondes, sur une machine qui venait de compiler le projet entier
// et de passer 1277 tests — le moment précis où un redémarrage est demandé.
// Le geste ne coûte RIEN dans le cas nominal : la boucle sort dès que la
// connexion aboutit, en quelques dizaines de millisecondes ; le budget ne
// se paie que quand il sert. Et le harnais de test, lui, accordait déjà
// 2 s à la même attente (tests/test_session.cpp) : le produit était le
// moins patient des deux.
//
// `total` borne l'attente entière ; `interval` espace les tentatives de
// connexion ; passé `patience`, l'appelant est prévenu UNE FOIS que ça
// dure — une attente visible vaut mieux qu'un abandon muet, et c'est la
// seule chose qui distingue « le bureau met du temps » de « le bureau ne
// reviendra pas ».
struct LaunchBudget {
  std::chrono::milliseconds total{30000};
  std::chrono::milliseconds interval{20};
  std::chrono::milliseconds patience{1000};
};

// Lance `exe_path --daemon`, récolte l'intermédiaire, puis attend que
// `socket_name` accepte une connexion. `on_slow` est appelée au plus une
// fois, quand l'attente dépasse `budget.patience` ; elle peut être vide.
DaemonLaunch launch_daemon(const std::string& socket_name,
                           const std::string& exe_path,
                           const std::function<void()>& on_slow = {},
                           LaunchBudget budget = {},
                           DaemonSpawner spawn = {});

}  // namespace sshos
