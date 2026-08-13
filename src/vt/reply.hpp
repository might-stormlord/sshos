#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "vt/modes.hpp"
#include "vt/sink.hpp"

namespace sshos {

// LES RÉPONSES AUX REQUÊTES DE L'INVITÉ.
//
// Un programme qui demande la position du curseur ou l'identité du
// terminal BLOQUE jusqu'à obtenir sa réponse. C'est le démon qui répond,
// en écrivant lui-même sur le maître du PTY. Relayer la question au vrai
// terminal du client serait faux à trois titres : la réponse décrirait le
// terminal du client et non le nôtre, elle arriverait de façon asynchrone,
// et elle s'intercalerait au milieu des frappes de l'utilisateur.
//
// Tout est rendu sous forme de chaîne, et une chaîne VIDE veut dire « rien
// à répondre ». C'est ce qui permet au liant de traiter toutes les
// séquences par le même chemin sans avoir à savoir lesquelles sont des
// questions.

// `CSI c` : notre identité. VT220 (62) avec la couleur ANSI (22). Elle est
// STABLE : un invité qui la relit doit lire la même chose, et elle décrit
// ce que nous savons faire, pas ce que le client sait faire.
std::string device_attributes();

// `CSI > c` : la seconde identité, celle que les programmes lisent pour
// deviner « quel émulateur, quelle version ». Elle n'est pas dans
// l'énoncé de la tâche, mais une question sans réponse bloque son
// auteur -- ne pas répondre coûterait plus cher que répondre.
std::string secondary_device_attributes();

// `CSI 6 n` : la position, en coordonnées 1-INDEXÉES du fil, depuis des
// coordonnées 0-indexées de la grille.
std::string cursor_position_report(int x, int y);

// `CSI ? 6 n` : la position ÉTENDUE, qui porte en plus le numéro de page.
// C'est une autre question que `CSI 6 n`, et sa réponse a un autre format.
// Nous n'avons qu'une page, et c'est la page 1.
std::string extended_cursor_position_report(int x, int y);

// `CSI 5 n` : « aucun défaut ».
std::string device_status_ok();

// `CSI ? Pd $ p` : l'état d'un mode privé DEC.
std::string dec_mode_report(int mode, const Modes& modes);

// Aiguillage : la réponse à une séquence CSI quelconque, ou une chaîne
// vide si ce n'en était pas une question.
std::string reply_for_csi(const Params& params, std::string_view intermediates,
                          uint8_t final_byte, int cursor_x, int cursor_y,
                          const Modes& modes);

}  // namespace sshos
