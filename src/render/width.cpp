#include "render/width.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "common/utf8.hpp"

namespace sshos {
namespace {

struct Range {
  char32_t lo, hi;
};

// Marques combinantes, formateurs, sélecteurs de variation.
constexpr std::array<Range, 14> kZero{{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x0E31, 0x0E31},
    {0x0E47, 0x0E4E}, {0x200B, 0x200F}, {0x2060, 0x2064}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF},
}};

// East Asian Wide et Fullwidth.
constexpr std::array<Range, 15> kWide{{
    {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
    {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
}};

// East Asian Ambiguous : large ou étroit selon le terminal, d'où la sonde.
constexpr std::array<Range, 8> kAmbiguous{{
    {0x00A1, 0x00A1}, {0x00B0, 0x00B4}, {0x00B6, 0x00BA}, {0x2010, 0x2027},
    {0x2190, 0x21FF}, {0x2500, 0x257F}, {0x25A0, 0x25FF}, {0x2E80, 0x2E80},
}};

template <std::size_t N>
bool in(const std::array<Range, N>& table, char32_t cp) {
  const auto it = std::upper_bound(
      table.begin(), table.end(), cp,
      [](char32_t v, const Range& r) { return v < r.lo; });
  if (it == table.begin()) return false;
  return cp <= std::prev(it)->hi;
}

bool g_ambiguous_wide = false;

}  // namespace

void set_ambiguous_wide(bool wide) { g_ambiguous_wide = wide; }
bool ambiguous_wide() { return g_ambiguous_wide; }

int char_width(char32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  if (in(kZero, cp)) return 0;
  if (g_ambiguous_wide && in(kAmbiguous, cp)) return 2;
  if (in(kWide, cp)) return 2;
  return 1;
}

int text_cells(std::string_view s) {
  int n = 0;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    i += utf8_decode(s, i, cp);
    n += char_width(cp);
  }
  return n;
}

std::string elide_to_cells(std::string_view s, int cells, std::string_view mark) {
  if (cells <= 0) return std::string();
  if (text_cells(s) <= cells) return std::string(s);

  const int keep = cells - text_cells(mark);
  // Même la marque ne tient pas : rien ne tient. Mieux vaut une cellule vide
  // qu'un signe de coupure qui déborderait sur la bordure d'à côté.
  if (keep < 0) return std::string();

  int seen = 0;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    const size_t used = utf8_decode(s, i, cp);
    const int cw = char_width(cp);
    if (seen + cw > keep) break;
    seen += cw;
    i += used;
  }
  return std::string(s.substr(0, i)) + std::string(mark);
}

}  // namespace sshos
