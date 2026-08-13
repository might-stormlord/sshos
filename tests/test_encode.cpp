#include <string>
#include <vector>

#include "harness.hpp"
#include "input/encode.hpp"
#include "input/parser.hpp"

using sshos::encode_key;
using sshos::InputParser;
using sshos::Key;
using sshos::KeyEvent;
namespace mod = sshos::mod;

namespace {

// Rejoue les octets produits dans le VRAI parseur d'entrée et rend la
// touche qu'il en tire. L'aller-retour est la seule vérification qui
// mesure autre chose que ma lecture de la spec : les deux moitiés doivent
// se répondre.
KeyEvent round_trip(const KeyEvent& k, bool app) {
  InputParser p;
  p.feed(encode_key(k, app));
  // `ESC` seul est indécidable tant qu'aucun octet ne suit : c'est le
  // délai d'ambiguïté qui tranche, et le test le simule.
  p.timeout();
  const auto e = p.next();
  if (!e) return KeyEvent{Key::None, 0, 0};
  const auto* ke = std::get_if<KeyEvent>(&*e);
  return ke != nullptr ? *ke : KeyEvent{Key::None, 0, 0};
}

bool same(const KeyEvent& a, const KeyEvent& b) {
  return a.key == b.key && a.ch == b.ch && a.mods == b.mods;
}

// Toutes les touches nommées. `Char` est traité à part : sa valeur compte.
const std::vector<Key> kNamed = {
    Key::Enter,  Key::Tab,    Key::BackTab, Key::Backspace, Key::Escape,
    Key::Up,     Key::Down,   Key::Left,    Key::Right,     Key::Home,
    Key::End,    Key::PgUp,   Key::PgDn,    Key::Insert,    Key::Delete,
    Key::F1,     Key::F2,     Key::F3,      Key::F4,        Key::F5,
    Key::F6,     Key::F7,     Key::F8,      Key::F9,        Key::F10,
    Key::F11,    Key::F12,
};

}  // namespace

// ------------------------------------------------------------ l'aller-retour

// LA propriété : `parse(encode(k)) == k`, sur toute la table, dans les
// DEUX modes de DECCKM. Une table écrite à la main se vérifie à la
// relecture ; celle-ci se vérifie contre le code qui la lira pour de vrai.
TEST(encode_round_trips_every_named_key_in_normal_mode) {
  for (Key k : kNamed) {
    const KeyEvent in{k, 0, 0};
    CHECK(same(round_trip(in, false), in));
  }
}

TEST(encode_round_trips_every_named_key_in_application_mode) {
  for (Key k : kNamed) {
    const KeyEvent in{k, 0, 0};
    CHECK(same(round_trip(in, true), in));
  }
}

// Les modificateurs s'encodent en paramètre. L'aller-retour les rend.
TEST(encode_round_trips_every_named_key_with_each_modifier) {
  for (Key k : kNamed) {
    for (uint8_t m : {mod::Shift, mod::Alt, mod::Ctrl,
                      static_cast<uint8_t>(mod::Ctrl | mod::Shift)}) {
      // `Entrée`, `Tab`, `Retour arrière` et `Échap` n'ont pas de forme
      // modifiée dans le fil : leur octet est le même, modificateur ou
      // pas. Ce n'est pas encodable, donc pas vérifiable.
      if (k == Key::Enter || k == Key::Tab || k == Key::Backspace ||
          k == Key::Escape) {
        continue;
      }
      const KeyEvent in{k, 0, m};
      CHECK(same(round_trip(in, false), in));
    }
  }
}

TEST(encode_round_trips_a_plain_character) {
  const KeyEvent in{Key::Char, U'a', 0};
  CHECK(same(round_trip(in, false), in));
}

TEST(encode_round_trips_a_non_ascii_character) {
  const KeyEvent in{Key::Char, U'é', 0};
  CHECK(same(round_trip(in, false), in));
}

// -------------------------------------------------------- les accords ASCII

TEST(encode_turns_ctrl_letters_into_their_control_byte) {
  CHECK_EQ(encode_key(KeyEvent{Key::Char, U'a', mod::Ctrl}, false),
           std::string("\001"));
  CHECK_EQ(encode_key(KeyEvent{Key::Char, U'z', mod::Ctrl}, false),
           std::string("\032"));
}

// `Ctrl+Espace` est l'octet nul -- c'est ce que le parseur rend en face.
TEST(encode_turns_ctrl_space_into_a_null_byte) {
  CHECK_EQ(encode_key(KeyEvent{Key::Char, U' ', mod::Ctrl}, false),
           std::string("\0", 1));
}

TEST(encode_prefixes_an_alt_chord_with_escape) {
  CHECK_EQ(encode_key(KeyEvent{Key::Char, U'a', mod::Alt}, false),
           std::string("\033a"));
}

// `Alt+Ctrl+a` porte les deux : l'échappement PUIS l'octet de contrôle.
TEST(encode_stacks_alt_on_top_of_a_control_byte) {
  CHECK_EQ(encode_key(KeyEvent{Key::Char, U'a', mod::Alt | mod::Ctrl}, false),
           std::string("\033\001"));
}

