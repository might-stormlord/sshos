#include "input/parser.hpp"

#include <cstdlib>
#include <vector>

namespace sshos {
namespace {

constexpr std::string_view kPasteEnd = "\033[201~";

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
    default: return Key::None;
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
    default: return Key::None;
  }
}

std::vector<int> split_params(std::string_view s) {
  std::vector<int> out;
  int cur = -1;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
    } else if (c == ';') {
      out.push_back(cur);
      cur = -1;
    }
  }
  out.push_back(cur);
  return out;
}

// La valeur du paramètre de modificateur est 1 + un masque dont les bits
// sont, dans l'ordre, Shift, Alt, Ctrl — exactement la disposition de mod::.
uint8_t mods_from_param(int p) {
  if (p <= 1) return 0;
  return static_cast<uint8_t>((p - 1) & 0x07);
}

// Nombre d'octets attendus pour une séquence UTF-8 commençant par b0.
int utf8_len(unsigned char b0) {
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0) == 0xC0) return 2;
  if ((b0 & 0xF0) == 0xE0) return 3;
  if ((b0 & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

void InputParser::timeout() {
  if (esc_pending_ && buf_.size() == 1 && buf_[0] == '\033') {
    buf_.clear();
    ready_.push_back(InputEvent{KeyEvent{Key::Escape, 0, 0}});
  }
  esc_pending_ = false;
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
    const size_t end = buf_.find(kPasteEnd);
    if (end == std::string::npos) return 0;
    ready_.push_back(InputEvent{PasteEvent{buf_.substr(0, end)}});
    in_paste_ = false;
    return end + kPasteEnd.size();
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
    const int need = utf8_len(b0);
    if (buf_.size() < static_cast<size_t>(need)) return 0;
    char32_t cp = b0;
    if (need > 1) {
      cp = b0 & (0xFF >> (need + 1));
      for (int k = 1; k < need; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(buf_[k]) & 0x3F);
      }
    }
    ready_.push_back(InputEvent{KeyEvent{Key::Char, cp, 0}});
    return static_cast<size_t>(need);
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
    if (p.size() < 3) return used;
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
