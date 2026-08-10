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
int run_client(std::string_view socket_name);

}  // namespace sshos
