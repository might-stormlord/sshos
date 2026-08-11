#include "harness.hpp"
#include "input/shortcuts.hpp"

using sshos::Action;
using sshos::Key;
using sshos::KeyEvent;
using sshos::LeaderDispatch;
using sshos::LeaderResult;

namespace {
KeyEvent ctrl(char32_t c) { return KeyEvent{Key::Char, c, sshos::mod::Ctrl}; }
KeyEvent plain(char32_t c) { return KeyEvent{Key::Char, c, 0}; }
}  // namespace

// L'ambiguïté que la spec ne pouvait pas exprimer avec un seul optional :
// la touche leader est avalée SANS produire d'action.
TEST(leader_swallows_its_own_key_without_producing_an_action) {
  LeaderDispatch d;
  CHECK(!d.armed());
  const LeaderResult r = d.feed(ctrl(U'a'));
  CHECK(r.consumed);
  CHECK(!r.action.has_value());
  CHECK(d.armed());
}

TEST(leader_passes_an_ordinary_key_straight_through) {
  LeaderDispatch d;
  const LeaderResult r = d.feed(plain(U'x'));
  CHECK(!r.consumed);
  CHECK(!r.action.has_value());
  CHECK(!d.armed());

  // Et un 'a' SANS Ctrl n'arme rien : c'est le modificateur qui fait la
  // touche leader, pas la lettre.
  const LeaderResult plain_a = d.feed(plain(U'a'));
  CHECK(!plain_a.consumed);
  CHECK(!d.armed());
}

TEST(leader_maps_the_armed_key_to_its_action_and_disarms) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(plain(U'w'));
  CHECK(r.consumed);
  REQUIRE(r.action.has_value());
  CHECK(*r.action == Action::Close);
  CHECK(!d.armed());
}

// Taper la touche leader deux fois de suite la transmet littéralement --
// sans quoi Ctrl+A deviendrait intapable pour l'application en dessous.
TEST(leader_typed_twice_yields_the_literal_key) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(ctrl(U'a'));
  CHECK(r.consumed);
  REQUIRE(r.action.has_value());
  CHECK(*r.action == Action::LiteralLeader);
  CHECK(!d.armed());
}

// Une touche armée sans liaison désarme et n'exécute rien. Elle est
// consommée : l'utilisateur a commencé un accord, il ne s'attend pas à voir
// la lettre apparaître dans son document.
TEST(leader_disarms_on_an_unbound_key_without_leaking_it) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(plain(U'é'));
  CHECK(r.consumed);
  CHECK(!r.action.has_value());
  CHECK(!d.armed());
}

// Une touche qui n'est pas un caractère désarme aussi : une flèche après
// l'accord ne doit pas rester en attente indéfiniment.
TEST(leader_disarms_on_a_non_character_key) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(KeyEvent{Key::Up, 0, 0});
  CHECK(r.consumed);
  CHECK(!r.action.has_value());
  CHECK(!d.armed());
}

TEST(leader_covers_every_action_of_the_table) {
  const struct {
    KeyEvent k;
    Action a;
  } table[] = {
      {plain(U'h'), Action::MoveLeft},        {plain(U'l'), Action::MoveRight},
      {plain(U'k'), Action::MoveUp},          {plain(U'j'), Action::MoveDown},
      {plain(U'L'), Action::GrowWidth},       {plain(U'H'), Action::ShrinkWidth},
      {plain(U'J'), Action::GrowHeight},      {plain(U'K'), Action::ShrinkHeight},
      {plain(U'n'), Action::NextWindow},      {plain(U'p'), Action::PrevWindow},
      {plain(U'w'), Action::Close},           {plain(U'm'), Action::Minimize},
      {plain(U'x'), Action::MaximizeToggle},  {plain(U'f'), Action::FullscreenToggle},
      {plain(U' '), Action::OpenMenu},        {plain(U'o'), Action::ToggleMouse},
      {plain(U'r'), Action::ForceRepaint},
  };
  for (const auto& e : table) {
    LeaderDispatch d;
    d.feed(ctrl(U'a'));
    const LeaderResult r = d.feed(e.k);
    REQUIRE(r.action.has_value());
    CHECK(*r.action == e.a);
  }
}

// La touche leader se choisit : rien dans le dispatcheur ne suppose 'a'.
TEST(leader_key_is_configurable) {
  LeaderDispatch d(U'b');
  CHECK(!d.feed(ctrl(U'a')).consumed);
  CHECK(!d.armed());
  CHECK(d.feed(ctrl(U'b')).consumed);
  CHECK(d.armed());
}
