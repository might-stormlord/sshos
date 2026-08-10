#include "render/profile.hpp"

#include <array>
#include <cstdio>

namespace sshos {
namespace {

std::string num(int v) { return std::to_string(v); }

// Cube 6x6x6 d'xterm, base 16.
uint8_t quantize_256(uint8_t r, uint8_t g, uint8_t b) {
  const auto q = [](uint8_t v) { return static_cast<int>(v * 5 / 255); };
  return static_cast<uint8_t>(16 + 36 * q(r) + 6 * q(g) + q(b));
}

// Bit 0 = rouge, bit 1 = vert, bit 2 = bleu : l'ordre ANSI historique.
int quantize_16(uint8_t r, uint8_t g, uint8_t b) {
  return (r > 127 ? 1 : 0) | (g > 127 ? 2 : 0) | (b > 127 ? 4 : 0);
}

void indexed_to_rgb(uint8_t idx, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (idx < 16) {
    const int lo = (idx & 8) != 0 ? 255 : 128;
    r = ((idx & 1) != 0) ? static_cast<uint8_t>(lo) : 0;
    g = ((idx & 2) != 0) ? static_cast<uint8_t>(lo) : 0;
    b = ((idx & 4) != 0) ? static_cast<uint8_t>(lo) : 0;
  } else if (idx < 232) {
    const int v = idx - 16;
    const std::array<int, 6> steps{0, 95, 135, 175, 215, 255};
    r = static_cast<uint8_t>(steps[(v / 36) % 6]);
    g = static_cast<uint8_t>(steps[(v / 6) % 6]);
    b = static_cast<uint8_t>(steps[v % 6]);
  } else {
    const auto v = static_cast<uint8_t>(8 + (idx - 232) * 10);
    r = g = b = v;
  }
}

std::string color_code(const Color& c, bool foreground, const OutputProfile& p) {
  const int base = foreground ? 38 : 48;
  const int simple = foreground ? 30 : 40;
  const int reset = foreground ? 39 : 49;

  if (c.kind == ColorKind::Default) return "\033[" + num(reset) + "m";

  uint8_t r = c.r;
  uint8_t g = c.g;
  uint8_t b = c.b;
  if (c.kind == ColorKind::Indexed) indexed_to_rgb(c.idx, r, g, b);

  switch (p.depth) {
    case ColorDepth::TrueColor:
      return "\033[" + num(base) + ";2;" + num(r) + ";" + num(g) + ";" + num(b) + "m";
    case ColorDepth::Indexed256:
      if (c.kind == ColorKind::Indexed)
        return "\033[" + num(base) + ";5;" + num(c.idx) + "m";
      return "\033[" + num(base) + ";5;" + num(quantize_256(r, g, b)) + "m";
    case ColorDepth::Mono16:
      return "\033[" + num(simple + quantize_16(r, g, b)) + "m";
  }
  return "";
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

}  // namespace

OutputProfile OutputProfile::detect(std::string_view term,
                                    std::string_view colorterm, bool utf8) {
  OutputProfile p;
  p.utf8 = utf8;
  if (contains(colorterm, "truecolor") || contains(colorterm, "24bit")) {
    p.depth = ColorDepth::TrueColor;
  } else if (contains(term, "256color")) {
    p.depth = ColorDepth::Indexed256;
  } else {
    p.depth = ColorDepth::Mono16;
  }
  return p;
}

std::string sgr_transition(const Style& from, const Style& to,
                           const OutputProfile& p) {
  if (from == to) return "";

  std::string out;
  Style base = from;

  // Aucun code n'éteint un attribut de façon portable : on réinitialise.
  if ((from.attrs & ~to.attrs) != 0) {
    out += "\033[0m";
    base = Style{};
  }

  const std::array<std::pair<uint16_t, int>, 6> codes{{
      {attr::Bold, 1},
      {attr::Dim, 2},
      {attr::Italic, 3},
      {attr::Underline, 4},
      {attr::Reverse, 7},
      {attr::Strike, 9},
  }};
  for (const auto& [bit, code] : codes) {
    if ((to.attrs & bit) != 0 && (base.attrs & bit) == 0) {
      out += "\033[" + num(code) + "m";
    }
  }

  if (!(base.fg == to.fg)) out += color_code(to.fg, true, p);
  if (!(base.bg == to.bg)) out += color_code(to.bg, false, p);
  return out;
}

std::string encode_utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

}  // namespace sshos
