#pragma once

#include <string>

#include "input/events.hpp"

namespace sshos {

// `KeyEvent` → les octets que l'invité attend. Le MIROIR EXACT de
// `input/parser.cpp`, qui fait le chemin inverse : ce que le client nous
// envoie, nous le redonnons à l'invité dans la forme qu'un vrai terminal
// aurait produite.
//
// `cursor_keys_application` est le mode `DECCKM` (mode 1). Il change les
// flèches -- `\033[A` devient `\033OA` -- et un `vim` en mode insertion
// devient inutilisable si on l'ignore, parce que ses raccourcis lisent la
// forme applicative.
//
// Les modificateurs s'encodent en PARAMÈTRE, à la façon d'xterm :
// `\033[1;5C` pour `Ctrl+→`. La valeur du paramètre est 1 + le masque des
// modificateurs, exactement ce que `mods_from_param()` décode en face.
//
// UNE ASYMÉTRIE IRRÉDUCTIBLE, et il vaut mieux la nommer que la
// découvrir : `Ctrl+I` et `Tab` sont le MÊME octet, tout comme `Ctrl+M` et
// `Entrée`, `Ctrl+J` et le saut de ligne, `Ctrl+H` et l'effacement
// arrière. Aucun terminal ne les distingue, et l'aller-retour ne peut donc
// pas les préserver. Ce n'est pas un défaut de cet encodeur : c'est le
// clavier ASCII.
std::string encode_key(const KeyEvent& key, bool cursor_keys_application);

}  // namespace sshos
