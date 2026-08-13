#include "input/encode.hpp"

#include <string>

#include "common/utf8.hpp"

namespace sshos {
namespace {

// La forme que prend une touche NON MODIFIÉE. Trois règles, et les
// confondre est le défaut classique : les flèches suivent `DECCKM`, `F1` à
// `F4` sont TOUJOURS en SS3, et la tabulation arrière n'a jamais de forme
// applicative -- `\033OZ` n'existe nulle part.
enum class Plain { Csi, Ss3Always, Ss3WhenApplication };

struct FinalKey {
  Key key;
  char final_byte;
  Plain plain;
};

// Les touches dont la séquence se termine par une lettre.
constexpr FinalKey kFinals[] = {
    {Key::Up, 'A', Plain::Ss3WhenApplication},
    {Key::Down, 'B', Plain::Ss3WhenApplication},
    {Key::Right, 'C', Plain::Ss3WhenApplication},
    {Key::Left, 'D', Plain::Ss3WhenApplication},
    {Key::Home, 'H', Plain::Ss3WhenApplication},
    {Key::End, 'F', Plain::Ss3WhenApplication},
    {Key::F1, 'P', Plain::Ss3Always},
    {Key::F2, 'Q', Plain::Ss3Always},
    {Key::F3, 'R', Plain::Ss3Always},
    {Key::F4, 'S', Plain::Ss3Always},
    {Key::BackTab, 'Z', Plain::Csi},
};

// Les touches dont la séquence se termine par `~`, désignées par un code.
struct TildeKey {
  Key key;
  int code;
};

constexpr TildeKey kTildes[] = {
    {Key::Insert, 2}, {Key::Delete, 3},  {Key::PgUp, 5},   {Key::PgDn, 6},
    {Key::F5, 15},    {Key::F6, 17},     {Key::F7, 18},    {Key::F8, 19},
    {Key::F9, 20},    {Key::F10, 21},    {Key::F11, 23},   {Key::F12, 24},
};

// Le paramètre de modificateur d'xterm : 1 + le masque. C'est exactement
// ce que `mods_from_param()` décode en face.
std::string mod_param(uint8_t mods) {
  return std::to_string(static_cast<int>(mods & 0x07) + 1);
}

const FinalKey* find_final(Key k) {
  for (const FinalKey& f : kFinals) {
    if (f.key == k) return &f;
  }
  return nullptr;
}

const TildeKey* find_tilde(Key k) {
  for (const TildeKey& t : kTildes) {
    if (t.key == k) return &t;
  }
  return nullptr;
}

// L'octet de contrôle d'un accord `Ctrl`, ou 0 si ce caractère n'en a pas.
// `Ctrl+Espace` vaut zéro DE PLEIN DROIT -- c'est ce que le parseur rend
// en face -- d'où le drapeau de sortie plutôt qu'une valeur sentinelle.
bool control_byte(char32_t ch, char& out) {
  if (ch >= U'a' && ch <= U'z') {
    out = static_cast<char>(ch - U'a' + 1);
    return true;
  }
  if (ch >= U'A' && ch <= U'Z') {
    out = static_cast<char>(ch - U'A' + 1);
    return true;
  }
  if (ch == U' ') {
    out = '\0';
    return true;
  }
  return false;
}

std::string encode_char(const KeyEvent& key) {
  std::string body;
  char ctrl = 0;
  if ((key.mods & mod::Ctrl) != 0 && control_byte(key.ch, ctrl)) {
    body.assign(1, ctrl);
  } else {
    body = encode_utf8(key.ch);
  }
  // `Alt` PRÉFIXE, il ne remplace pas : `Alt+Ctrl+a` porte les deux.
  if ((key.mods & mod::Alt) != 0) return "\033" + body;
  return body;
}

}  // namespace

std::string encode_key(const KeyEvent& key, bool cursor_keys_application) {
  if (key.key == Key::Char) return encode_char(key);

  // Les quatre touches qui SONT un octet ASCII. Elles n'ont pas de forme
  // modifiée dans le fil -- leur octet est le même, modificateur ou pas --
  // sauf `Alt`, qui préfixe tout.
  const char* ascii = nullptr;
  switch (key.key) {
    case Key::Enter:
      ascii = "\r";
      break;
    case Key::Tab:
      ascii = "\t";
      break;
    case Key::Backspace:
      ascii = "\177";
      break;
    case Key::Escape:
      ascii = "\033";
      break;
    default:
      break;
  }
  if (ascii != nullptr) {
    std::string out = ascii;
    if ((key.mods & mod::Alt) != 0) out.insert(0, "\033");
    return out;
  }

  const uint8_t mods = key.mods & 0x07;

  if (const FinalKey* f = find_final(key.key)) {
    const std::string final_byte(1, f->final_byte);
    if (mods != 0) {
      // Une touche MODIFIÉE reprend toujours la forme CSI : la forme SS3
      // n'a pas de place où mettre le paramètre.
      return "\033[1;" + mod_param(mods) + final_byte;
    }
    const bool ss3 = f->plain == Plain::Ss3Always ||
                     (f->plain == Plain::Ss3WhenApplication &&
                      cursor_keys_application);
    return (ss3 ? "\033O" : "\033[") + final_byte;
  }

  if (const TildeKey* t = find_tilde(key.key)) {
    const std::string code = std::to_string(t->code);
    if (mods != 0) return "\033[" + code + ";" + mod_param(mods) + "~";
    return "\033[" + code + "~";
  }

  // `None` et `Unknown` : rien à envoyer. Une chaîne non vide mettrait des
  // octets parasites dans le fil de l'invité.
  return "";
}

}  // namespace sshos
