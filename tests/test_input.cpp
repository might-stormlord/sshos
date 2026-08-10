#include <string>
#include <variant>
#include <vector>

#include "harness.hpp"
#include "input/parser.hpp"

using namespace sshos;

static std::vector<InputEvent> drain(InputParser& p, std::string_view bytes) {
  p.feed(bytes);
  std::vector<InputEvent> out;
  while (auto e = p.next()) out.push_back(*e);
  return out;
}

static KeyEvent one_key(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  return std::get<KeyEvent>(v.at(0));
}

static MouseEvent one_mouse(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  return std::get<MouseEvent>(v.at(0));
}

TEST(input_plain_characters) {
  const auto k = one_key("a");
  CHECK(k.key == Key::Char);
  CHECK_EQ(k.ch, U'a');
  CHECK_EQ(static_cast<int>(k.mods), 0);
}

TEST(input_control_characters) {
  const auto ctrl_a = one_key("\001");
  CHECK(ctrl_a.key == Key::Char);
  CHECK_EQ(ctrl_a.ch, U'a');
  CHECK_EQ(static_cast<int>(ctrl_a.mods), static_cast<int>(mod::Ctrl));

  CHECK(one_key("\r").key == Key::Enter);
  CHECK(one_key("\t").key == Key::Tab);
  CHECK(one_key("\177").key == Key::Backspace);
}

TEST(input_utf8_characters_wait_for_all_their_bytes) {
  InputParser p;
  CHECK(drain(p, "\xc3").empty());          // moitié de é
  const auto v = drain(p, "\xa9");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'é');
}

TEST(input_arrows_and_function_keys) {
  CHECK(one_key("\033[A").key == Key::Up);
  CHECK(one_key("\033[D").key == Key::Left);
  CHECK(one_key("\033[H").key == Key::Home);
  CHECK(one_key("\033[3~").key == Key::Delete);
  CHECK(one_key("\033[5~").key == Key::PgUp);
  CHECK(one_key("\033OP").key == Key::F1);
  CHECK(one_key("\033[15~").key == Key::F5);
  CHECK(one_key("\033[Z").key == Key::BackTab);
}

TEST(input_modified_arrows) {
  const auto k = one_key("\033[1;5A");  // Ctrl+Haut
  CHECK(k.key == Key::Up);
  CHECK_EQ(static_cast<int>(k.mods), static_cast<int>(mod::Ctrl));

  const auto s = one_key("\033[1;2C");  // Shift+Droite
  CHECK(s.key == Key::Right);
  CHECK_EQ(static_cast<int>(s.mods), static_cast<int>(mod::Shift));
}

// ESC isolé n'est décidable qu'au temps mort : c'est le préfixe de tout.
TEST(input_lone_escape_needs_the_timeout) {
  InputParser p;
  CHECK(drain(p, "\033").empty());
  CHECK(p.esc_pending());
  p.timeout();
  auto e = p.next();
  CHECK(e.has_value());
  CHECK(std::get<KeyEvent>(*e).key == Key::Escape);
  CHECK(!p.esc_pending());
}

TEST(input_escape_followed_by_a_letter_is_alt) {
  const auto k = one_key("\033a");
  CHECK(k.key == Key::Char);
  CHECK_EQ(k.ch, U'a');
  CHECK_EQ(static_cast<int>(k.mods), static_cast<int>(mod::Alt));
}

TEST(input_sgr_mouse_press_and_release) {
  const auto press = one_mouse("\033[<0;10;5M");
  CHECK(press.action == MouseAction::Press);
  CHECK_EQ(static_cast<int>(press.button), 0);
  CHECK_EQ(press.x, 9);   // 0-indexé
  CHECK_EQ(press.y, 4);

  const auto rel = one_mouse("\033[<0;10;5m");
  CHECK(rel.action == MouseAction::Release);
}

// Bit 32 = mouvement. Le mode 1002 le signale à chaque cellule franchie.
TEST(input_sgr_mouse_motion) {
  const auto m = one_mouse("\033[<32;10;5M");
  CHECK(m.action == MouseAction::Motion);
  CHECK_EQ(static_cast<int>(m.button), 0);
}

// Bit 64 = molette. Une molette n'émet JAMAIS de relâchement : la traiter
// comme un bouton verrouille la machine à états en glissement perpétuel.
TEST(input_wheel_is_not_a_button) {
  CHECK(one_mouse("\033[<64;1;1M").action == MouseAction::WheelUp);
  CHECK(one_mouse("\033[<65;1;1M").action == MouseAction::WheelDown);
}

TEST(input_mouse_modifier_bits) {
  const auto m = one_mouse("\033[<16;1;1M");  // +16 = Ctrl
  CHECK_EQ(static_cast<int>(m.mods), static_cast<int>(mod::Ctrl));
  const auto s = one_mouse("\033[<4;1;1M");   // +4 = Shift
  CHECK_EQ(static_cast<int>(s.mods), static_cast<int>(mod::Shift));
}

// Sans encadrement, chaque octet collé traverse le répartiteur de
// raccourcis : coller un transcript coloré tire des accords au hasard.
TEST(input_bracketed_paste_is_opaque) {
  InputParser p;
  const auto v = drain(p, "\033[200~\033[Ax\033[201~");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text, std::string("\033[Ax"));
}

TEST(input_incomplete_paste_yields_nothing) {
  InputParser p;
  CHECK(drain(p, "\033[200~abc").empty());
  const auto v = drain(p, "def\033[201~");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text, std::string("abcdef"));
}

TEST(input_focus_events) {
  InputParser p;
  const auto v = drain(p, "\033[I\033[O");
  CHECK_EQ(v.size(), static_cast<size_t>(2));
  CHECK(std::get<FocusEvent>(v.at(0)).focused);
  CHECK(!std::get<FocusEvent>(v.at(1)).focused);
}

// read() découpe où il veut : le parseur doit être insensible au découpage.
TEST(input_is_insensitive_to_chunk_boundaries) {
  const std::string wire = "\033[<0;10;5Mabc\033[1;5A\033[200~xy\033[201~";
  InputParser whole;
  const auto expected = drain(whole, wire);

  InputParser piecewise;
  std::vector<InputEvent> got;
  for (char c : wire) {
    piecewise.feed(std::string_view(&c, 1));
    while (auto e = piecewise.next()) got.push_back(*e);
  }
  CHECK_EQ(got.size(), expected.size());
  CHECK_EQ(got.size(), static_cast<size_t>(6));
}
