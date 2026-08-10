#include "input/parser.hpp"

#include <cstdlib>
#include <vector>

#include "common/utf8.hpp"

namespace sshos {
namespace {

constexpr std::string_view kPasteEnd = "\033[201~";

// C2 : au-delà de cette taille sans terminateur, un collage est livré par
// fragments plutôt que de faire grossir le tampon interne sans limite. Un
// collage est du contenu qu'un humain a voulu livrer : le fragmenter
// préserve tous les octets (cf. PasteEvent::complete dans events.hpp),
// contrairement à un CSI démesuré (kMaxCsiParamsLen ci-dessous), que
// personne n'a tapé et qu'on peut abandonner sans rien devoir.
constexpr size_t kMaxPasteChunk = 1 * 1024 * 1024;

// C2 : au-delà de cette longueur de paramètres sans octet final, une
// séquence CSI n'est plus quelque chose qu'un terminal légitime émettrait.
// On l'abandonne et on resynchronise sur la suite plutôt que de laisser le
// tampon grossir en attendant une fin qui ne vient jamais.
constexpr size_t kMaxCsiParamsLen = 128;

// C1 : plafond de saturation de l'accumulateur de paramètre dans
// split_params. Très au-delà de toute coordonnée, tout code tilde ou tout
// paramètre de modificateur légitime (cf. mods_from_param) ; sert
// uniquement à empêcher le débordement arithmétique signé (comportement
// indéterminé) d'une chaîne de chiffres démesurée. La valeur saturée n'est
// jamais ensuite traitée comme un modificateur plausible : voir
// mods_from_param, qui rejette explicitement tout ce qui dépasse la plage
// légitime du protocole plutôt que d'agir dessus par simple modulo.
constexpr int kMaxCsiParam = 65535;

Key key_from_tilde(int n) {
  switch (n) {
    case 1: return Key::Home;
    case 2: return Key::Insert;
    case 3: return Key::Delete;
    case 4: return Key::End;
    case 5: return Key::PgUp;
    case 6: return Key::PgDn;
    case 15: return Key::F5;
    case 17: return Key::F6;
    case 18: return Key::F7;
    case 19: return Key::F8;
    case 20: return Key::F9;
    case 21: return Key::F10;
    case 23: return Key::F11;
    case 24: return Key::F12;
    // M1 : un code numérique absent de cette table ressemble bel et bien à
    // une touche encodée en tilde (contrairement à la molette ou au focus,
    // court-circuités avant d'arriver ici) ; Unknown le dit, là où None
    // laissait croire qu'il n'y avait rien à rapporter.
    default: return Key::Unknown;
  }
}

Key key_from_final(char f) {
  switch (f) {
    case 'A': return Key::Up;
    case 'B': return Key::Down;
    case 'C': return Key::Right;
    case 'D': return Key::Left;
    case 'H': return Key::Home;
    case 'F': return Key::End;
    case 'Z': return Key::BackTab;
    case 'P': return Key::F1;
    case 'Q': return Key::F2;
    case 'R': return Key::F3;
    case 'S': return Key::F4;
    // M1 : même raisonnement que key_from_tilde ci-dessus.
    default: return Key::Unknown;
  }
}

std::vector<int> split_params(std::string_view s) {
  std::vector<int> out;
  int cur = -1;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      const int base = (cur < 0 ? 0 : cur);
      const int digit = c - '0';
      // C1 : sature au lieu de déborder. `base > (kMaxCsiParam - digit) /
      // 10` détecte qu'ajouter un chiffre de plus dépasserait le plafond
      // AVANT de faire le calcul qui déborderait — jamais après.
      cur = (base > (kMaxCsiParam - digit) / 10) ? kMaxCsiParam
                                                  : base * 10 + digit;
    } else if (c == ';') {
      out.push_back(cur);
      cur = -1;
    }
  }
  out.push_back(cur);
  return out;
}

