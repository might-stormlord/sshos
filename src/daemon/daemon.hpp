#pragma once

#include <string>
#include <string_view>

#include "daemon/journal.hpp"

namespace sshos {

// `journal_path` n'existe que pour les tests, comme `self_exe` de
// UpdateService et `boot_id_path` de net.hpp : il a pour valeur par défaut
// le vrai chemin, et il permet d'éprouver ce que le démon écrit quand il
// n'arrive pas à démarrer, sans toucher au journal de la machine.
int run_daemon(std::string_view socket_name,
               const std::string& journal_path = daemon_journal_path());

}  // namespace sshos
