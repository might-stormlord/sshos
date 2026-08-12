#include <string>
#include <vector>

#include "harness.hpp"
#include "input/shortcuts.hpp"
#include "render/width.hpp"

using sshos::Action;
using sshos::Key;
using sshos::KeyEvent;
using sshos::LeaderDispatch;
using sshos::LeaderResult;

namespace mod = sshos::mod;

namespace {
KeyEvent ctrl(char32_t c) { return KeyEvent{Key::Char, c, sshos::mod::Ctrl}; }
KeyEvent plain(char32_t c) { return KeyEvent{Key::Char, c, 0}; }
KeyEvent arrow(Key k, uint8_t mods) { return KeyEvent{k, 0, mods}; }
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

// Une touche qui n'est pas un caractère désarme aussi. Échap sert
// d'exemple depuis que les flèches ont une liaison : ce qui compte est
// qu'AUCUNE touche ne laisse l'accord en attente indéfiniment.
TEST(leader_disarms_on_a_non_character_key) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(KeyEvent{Key::Escape, 0, 0});
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
      {plain(U'w'), Action::Close},           {plain(U'-'), Action::Minimize},
      {plain(U'z'), Action::MaximizeToggle},  {plain(U'f'), Action::FullscreenToggle},
      {plain(U' '), Action::OpenMenu},        {plain(U'm'), Action::ToggleMouse},
      {plain(U'r'), Action::ForceRepaint},    {plain(U'd'), Action::Detach},
      {plain(U'?'), Action::ShowHelp},
      // Le chemin principal de la spec §7.4, celui qu'un utilisateur venu
      // d'un vrai bureau essaie en premier. Il avait disparu de la table
      // sans que rien ne le signale, faute d'un test qui l'exige.
      {arrow(Key::Left, 0), Action::MoveLeft},
      {arrow(Key::Right, 0), Action::MoveRight},
      {arrow(Key::Up, 0), Action::MoveUp},
      {arrow(Key::Down, 0), Action::MoveDown},
      {arrow(Key::Left, mod::Shift), Action::ShrinkWidth},
      {arrow(Key::Right, mod::Shift), Action::GrowWidth},
      {arrow(Key::Up, mod::Shift), Action::ShrinkHeight},
      {arrow(Key::Down, mod::Shift), Action::GrowHeight},
      {arrow(Key::Tab, 0), Action::NextWindow},
      {arrow(Key::Tab, mod::Shift), Action::PrevWindow},
      // Les deux écritures de « Maj+Tab » se rejoignent : `\033[Z` le dit
      // par le nom de la touche, `\033[1;2I` par le modificateur.
      {arrow(Key::BackTab, 0), Action::PrevWindow},
      {arrow(Key::BackTab, mod::Shift), Action::PrevWindow},
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

// Le garde-fou contre la dérive qui a rendu ce round nécessaire : la table
// des raccourcis s'était éloignée de la spec §7.4 -- flèches et Tab
// disparues, `m` volé à la bascule souris -- sans qu'aucun test ne s'en
// aperçoive, parce que chaque test vérifiait la table telle qu'elle était.
// Celui-ci ne vérifie pas une liaison : il vérifie que rien d'atteignable
// au clavier n'échappe à l'aide, donc à l'utilisateur.
TEST(every_bound_action_appears_in_the_help) {
  for (const Action a : sshos::bound_actions()) {
    bool documented = false;
    for (const auto& row : sshos::binding_help()) {
      for (const Action ra : row.actions) documented = documented || ra == a;
    }
    if (!documented) {
      th::fail(__FILE__, __LINE__,
               "action " + std::to_string(static_cast<int>(a)) +
                   " liee au clavier mais absente de l'aide");
    }
  }
}

// Et la réciproque : une ligne d'aide qui documenterait un accord retiré
// enverrait l'utilisateur taper dans le vide.
TEST(the_help_documents_nothing_that_is_not_bound) {
  const std::vector<Action> bound = sshos::bound_actions();
  for (const auto& row : sshos::binding_help()) {
    for (const Action ra : row.actions) {
      bool exists = false;
      for (const Action b : bound) exists = exists || b == ra;
      if (!exists) {
        th::fail(__FILE__, __LINE__,
                 std::string("l'aide annonce « ") + row.keys +
                     " » qui n'est lie a rien");
      }
    }
  }
}

// L'aide se lit dans un cadre : deux lignes qui ne tiennent pas dans la
// même largeur se liraient de travers. Rien ne l'impose au compilateur --
// d'où ce test, qui borne aussi la table pour qu'elle reste affichable sur
// un terminal étroit.
TEST(the_help_stays_within_a_readable_width) {
  for (const auto& row : sshos::binding_help()) {
    CHECK(sshos::text_cells(row.keys) <= 20);
    CHECK(sshos::text_cells(row.what) <= 30);
    CHECK(sshos::text_cells(row.keys) > 0);
    CHECK(sshos::text_cells(row.what) > 0);
  }
}

// ---------------------------------------------------------------------------
// Les accords qui s'enchaînent. Sans eux, pousser une fenêtre de dix
// cellules demande dix Ctrl+A -- signalé à l'usage, pas par un test.
// ---------------------------------------------------------------------------

TEST(leader_keeps_the_chord_alive_after_a_repeatable_gesture) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult first = d.feed(arrow(Key::Left, 0));
  REQUIRE(first.action.has_value());
  CHECK(*first.action == Action::MoveLeft);
  CHECK(d.repeating());
  CHECK(!d.armed());