// La valeur du paramètre de modificateur xterm est 1 + un masque sur 4
// bits (Shift, Alt, Ctrl, Meta, dans cet ordre) : la plage légitime est
// donc [1,16]. Ce code n'a que les trois premiers bits dans mod:: ; Meta
// est ignoré par le masque 0x07 ci-dessous, comme avant ce correctif.
uint8_t mods_from_param(int p) {
  // C1 : au-delà de 16, la valeur ne peut provenir d'aucun terminal réel —
  // c'est un accumulateur saturé (cf. kMaxCsiParam) ou un pair hostile, pas
  // des bits de modificateur à extraire par modulo. Lui faire dire quelque
  // chose produirait un évènement confiant mais faux : c'est exactement
  // ainsi qu'un Haut Ctrl seul («\033[1;5A») suivi d'un flot de chiffres
  // devenait Alt+Ctrl («\033[1;599999999999999999999A», mods=6) avant ce
  // correctif — saturer l'accumulateur sans borner aussi ce qu'on en fait
  // n'aurait fait que rendre le mods=6 reproductible sans UB, pas correct.
  if (p <= 1 || p > 16) return 0;
  return static_cast<uint8_t>((p - 1) & 0x07);
}

// Nombre total d'octets attendus pour une séquence UTF-8 commençant par b0,
// lead byte inclus. Ne valide rien : sert seulement à savoir combien
// d'octets attendre avant de tenter un décodage complet. Toute la
// validation (sur-longueurs, surrogates, bornes) vit dans
// common::utf8_decode, partagé avec render/ (cf. finding I1).
int utf8_seq_len(unsigned char b0) {
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0) == 0xC0) return 2;
  if ((b0 & 0xF0) == 0xE0) return 3;
  if ((b0 & 0xF8) == 0xF0) return 4;
  return 1;  // lead byte invalide : utf8_decode le rendra en U+FFFD
}

}  // namespace

void InputParser::timeout() {
  esc_pending_ = false;

  // I2 : le collage a son propre mécanisme de bornage et de livraison par
  // fragments (cf. finding C2) ; un timeout ne doit jamais le tronquer.
  if (in_paste_ || buf_.empty()) return;

  const auto b0 = static_cast<unsigned char>(buf_[0]);

  if (b0 != 0x1b) {
    // Le seul état non-ESC qui reste en suspens ici est un caractère UTF-8
    // multi-octets tronqué (step() attend le reste, cf. utf8_seq_len) :
    // plus rien n'arrivera pour le compléter. utf8_decode rend U+FFFD et
    // consomme ce qu'il y a, jamais 0 : aucun octet ne reste bloqué.
    char32_t cp = 0;
    const size_t used = utf8_decode(buf_, 0, cp);
    ready_.push_back(InputEvent{KeyEvent{Key::Char, cp, 0}});
    buf_.erase(0, used);
    return;
  }

  // I2 : ESC amorce trois séquences ambiguës — lui seul, ESC+O (SS3), ou
  // ESC+[ (CSI). L'ancien timeout() ne résolvait que la première ; les deux
  // autres laissaient leurs octets bloqués indéfiniment dans buf_, prêts à
  // absorber la prochaine frappe sans rapport comme un faux octet final. Un
  // octet tapé par l'utilisateur ne doit jamais disparaître : les trois
  // états sont résolus ici. Une fois le délai expiré, rien de plus
  // n'arrivera : ESC ne peut plus se transformer en séquence longue, il ne
  // reste que lui-même, suivi des octets bruts déjà là, rendus honnêtement
  // caractère par caractère plutôt que devinés comme Alt/CSI/SS3.
  if (buf_.size() == 1) {
    buf_.clear();
    ready_.push_back(InputEvent{KeyEvent{Key::Escape, 0, 0}});
    return;
  }

  ready_.push_back(InputEvent{KeyEvent{Key::Escape, 0, 0}});
  for (size_t k = 1; k < buf_.size(); ++k) {
    ready_.push_back(InputEvent{
        KeyEvent{Key::Char, static_cast<char32_t>(static_cast<unsigned char>(buf_[k])), 0}});
  }
  buf_.clear();
}

void InputParser::pump() {
  while (!buf_.empty()) {
    const size_t used = step();
    if (used == 0) return;  // séquence incomplète : attendre d'autres octets
    buf_.erase(0, used);
  }
}

