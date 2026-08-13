#pragma once

#include <cstdint>

namespace sshos {

// LE JEU DE CARACTÈRES DEC.
//
// `ESC ( 0` bascule le jeu G0 sur les semi-graphiques DEC : les lettres
// `j` à `x` cessent d'être des lettres et deviennent les traits d'un
// cadre. `ESC ( B` revient à l'ASCII.
//
// Ce n'est pas de l'archéologie : `mc`, `dialog` et les `ncurses` un peu
// anciens dessinent TOUS leurs cadres comme ça. Sans la table, un
// gestionnaire de fichiers en mode texte s'affiche en `qqqqqqqq` au lieu
// d'une ligne horizontale.
//
// Seul G0 est modélisé. G1/G2/G3 et les bascules `SO`/`SI` existent dans
// la norme, mais rien de ce que nous visons ne s'en sert -- et un jeu
// qu'on prétendrait gérer sans le tester serait pire que son absence.
enum class Charset : uint8_t {
  Ascii,     // `ESC ( B`
  Graphics,  // `ESC ( 0`
};

// L'octet final d'un `ESC (` → le jeu qu'il désigne. Un final inconnu rend
// `Ascii` : c'est le jeu qui ne surprend personne.
Charset charset_from_final(uint8_t final_byte);

// Traduit un point de code SI le jeu courant l'exige. Hors de la plage
// `0x5F`-`0x7E`, et en ASCII, le caractère passe tel quel.
char32_t translate(char32_t cp, Charset set);

}  // namespace sshos