  // La flèche suivante agit SANS qu'on reprenne le leader, et la série
  // continue : c'est l'écart entre deux gestes qui est borné, pas leur
  // nombre.
  for (int i = 0; i < 5; ++i) {
    const LeaderResult again = d.feed(arrow(Key::Left, 0));
    REQUIRE(again.action.has_value());
    CHECK(*again.action == Action::MoveLeft);
    CHECK(again.consumed);
    CHECK(d.repeating());
  }
}

// La règle qui rend la fenêtre de répétition inoffensive : en série, ce qui
// ne s'enchaîne pas n'est PAS capté -- ni exécuté, ni avalé. Sans elle, un
// « w » tapé dans un document une seconde après un déplacement fermerait la
// fenêtre.
TEST(leader_hands_back_anything_that_does_not_chain) {
  const char32_t dangerous[] = {U'w', U'z', U'f', U'-', U'd', U'x', U'é'};
  for (const char32_t c : dangerous) {
    LeaderDispatch d;
    d.feed(ctrl(U'a'));
    d.feed(arrow(Key::Down, 0));
    REQUIRE(d.repeating());

    const LeaderResult r = d.feed(plain(c));
    CHECK(!r.consumed);          // l'application la reçoit
    CHECK(!r.action.has_value());  // et le bureau ne fait rien
    CHECK(!d.repeating());
    CHECK(!d.armed());
  }
}

// Un geste qui ne s'enchaîne pas ferme l'accord sur-le-champ, comme avant.
TEST(leader_closes_the_chord_after_a_gesture_that_does_not_repeat) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  const LeaderResult r = d.feed(plain(U'z'));
  REQUIRE(r.action.has_value());
  CHECK(*r.action == Action::MaximizeToggle);
  CHECK(!d.repeating());
  CHECK(!d.armed());
}

// En série, le leader rouvre un accord franc : on veut visiblement autre
// chose qu'un déplacement de plus. Il ne s'émet donc PAS littéralement.
TEST(leader_reopens_a_fresh_chord_from_inside_a_series) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  d.feed(arrow(Key::Up, 0));
  REQUIRE(d.repeating());

  const LeaderResult r = d.feed(ctrl(U'a'));
  CHECK(r.consumed);
  CHECK(!r.action.has_value());
  CHECK(d.armed());

  const LeaderResult then = d.feed(plain(U'w'));
  REQUIRE(then.action.has_value());
  CHECK(*then.action == Action::Close);
}

// La session peut clore la série quand son délai est écoulé. Le dispatcheur
// n'a pas d'horloge, et c'est voulu : une table qui saurait l'heure serait
// intestable.
TEST(leader_lets_the_session_close_the_series) {
  LeaderDispatch d;
  d.feed(ctrl(U'a'));
  d.feed(arrow(Key::Right, 0));
  REQUIRE(d.repeating());

  d.reset();
  CHECK(!d.repeating());
  const LeaderResult r = d.feed(arrow(Key::Right, 0));
  CHECK(!r.consumed);  // la flèche appartient de nouveau à l'application
  CHECK(!r.action.has_value());
}

// Exactement les gestes qu'on répète, et rien d'autre. « fermer » enchaîné
// serait une machine à détruire des fenêtres ; une bascule enchaînée ne
// ferait qu'osciller.
TEST(only_the_gestures_one_actually_repeats_are_repeatable) {
  const Action yes[] = {Action::MoveLeft,    Action::MoveRight,
                        Action::MoveUp,      Action::MoveDown,
                        Action::GrowWidth,   Action::ShrinkWidth,
                        Action::GrowHeight,  Action::ShrinkHeight,
                        Action::NextWindow,  Action::PrevWindow};
  const Action no[] = {Action::Close,      Action::Minimize,
                       Action::MaximizeToggle, Action::FullscreenToggle,
                       Action::OpenMenu,   Action::Detach,
                       Action::ToggleMouse, Action::ForceRepaint,
                       Action::ShowHelp,   Action::LiteralLeader};
  for (const Action a : yes) CHECK(sshos::is_repeatable(a));
  for (const Action a : no) CHECK(!sshos::is_repeatable(a));
}
