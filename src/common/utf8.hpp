#pragma once

#include <cstddef>
#include <string>
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

// Encode un scalaire Unicode en UTF-8. Rejette les mêmes valeurs
// qu'utf8_decode() refuse en lecture — surrogates [D800,DFFF], > 10FFFF —
// en les remplaçant par U+FFFD : ce sont les deux moitiés du même codec,
// et elles doivent s'accorder sur ce qui est un scalaire Unicode valide.
//
// Vit ici plutôt que dans render/, son unique consommateur actuel
// (render/diff.cpp), pour la même raison qu'utf8_decode() : un encodeur
// UTF-8 n'est pas une préoccupation de rendu, c'est la moitié écriture
// d'un codec dont la moitié lecture est déjà partagée. Le laisser dans un
// en-tête consacré au profil de sortie (ColorDepth, OutputProfile) aurait
// forcé tout futur consommateur hors render/ (par ex. common/ ou input/,
// pour émettre du texte) à soit tirer toute l'API couleur pour obtenir un
// encodeur, soit en écrire une seconde copie — exactement le chemin qui a
// produit deux décodeurs avant l'extraction ci-dessus. Ne pas la
// rapatrier dans render/ au prétexte qu'elle n'a aujourd'hui qu'un
// consommateur là-bas : c'est le nombre de consommateurs FUTURS hors
// render/ que ce placement anticipe, pas le nombre actuel.
std::string encode_utf8(char32_t cp);

}  // namespace sshos
