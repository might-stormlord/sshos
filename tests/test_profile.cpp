#include <string>

#include "harness.hpp"
#include "render/profile.hpp"

using sshos::Color;
using sshos::ColorDepth;
using sshos::OutputProfile;
using sshos::sgr_transition;
using sshos::Style;

TEST(profile_detects_truecolor) {
  const auto p = OutputProfile::detect("xterm-256color", "truecolor", true);
  CHECK(p.depth == ColorDepth::TrueColor);
  CHECK(p.utf8);
}

TEST(profile_detects_256_and_16) {
  CHECK(OutputProfile::detect("xterm-256color", "", true).depth ==
        ColorDepth::Indexed256);
  CHECK(OutputProfile::detect("xterm", "", true).depth == ColorDepth::Mono16);
  CHECK(OutputProfile::detect("vt100", "", false).depth == ColorDepth::Mono16);
}

TEST(sgr_emits_nothing_when_style_is_unchanged) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  CHECK_EQ(sgr_transition(Style{}, Style{}, p), std::string(""));
}

TEST(sgr_truecolor_foreground) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style to;
  to.fg = Color::rgb(255, 0, 0);
  CHECK_EQ(sgr_transition(Style{}, to, p), std::string("\033[38;2;255;0;0m"));
}

TEST(sgr_quantizes_to_256_and_16) {
  Style to;
  to.fg = Color::rgb(255, 0, 0);
  const auto p256 = OutputProfile::detect("xterm-256color", "", true);
  CHECK_EQ(sgr_transition(Style{}, to, p256), std::string("\033[38;5;196m"));
  const auto p16 = OutputProfile::detect("xterm", "", true);
  CHECK_EQ(sgr_transition(Style{}, to, p16), std::string("\033[31m"));
}

TEST(sgr_adds_attribute_incrementally) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style to;
  to.attrs = sshos::attr::Bold;
  CHECK_EQ(sgr_transition(Style{}, to, p), std::string("\033[1m"));
}

// Retirer un attribut n'a pas de code incrémental fiable : on réinitialise
// puis on repose l'état complet.
TEST(sgr_resets_when_an_attribute_is_removed) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.attrs = sshos::attr::Bold | sshos::attr::Underline;
  from.fg = Color::rgb(1, 2, 3);
  Style to;
  to.attrs = sshos::attr::Bold;
  CHECK_EQ(sgr_transition(from, to, p), std::string("\033[0m\033[1m"));
}

TEST(sgr_returns_to_default_color_with_39_and_49) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.fg = Color::rgb(255, 0, 0);
  from.bg = Color::indexed(4);
  CHECK_EQ(sgr_transition(from, Style{}, p), std::string("\033[39m\033[49m"));
}

TEST(encode_utf8_roundtrip) {
  CHECK_EQ(sshos::encode_utf8(U'a'), std::string("a"));
  CHECK_EQ(sshos::encode_utf8(U'日'), std::string("\xe6\x97\xa5"));
  CHECK_EQ(sshos::encode_utf8(U'\U0001f600'), std::string("\xf0\x9f\x98\x80"));
}

// Valider que les surrogates sont substitués par U+FFFD.
TEST(encode_utf8_rejects_surrogates) {
  // Première surrogate (D800)
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0xD800)), std::string("\xef\xbf\xbd"));
  // Dernière surrogate (DFFF)
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0xDFFF)), std::string("\xef\xbf\xbd"));
}

// Valider que les valeurs > U+10FFFF sont substituées.
TEST(encode_utf8_rejects_out_of_range) {
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0x110000)), std::string("\xef\xbf\xbd"));
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0x200000)), std::string("\xef\xbf\xbd"));
}

// Valider les limites valides du scalaire Unicode.
TEST(encode_utf8_accepts_scalar_boundaries) {
  // U+D7FF : avant la première surrogate
  CHECK_EQ(sshos::encode_utf8(U'퟿'), std::string("\xed\x9f\xbf"));
  // U+E000 : après la dernière surrogate
  CHECK_EQ(sshos::encode_utf8(U''), std::string("\xee\x80\x80"));
  // U+10FFFF : maximum valide
  CHECK_EQ(sshos::encode_utf8(U'\U0010FFFF'), std::string("\xf4\x8f\xbf\xbf"));
}

// Vérifier que la couleur est re-émise après une réinitialisation d'attributs.
// Réinitialiser supprime aussi la couleur : elle doit être rétablie.
TEST(sgr_replays_color_after_attribute_reset) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.attrs = sshos::attr::Bold;
  from.fg = Color::rgb(10, 20, 30);
  Style to;
  to.fg = Color::rgb(10, 20, 30);
  // La couleur est inchangée mais l'attribut est retiré, donc réinitialisation.
  // Après \033[0m, la couleur par défaut est active : la reposer.
  CHECK_EQ(sgr_transition(from, to, p), std::string("\033[0m\033[38;2;10;20;30m"));
}

// Vérifier qu'une modification du fond seul n'émet que le code du fond.
// Pas de code de premier plan si le premier plan ne change pas.
TEST(sgr_changes_background_only) {
  const auto p = OutputProfile::detect("xterm", "truecolor", true);
  Style from;
  from.fg = Color::rgb(100, 100, 100);
  Style to;
  to.fg = Color::rgb(100, 100, 100);
  to.bg = Color::rgb(1, 1, 1);
  // Seul le fond change : un seul code de fond.
  CHECK_EQ(sgr_transition(from, to, p), std::string("\033[48;2;1;1;1m"));
}
