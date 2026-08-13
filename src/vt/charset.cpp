#include "vt/charset.hpp"

namespace sshos {
namespace {

// La table du jeu semi-graphique DEC. Elle ne couvre QUE `0x5F` à `0x7E` :
// c'est ce que la norme redéfinit, et rien d'autre. Les chiffres et les
// majuscules restent eux-mêmes, ce qui permet à un titre écrit pendant que
// le jeu est actif de rester lisible.
constexpr char32_t kGraphics[] = {
    U' ',                                              // 0x5F  _
    U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', U'±',  // `abcdefg
    U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼', U'⎺',  // hijklmno
    U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', U'┬',  // pqrstuvw
    U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·',        // xyz{|}~
};

constexpr char32_t kFirst = 0x5F;
constexpr char32_t kLast = 0x7E;

static_assert(sizeof(kGraphics) / sizeof(kGraphics[0]) == kLast - kFirst + 1,
              "la table doit couvrir exactement 0x5F a 0x7E");

}  // namespace

Charset charset_from_final(uint8_t final_byte) {
  // Tout final autre que `0` rend l'ASCII, y compris ceux que la norme
  // définit et que nous n'avons pas : lire des lettres est toujours moins
  // faux que lire des traits arbitraires.
  return final_byte == '0' ? Charset::Graphics : Charset::Ascii;
}

char32_t translate(char32_t cp, Charset set) {
  if (set != Charset::Graphics) return cp;
  if (cp < kFirst || cp > kLast) return cp;
  return kGraphics[cp - kFirst];
}

}  // namespace sshos