// ------------------------------------------------------- les formes exactes

// DECCKM ne change QUE les flèches et les deux touches de bord. S'y
// tromper rend `vim` inutilisable en mode insertion.
TEST(encode_uses_the_application_form_of_the_arrows_when_decckm_is_on) {
  CHECK_EQ(encode_key(KeyEvent{Key::Up, 0, 0}, false), std::string("\033[A"));
  CHECK_EQ(encode_key(KeyEvent{Key::Up, 0, 0}, true), std::string("\033OA"));
  CHECK_EQ(encode_key(KeyEvent{Key::Home, 0, 0}, true), std::string("\033OH"));
}

// Une flèche MODIFIÉE reprend la forme CSI même en mode applicatif : c'est
// ce que fait xterm, et la forme SS3 n'a pas de place où mettre le
// paramètre.
TEST(encode_falls_back_to_the_csi_form_for_a_modified_arrow) {
  CHECK_EQ(encode_key(KeyEvent{Key::Right, 0, mod::Ctrl}, true),
           std::string("\033[1;5C"));
}

TEST(encode_writes_the_modifier_as_a_parameter) {
  CHECK_EQ(encode_key(KeyEvent{Key::Right, 0, mod::Ctrl}, false),
           std::string("\033[1;5C"));
  CHECK_EQ(encode_key(KeyEvent{Key::Delete, 0, mod::Shift}, false),
           std::string("\033[3;2~"));
}

TEST(encode_gives_the_classic_forms_to_the_unmodified_keys) {
  CHECK_EQ(encode_key(KeyEvent{Key::Enter, 0, 0}, false), std::string("\r"));
  CHECK_EQ(encode_key(KeyEvent{Key::Tab, 0, 0}, false), std::string("\t"));
  CHECK_EQ(encode_key(KeyEvent{Key::Backspace, 0, 0}, false),
           std::string("\177"));
  CHECK_EQ(encode_key(KeyEvent{Key::Escape, 0, 0}, false), std::string("\033"));
  CHECK_EQ(encode_key(KeyEvent{Key::BackTab, 0, 0}, false),
           std::string("\033[Z"));
  CHECK_EQ(encode_key(KeyEvent{Key::F1, 0, 0}, false), std::string("\033OP"));
  CHECK_EQ(encode_key(KeyEvent{Key::F5, 0, 0}, false), std::string("\033[15~"));
}

// Rien à envoyer pour ce qui n'est pas une touche : le liant écrit ce
// qu'on lui rend, et une chaîne non vide mettrait des octets parasites
// dans le fil de l'invité.
TEST(encode_sends_nothing_for_a_key_that_is_not_one) {
  CHECK_EQ(encode_key(KeyEvent{Key::None, 0, 0}, false), std::string(""));
  CHECK_EQ(encode_key(KeyEvent{Key::Unknown, 0, 0}, false), std::string(""));
}

// `Alt` préfixe AUSSI les touches qui sont un octet ASCII. L'aller-retour
// ne peut pas le vérifier -- le parseur ne reconnaît un accord `Alt` que
// sur un octet imprimable -- mais un vrai terminal l'émet, et `Alt+Entrée`
// est un raccourci courant.
TEST(encode_prefixes_an_alt_chord_on_an_ascii_key) {
  CHECK_EQ(encode_key(KeyEvent{Key::Enter, 0, mod::Alt}, false),
           std::string("\033\r"));
  CHECK_EQ(encode_key(KeyEvent{Key::Backspace, 0, mod::Alt}, false),
           std::string("\033\177"));
}

// Et les autres modificateurs ne changent RIEN sur ces quatre touches :
// leur octet est le même, et inventer une forme modifiée enverrait à
// l'invité une séquence qu'aucun terminal ne produit.
TEST(encode_leaves_the_ascii_keys_alone_under_ctrl_and_shift) {
  CHECK_EQ(encode_key(KeyEvent{Key::Enter, 0, mod::Ctrl}, false),
           std::string("\r"));
  CHECK_EQ(encode_key(KeyEvent{Key::Tab, 0, mod::Shift}, false),
           std::string("\t"));
}

// La tabulation arrière n'a JAMAIS de forme applicative : `\033OZ`
// n'existe nulle part, et l'émettre laisserait un `vim` sans son
// `Maj+Tab`.
TEST(encode_never_puts_the_back_tab_in_application_form) {
  CHECK_EQ(encode_key(KeyEvent{Key::BackTab, 0, 0}, true),
           std::string("\033[Z"));
}

// `F1` à `F4` sont en SS3 même hors mode applicatif : c'est leur forme
// normale, et DECCKM ne les concerne pas.
TEST(encode_keeps_the_function_keys_in_ss3_whatever_the_mode) {
  CHECK_EQ(encode_key(KeyEvent{Key::F4, 0, 0}, false), std::string("\033OS"));
  CHECK_EQ(encode_key(KeyEvent{Key::F4, 0, 0}, true), std::string("\033OS"));
}