size_t InputParser::step() {
  esc_pending_ = false;

  if (in_paste_) {
    // I4 : ne rescanne pas tout buf_ à chaque octet livré (O(n²) sur un
    // collage nourri octet par octet). On reprend à partir d'où le scan
    // précédent s'est arrêté, en reculant de kPasteEnd.size()-1 pour ne
    // jamais rater un terminateur qui chevauche la frontière entre deux
    // feed() successifs.
    const size_t rescan_from = paste_scanned_ > kPasteEnd.size() - 1
                                    ? paste_scanned_ - (kPasteEnd.size() - 1)
                                    : 0;
    const size_t end = buf_.find(kPasteEnd, rescan_from);

    if (end != std::string::npos) {
      ready_.push_back(InputEvent{PasteEvent{buf_.substr(0, end), /*complete=*/true}});
      in_paste_ = false;
      paste_scanned_ = 0;
      return end + kPasteEnd.size();
    }

    // C2 : pas de terminateur en vue. Au-delà du plafond, livrer ce qu'on a
    // comme fragment intermédiaire (complete=false) plutôt que de
    // continuer à grossir : un collage est du contenu que l'utilisateur a
    // voulu livrer, il ne doit jamais disparaître, même découpé en
    // plusieurs évènements. On garde en réserve les derniers
    // kPasteEnd.size()-1 octets : le terminateur pourrait chevaucher tout
    // juste la coupe.
    if (buf_.size() >= kMaxPasteChunk) {
      const size_t keep = kPasteEnd.size() - 1;
      const size_t flush = buf_.size() - keep;
      ready_.push_back(InputEvent{PasteEvent{buf_.substr(0, flush), /*complete=*/false}});
      paste_scanned_ = 0;
      return flush;
    }

    paste_scanned_ =
        buf_.size() >= kPasteEnd.size() - 1 ? buf_.size() - (kPasteEnd.size() - 1) : 0;
    return 0;
  }

  const auto b0 = static_cast<unsigned char>(buf_[0]);

  if (b0 != 0x1b) {
    if (b0 == '\r' || b0 == '\n') {
      ready_.push_back(InputEvent{KeyEvent{Key::Enter, 0, 0}});
      return 1;
    }
    if (b0 == '\t') {
      ready_.push_back(InputEvent{KeyEvent{Key::Tab, 0, 0}});
      return 1;
    }
    if (b0 == 0x7f || b0 == 0x08) {
      ready_.push_back(InputEvent{KeyEvent{Key::Backspace, 0, 0}});
      return 1;
    }
    if (b0 < 0x20) {
      const char32_t letter = (b0 == 0) ? U' ' : static_cast<char32_t>(U'a' + b0 - 1);
      ready_.push_back(InputEvent{KeyEvent{Key::Char, letter, mod::Ctrl}});
      return 1;
    }
    // I1 : le décodage et la validation UTF-8 vivent désormais dans
    // common/utf8.*, partagés avec render/ (cf. le commentaire de
    // utf8_decode pour ce que ça ferme : sur-longueurs, surrogates,
    // continuations isolées, valeurs > 10FFFF...). utf8_seq_len ne fait que
    // prédire combien d'octets attendre avant de décoder pour de vrai ; ce
    // n'est pas une seconde implémentation de la validation.
    const int need = utf8_seq_len(b0);
    if (buf_.size() < static_cast<size_t>(need)) return 0;
    char32_t cp = 0;
    const size_t used = utf8_decode(buf_, 0, cp);
    ready_.push_back(InputEvent{KeyEvent{Key::Char, cp, 0}});
    return used;
  }

  // ESC seul : indécidable tant qu'aucun octet ne suit.
  if (buf_.size() == 1) {
    esc_pending_ = true;
    return 0;
  }

  if (buf_[1] == 'O') {
    if (buf_.size() < 3) return 0;
    const Key k = key_from_final(buf_[2]);
    if (k != Key::None) ready_.push_back(InputEvent{KeyEvent{k, 0, 0}});
    return 3;
  }

  if (buf_[1] != '[') {
    // ESC + octet : accord Alt.
    const auto b1 = static_cast<unsigned char>(buf_[1]);
    if (b1 >= 0x20 && b1 < 0x7f) {
      ready_.push_back(
          InputEvent{KeyEvent{Key::Char, static_cast<char32_t>(b1), mod::Alt}});
    }
    return 2;
  }

  // CSI : paramètres jusqu'à un octet final dans 0x40..0x7E.
  size_t i = 2;
  while (i < buf_.size()) {
    // C2 : une séquence CSI qui s'étire sans fin en vue n'est pas quelque
    // chose qu'un terminal légitime émet. On abandonne et on resynchronise
    // sans cérémonie sur ce qui suit — contrairement au collage, personne
    // n'a tapé ça, il n'y a rien à préserver.
    if (i - 2 >= kMaxCsiParamsLen) return i;
    const auto c = static_cast<unsigned char>(buf_[i]);
    if (c >= 0x40 && c <= 0x7E) break;
    ++i;
  }
  if (i >= buf_.size()) return 0;  // final pas encore arrivé

  const char final_byte = buf_[i];
  const std::string_view params(buf_.data() + 2, i - 2);
  const size_t used = i + 1;

  if (!params.empty() && params[0] == '<') {
    const auto p = split_params(params.substr(1));
    // I3 : un paramètre manquant se code comme -1 (cf. split_params : cur
    // reste à sa valeur initiale). Une souris SGR authentique ne les omet
    // jamais. cb=0 est un bouton légitime (clic gauche) donc n'est rejeté
    // que s'il est négatif ; x/y sont 1-indexés sur le fil (leur minimum
    // légitime est 1), donc rejetés dès qu'ils sont sous 1 — sinon un
    // câble à 0 produirait un x/y=-1 après la conversion 0-indexée juste en
    // dessous, violant le même contrat (events.hpp:32) qu'un paramètre
    // manquant, juste d'une unité de moins. Mieux vaut ne rien émettre que
    // d'agir sur une valeur sentinelle : cb=-1 forgeait un évènement
    // « molette + tous les modificateurs » avant ce correctif.
    if (p.size() < 3 || p[0] < 0 || p[1] < 1 || p[2] < 1) return used;
    const int cb = p[0];
    MouseEvent m;
    m.x = p[1] - 1;
    m.y = p[2] - 1;
    if ((cb & 4) != 0) m.mods |= mod::Shift;
    if ((cb & 8) != 0) m.mods |= mod::Alt;
    if ((cb & 16) != 0) m.mods |= mod::Ctrl;

    if ((cb & 64) != 0) {
      // Molette : jamais de relâchement, donc hors machine à états.
      m.action = (cb & 1) != 0 ? MouseAction::WheelDown : MouseAction::WheelUp;
    } else if ((cb & 32) != 0) {
      m.action = MouseAction::Motion;
      m.button = static_cast<uint8_t>(cb & 3);
    } else {
      m.action = final_byte == 'm' ? MouseAction::Release : MouseAction::Press;
      m.button = static_cast<uint8_t>(cb & 3);
    }
    ready_.push_back(InputEvent{m});
    return used;
  }

  if (final_byte == 'I' || final_byte == 'O') {
    ready_.push_back(InputEvent{FocusEvent{final_byte == 'I'}});
    return used;
  }

  const auto p = split_params(params);

  if (final_byte == '~') {
    const int n = p.empty() || p[0] < 0 ? 0 : p[0];
    if (n == 200) {
      in_paste_ = true;
      paste_scanned_ = 0;
      return used;
    }
    const Key k = key_from_tilde(n);
    if (k != Key::None) {
      ready_.push_back(
          InputEvent{KeyEvent{k, 0, p.size() > 1 ? mods_from_param(p[1]) : uint8_t{0}}});
    }
    return used;
  }

  const Key k = key_from_final(final_byte);
  if (k != Key::None) {
    ready_.push_back(
        InputEvent{KeyEvent{k, 0, p.size() > 1 ? mods_from_param(p[1]) : uint8_t{0}}});
  }
  return used;
}

}  // namespace sshos
