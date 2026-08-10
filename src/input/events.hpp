#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace sshos {

enum class Key : uint16_t {
  None, Char, Enter, Tab, BackTab, Backspace, Escape,
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

struct PasteEvent { std::string text; };
struct FocusEvent { bool focused = false; };

using InputEvent = std::variant<KeyEvent, MouseEvent, PasteEvent, FocusEvent>;

}  // namespace sshos
