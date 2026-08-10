#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace sshos {

// M1 : None veut dire « rien à rapporter ici » (molette, focus — court-
// circuités avant d'atteindre le répartiteur de touches). Unknown veut dire
// « une touche est arrivée, ce parseur ne sait pas laquelle » (octet final
// CSI ou code tilde absent des tables key_from_*). Les deux se confondaient
// avant ce correctif ; Key et input/events.hpp ne sont référencés que par
// src/input/* (aucune trace dans common/proto.*), donc insérer Unknown ici
// ne déplace la valeur numérique d'aucun type qui traverse le réseau.
enum class Key : uint16_t {
  None, Unknown, Char, Enter, Tab, BackTab, Backspace, Escape,
  Up, Down, Left, Right, Home, End, PgUp, PgDn, Insert, Delete,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

namespace mod {
inline constexpr uint8_t Shift = 1;
inline constexpr uint8_t Alt = 2;
inline constexpr uint8_t Ctrl = 4;
}  // namespace mod

struct KeyEvent {
  Key key = Key::None;
  char32_t ch = 0;   // valable seulement si key == Key::Char
  uint8_t mods = 0;
};

enum class MouseAction { Press, Release, Motion, WheelUp, WheelDown };

struct MouseEvent {
  MouseAction action = MouseAction::Press;
  uint8_t button = 0;
  int x = 0;  // 0-indexé
  int y = 0;
  uint8_t mods = 0;
};

// C2 : un collage au-delà du plafond interne est livré en plusieurs
// PasteEvent successifs plutôt que d'être tronqué ou de faire grossir la
// mémoire retenue sans limite (cf. src/input/parser.cpp). `complete` vaut
// false sur tous les fragments sauf le dernier, qui referme la séquence ;
// un appelant qui ignore ce champ reconstitue quand même tous les octets
// en les concaténant dans l'ordre — rien n'est perdu, seul le découpage en
// évènements change.
struct PasteEvent { std::string text; bool complete = true; };
struct FocusEvent { bool focused = false; };

using InputEvent = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent>;

}  // namespace sshos
