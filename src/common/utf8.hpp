#pragma once

#include <cstddef>
#include <string_view>

namespace sshos {

// Décode un scalaire Unicode à partir de s[pos]. Rend le nombre d'octets
// consommés, `out` vaut U+FFFD sur séquence invalide ou tronquée, et la
// consommation avance toujours d'au moins 1 : un décodeur qui n'avance pas
// boucle indéfiniment.
//
// Partagé entre render/ (texte déjà en mémoire, un flux complet à chaque
// appel) et input/ (octets reçus au fil de l'eau depuis le terminal) : les
// deux traitent de l'UTF-8 non fiable, et cette validation est la seule
// défense contre les surrogates encodés [D800,DFFF], les valeurs > 10FFFF,
// les suites sur-longues (CVE-2000-0884 : C0 AF ne doit jamais redevenir
// '/'), et les octets de continuation isolés. Avant l'extraction, input/
// avait sa propre copie non durcie de ce décodeur ; il n'y en a plus qu'une
// — ne pas la dupliquer une seconde fois.
size_t utf8_decode(std::string_view s, size_t pos, char32_t& out);

}  // namespace sshos
