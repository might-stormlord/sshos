#pragma once

#include <cstdint>

#include "vt/sink.hpp"

namespace sshos {

// Le niveau de rapport souris que l'invité a demandé. Les trois modes DEC
// sont des drapeaux indépendants -- une application peut poser 1002 puis
// 1003 -- mais ce que le liant doit savoir tient en un seul niveau : le
// PLUS PERMISSIF de ceux qui sont actifs, puisque c'est lui qui décide
// quels événements méritent d'être encodés.
enum class MouseTracking : uint8_t {
  None,
  Click,  // 1000 : enfoncement et relâchement
  Drag,   // 1002 : et le mouvement bouton enfoncé
  Any,    // 1003 : et le mouvement à vide
};

// Ce que l'invité a demandé par `DECSET`/`DECRST`. Un ENREGISTREMENT, pas
// un moteur : rien ici ne dessine ni ne bouge. Deux modes ont un effet
// mécanique sur la grille -- 7 (retour automatique) et 1049 (écran
// alterné) -- et cet effet vit dans `Screen`, que le liant actionne après
// avoir noté la demande. La séparation est délibérée : la tâche 9 devra
// RÉPONDRE à un `DECRQM` sur n'importe lequel de ces modes, y compris ceux
// qui ne changent rien à l'écran, et cette réponse se lit ici.
//
// Les défauts sont ceux d'un terminal qui vient de s'allumer : le retour
// automatique et le curseur sont ACTIFS, tout le reste est éteint.
struct Modes {
  bool cursor_keys_application = false;  // 1    DECCKM
  bool autowrap = true;                  // 7    DECAWM
  bool cursor_visible = true;            // 25   DECTCEM
  bool mouse_click = false;              // 1000
  bool mouse_drag = false;               // 1002
  bool mouse_any = false;                // 1003
  bool mouse_sgr = false;                // 1006
  bool alt_screen = false;               // 1049
  bool bracketed_paste = false;          // 2004

  MouseTracking tracking() const;

  // Un mode inconnu est ignoré en silence, comme le fait tout terminal :
  // une application qui demande un mode que nous n'avons pas doit pouvoir
  // continuer, pas se voir refuser sa séquence.
  void set(int mode, bool on);

  // Ce que `DECRQM` (tâche 9) a besoin de savoir : est-ce un mode dont
  // nous avons entendu parler, et dans quel état est-il ? La norme
  // distingue « non reconnu » de « éteint », et un invité qui verrait
  // « éteint » pour un mode inconnu croirait pouvoir l'allumer.
  bool knows(int mode) const;
  bool get(int mode) const;

  bool operator==(const Modes&) const = default;
};

// `CSI ? Pm h` (poser) et `CSI ? Pm l` (retirer). Une seule séquence peut
// porter PLUSIEURS modes -- `\033[?1000;1002;1006h` est la façon normale
// d'allumer la souris -- et chacun est traité pour lui-même.
void apply_dec_private(const Params& params, bool on, Modes& modes);

}  // namespace sshos
