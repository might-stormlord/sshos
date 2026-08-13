#include "render/profile.hpp"

#include <array>
#include <cstdio>

namespace sshos {

// Déclarée en avant : color_code(), qui vit dans le namespace anonyme
// ci-dessous, s'appuie dessus, mais la définition doit rester hors de cet
// anonyme puisque le header l'expose.
Color quantize_color(const Color& c, ColorDepth d);

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

  if (p.depth == ColorDepth::TrueColor) {
    uint8_t r = c.r;
    uint8_t g = c.g;
    uint8_t b = c.b;
    if (c.kind == ColorKind::Indexed) indexed_to_rgb(c.idx, r, g, b);
    return "\033[" + num(base) + ";2;" + num(r) + ";" + num(g) + ";" + num(b) + "m";
  }

  // Une seule règle de réduction dans tout le projet : celle de
  // quantize_color. Le thème et l'émetteur SGR doivent répondre la même
  // chose, sinon le thème « prouve » une distinction que l'écran ne montre
  // pas.
  const Color q = quantize_color(c, p.depth);
  if (p.depth == ColorDepth::Indexed256)
    return "\033[" + num(base) + ";5;" + num(q.idx) + "m";
  return "\033[" + num(simple + q.idx) + "m";
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

}  // namespace

Color quantize_color(const Color& c, ColorDepth d) {
  if (c.kind == ColorKind::Default) return c;
  if (d == ColorDepth::TrueColor) return c;

  uint8_t r = c.r;
  uint8_t g = c.g;
  uint8_t b = c.b;
  if (c.kind == ColorKind::Indexed) indexed_to_rgb(c.idx, r, g, b);

  if (d == ColorDepth::Indexed256) {
    // Un index reste lui-même : le terminal en connaît déjà les 256, le
    // faire transiter par le cube 6x6x6 ne ferait que le dégrader.
    if (c.kind == ColorKind::Indexed) return c;
    return Color::indexed(quantize_256(r, g, b));
  }
  return Color::indexed(static_cast<uint8_t>(quantize_16(r, g, b)));
}

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

  // Blink et Hidden ne viennent jamais de l'interface, seulement d'un
  // invité du terminal. Ils figurent tout de même ici : les poser sans
  // les émettre laisserait un SGR 8 s'afficher en clair, et ferait
  // réinitialiser le pinceau pour un changement que personne ne verrait.
  const std::array<std::pair<uint16_t, int>, 8> codes{{
      {attr::Bold, 1},
      {attr::Dim, 2},
      {attr::Italic, 3},
      {attr::Underline, 4},
      {attr::Blink, 5},
      {attr::Reverse, 7},
      {attr::Hidden, 8},
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

// encode_utf8() a déménagé, byte pour byte, dans common/utf8.cpp (aux
// côtés d'utf8_decode() qu'elle complète — voir le commentaire de
// common/utf8.hpp pour la raison du regroupement). Ce fichier ne
// l'appelait pas lui-même ; aucun comportement de sgr_transition() ou de
// color_code() n'a changé.

}  // namespace sshos
