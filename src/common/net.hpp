#pragma once

#include <sys/types.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "common/fd.hpp"

namespace sshos {

struct AddressInUse : std::runtime_error {
  AddressInUse() : std::runtime_error("adresse deja utilisee") {}
};

std::string read_boot_id();
std::string socket_name(uid_t uid, std::string_view boot_id);

// Adresse abstraite : sun_path[0] == '\0'. Rien sur le système de fichiers,
// donc rien à nettoyer et rien que logind puisse effacer.
Fd bind_abstract(std::string_view name);
Fd connect_abstract(std::string_view name);

// Une adresse abstraite n'a pas de permissions : tout processus de la
// machine peut s'y connecter. SO_PEERCRED est la seule barrière.
Fd accept_peer(int listen_fd, uid_t expected_uid);

}  // namespace sshos
