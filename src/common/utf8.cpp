#include "common/utf8.hpp"

namespace sshos {

size_t utf8_decode(std::string_view s, size_t pos, char32_t& out) {
  out = U'�';
  if (pos >= s.size()) return 1;

  const auto b0 = static_cast<unsigned char>(s[pos]);
  int need = 0;
  char32_t cp = 0;

  if (b0 < 0x80) {
    out = b0;
    return 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    need = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    need = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    need = 3;
    cp = b0 & 0x07;
  } else {
    return 1;  // octet de continuation isolé ou séquence illégale
  }

  if (pos + need >= s.size() + 0 && pos + need > s.size() - 1) {
    // séquence tronquée : consommer ce qui est là, sans jamais rendre 0
    return s.size() - pos;
  }

  for (int k = 1; k <= need; ++k) {
    const auto bk = static_cast<unsigned char>(s[pos + k]);
    if ((bk & 0xC0) != 0x80) return static_cast<size_t>(k);
    cp = (cp << 6) | (bk & 0x3F);
  }

  // Stratégie : contrôle générique de la forme des continuations (0xC0 == 0x80),
  // consommation de la longueur nominale complète, puis validation post-hoc
  // du scalaire Unicode et substitution sur toute la portée si invalide.
  // Cette validation est charge-portante : elle est la seule défense contre
  // les surrogates [D800, DFFF], les valeurs > 10FFFF, et les séquences
  // trop longues (ex: E0 80 80 → 0). Ne pas la retirer : elle rouvrirait
  // les failles qu'elle ferme. (Différent des rangées par lead-byte du WHATWG.)
  const bool is_surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  const bool is_out_of_range = (cp > 0x10FFFF);
  const bool is_overlong = (need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
                           (need == 3 && cp < 0x10000);

  if (is_surrogate || is_out_of_range || is_overlong) {
    out = 0xFFFD;  // Caractère de remplacement
  } else {
    out = cp;
  }
  return static_cast<size_t>(need + 1);
}

// Déplacée depuis render/profile.cpp (déménagement à l'octet près, aucun
// changement de comportement — voir le commentaire d'encode_utf8() dans
// common/utf8.hpp pour la raison du regroupement avec utf8_decode()).
std::string encode_utf8(char32_t cp) {
  // Valider que cp est un scalaire Unicode valide.
  // Les surrogates [D800, DFFF] et les valeurs > 10FFFF doivent être rejetées.
  if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    cp = 0xFFFD;  // Caractère de remplacement
  }

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

std::string fold_to_ascii(std::string_view s) {
  struct Fold {
    char32_t cp;
    const char* to;
  };
  // Latin-1 supplément et Latin étendu-A, restreints à ce qu'un bureau
  // francophone écrit vraiment. Les ligatures rendent DEUX lettres, d'où
  // des chaînes plutôt que des caractères.
  static constexpr Fold kFolds[] = {
      {U'à', "a"},  {U'â', "a"},  {U'ä', "a"},  {U'á', "a"},  {U'ã', "a"},
      {U'å', "a"},  {U'ç', "c"},  {U'è', "e"},  {U'é', "e"},  {U'ê', "e"},
      {U'ë', "e"},  {U'ì', "i"},  {U'í', "i"},  {U'î', "i"},  {U'ï', "i"},
      {U'ñ', "n"},  {U'ò', "o"},  {U'ó', "o"},  {U'ô', "o"},  {U'ö', "o"},
      {U'õ', "o"},  {U'ù', "u"},  {U'ú', "u"},  {U'û', "u"},  {U'ü', "u"},
      {U'ý', "y"},  {U'ÿ', "y"},  {U'æ', "ae"}, {U'œ', "oe"}, {U'ß', "ss"},
      {U'À', "A"},  {U'Â', "A"},  {U'Ä', "A"},  {U'Á', "A"},  {U'Ã', "A"},
      {U'Å', "A"},  {U'Ç', "C"},  {U'È', "E"},  {U'É', "E"},  {U'Ê', "E"},
      {U'Ë', "E"},  {U'Ì', "I"},  {U'Í', "I"},  {U'Î', "I"},  {U'Ï', "I"},
      {U'Ñ', "N"},  {U'Ò', "O"},  {U'Ó', "O"},  {U'Ô', "O"},  {U'Ö', "O"},
      {U'Õ', "O"},  {U'Ù', "U"},  {U'Ú', "U"},  {U'Û', "U"},  {U'Ü', "U"},
      {U'Ý', "Y"},  {U'Æ', "AE"}, {U'Œ', "OE"},
      // Ponctuation typographique : elle arrive par les titres que les
      // applications posent, et vaut mieux qu'un point d'interrogation.
      {U'’', "'"},  {U'‘', "'"},  {U'“', "\""}, {U'”', "\""}, {U'–', "-"},
      {U'—', "-"},  {U'…', "..."}, {U'«', "\""}, {U'»', "\""},
  };

  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    i += utf8_decode(s, i, cp);
    if (cp < 0x80) {
      out += static_cast<char>(cp);
      continue;
    }
    const char* to = nullptr;
    for (const auto& f : kFolds) {
      if (f.cp == cp) {
        to = f.to;
        break;
      }
    }
    out += (to != nullptr) ? to : "?";
  }
  return out;
}

}  // namespace sshos
