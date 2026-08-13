#pragma once

#include <cstdint>

namespace sshos {

struct Size {
  int w = 0;
  int h = 0;
  bool operator==(const Size&) const = default;
};

struct Pos {
  int x = 0;
  int y = 0;
  bool operator==(const Pos&) const = default;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool operator==(const Rect&) const = default;
  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

// Type somme explicite. Un entier nu confondrait SGR 39/49 (couleur par
// défaut du terminal) avec la couleur indexée 7, et écraserait le truecolor.
enum class ColorKind : uint8_t { Default, Indexed, Rgb };

struct Color {
  ColorKind kind = ColorKind::Default;
  uint8_t idx = 0;
  uint8_t r = 0, g = 0, b = 0;

  static constexpr Color def() { return {}; }
  static constexpr Color indexed(uint8_t i) { return {ColorKind::Indexed, i, 0, 0, 0}; }
  static constexpr Color rgb(uint8_t rr, uint8_t gg, uint8_t bb) {
    return {ColorKind::Rgb, 0, rr, gg, bb};
  }
  bool operator==(const Color&) const = default;
};

namespace attr {
inline constexpr uint16_t Bold = 1 << 0;
inline constexpr uint16_t Dim = 1 << 1;
inline constexpr uint16_t Italic = 1 << 2;
inline constexpr uint16_t Underline = 1 << 3;
inline constexpr uint16_t Reverse = 1 << 4;
inline constexpr uint16_t Strike = 1 << 5;
// Ces deux-là, l'interface du bureau ne les emploie pas ; ils existent
// pour le VT, qui doit pouvoir porter un SGR 5 ou un SGR 8 reçu de
// l'invité. Sans eux, `SGR 5` puis `SGR 25` ne se distinguerait plus
// d'une paire de non-opérations, et un mot de passe demandé caché
// s'afficherait en clair.
inline constexpr uint16_t Blink = 1 << 6;
inline constexpr uint16_t Hidden = 1 << 7;
}  // namespace attr

struct Style {
  Color fg = Color::def();
  Color bg = Color::def();
  uint16_t attrs = 0;
  bool operator==(const Style&) const = default;
};

// Jeu de caractères des encadrements. Il vit ici plutôt que dans theme.hpp
// pour que surface.hpp, qui déclare View::box(), n'ait pas à tirer
// profile.hpp derrière lui.
enum class Border { Unicode, Ascii };

// width : 1 normal, 2 pleine chasse, 0 cellule de continuation.
// cluster : 0 quand le graphème tient dans `ch`, sinon index dans le
// réservoir de la Surface. Le réservoir ne coûte que sur ce qui l'exige.
struct Cell {
  char32_t ch = U' ';
  uint32_t cluster = 0;
  Color fg = Color::def();
  Color bg = Color::def();
  uint16_t attrs = 0;
  uint8_t width = 1;
  bool operator==(const Cell&) const = default;
};

inline constexpr Cell kContinuation{U'\0', 0, Color::def(), Color::def(), 0, 0};

}  // namespace sshos
