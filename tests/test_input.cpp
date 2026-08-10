#include <chrono>
#include <cstdio>
#include <optional>
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

// Défaut corrigé : l'ancienne version faisait CHECK_EQ(v.size(), 1) puis
// return v.at(0) sans condition — un résultat vide (ce que produisent
// justement plusieurs des cas ci-dessous avant leur correctif) levait
// std::out_of_range au lieu d'échouer proprement. Même précédent que
// roundtrip() dans tests/test_proto.cpp : on rend l'optional lui-même,
// chaque appelant le garde (REQUIRE) puis revient tôt s'il est vide avant
// tout déréférencement. Une absence d'évènement est un résultat ordinaire
// que le type doit pouvoir porter, pas une exception à attraper.
static std::optional<KeyEvent> one_key(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  if (v.size() != 1) return std::nullopt;
  return std::get<KeyEvent>(v.at(0));
}

static std::optional<MouseEvent> one_mouse(std::string_view bytes) {
  InputParser p;
  const auto v = drain(p, bytes);
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  if (v.size() != 1) return std::nullopt;
  return std::get<MouseEvent>(v.at(0));
}

TEST(input_plain_characters) {
  const auto k = one_key("a");
  REQUIRE(k.has_value());
  CHECK(k->key == Key::Char);
  CHECK_EQ(k->ch, U'a');
  CHECK_EQ(static_cast<int>(k->mods), 0);
}

TEST(input_control_characters) {
  const auto ctrl_a = one_key("\001");
  REQUIRE(ctrl_a.has_value());
  CHECK(ctrl_a->key == Key::Char);
  CHECK_EQ(ctrl_a->ch, U'a');
  CHECK_EQ(static_cast<int>(ctrl_a->mods), static_cast<int>(mod::Ctrl));

  const auto enter = one_key("\r");
  REQUIRE(enter.has_value());
  CHECK(enter->key == Key::Enter);

  const auto tab = one_key("\t");
  REQUIRE(tab.has_value());
  CHECK(tab->key == Key::Tab);

  const auto bs = one_key("\177");
  REQUIRE(bs.has_value());
  CHECK(bs->key == Key::Backspace);
}

TEST(input_utf8_characters_wait_for_all_their_bytes) {
  InputParser p;
  CHECK(drain(p, "\xc3").empty());          // moitié de é
  const auto v = drain(p, "\xa9");
  CHECK_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'é');
}

TEST(input_arrows_and_function_keys) {
  auto up = one_key("\033[A");
  REQUIRE(up.has_value());
  CHECK(up->key == Key::Up);

  auto left = one_key("\033[D");
  REQUIRE(left.has_value());
  CHECK(left->key == Key::Left);

  auto home = one_key("\033[H");
  REQUIRE(home.has_value());
  CHECK(home->key == Key::Home);

  auto del = one_key("\033[3~");
  REQUIRE(del.has_value());
  CHECK(del->key == Key::Delete);

  auto pgup = one_key("\033[5~");
  REQUIRE(pgup.has_value());
  CHECK(pgup->key == Key::PgUp);

  auto f1 = one_key("\033OP");
  REQUIRE(f1.has_value());
  CHECK(f1->key == Key::F1);

  auto f5 = one_key("\033[15~");
  REQUIRE(f5.has_value());
  CHECK(f5->key == Key::F5);

  auto backtab = one_key("\033[Z");
  REQUIRE(backtab.has_value());
  CHECK(backtab->key == Key::BackTab);
}

TEST(input_modified_arrows) {
  const auto k = one_key("\033[1;5A");  // Ctrl+Haut
  REQUIRE(k.has_value());
  CHECK(k->key == Key::Up);
  CHECK_EQ(static_cast<int>(k->mods), static_cast<int>(mod::Ctrl));

  const auto s = one_key("\033[1;2C");  // Shift+Droite
  REQUIRE(s.has_value());
  CHECK(s->key == Key::Right);
  CHECK_EQ(static_cast<int>(s->mods), static_cast<int>(mod::Shift));
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
  REQUIRE(k.has_value());
  CHECK(k->key == Key::Char);
  CHECK_EQ(k->ch, U'a');
  CHECK_EQ(static_cast<int>(k->mods), static_cast<int>(mod::Alt));
}

TEST(input_sgr_mouse_press_and_release) {
  const auto press = one_mouse("\033[<0;10;5M");
  REQUIRE(press.has_value());
  CHECK(press->action == MouseAction::Press);
  CHECK_EQ(static_cast<int>(press->button), 0);
  CHECK_EQ(press->x, 9);   // 0-indexé
  CHECK_EQ(press->y, 4);

  const auto rel = one_mouse("\033[<0;10;5m");
  REQUIRE(rel.has_value());
  CHECK(rel->action == MouseAction::Release);
}

// Bit 32 = mouvement. Le mode 1002 le signale à chaque cellule franchie.
TEST(input_sgr_mouse_motion) {
  const auto m = one_mouse("\033[<32;10;5M");
  REQUIRE(m.has_value());
  CHECK(m->action == MouseAction::Motion);
  CHECK_EQ(static_cast<int>(m->button), 0);
}

// Bit 64 = molette. Une molette n'émet JAMAIS de relâchement : la traiter
// comme un bouton verrouille la machine à états en glissement perpétuel.
TEST(input_wheel_is_not_a_button) {
  const auto up = one_mouse("\033[<64;1;1M");
  REQUIRE(up.has_value());
  CHECK(up->action == MouseAction::WheelUp);

  const auto down = one_mouse("\033[<65;1;1M");
  REQUIRE(down.has_value());
  CHECK(down->action == MouseAction::WheelDown);
}

TEST(input_mouse_modifier_bits) {
  const auto m = one_mouse("\033[<16;1;1M");  // +16 = Ctrl
  REQUIRE(m.has_value());
  CHECK_EQ(static_cast<int>(m->mods), static_cast<int>(mod::Ctrl));

  const auto s = one_mouse("\033[<4;1;1M");   // +4 = Shift
  REQUIRE(s.has_value());
  CHECK_EQ(static_cast<int>(s->mods), static_cast<int>(mod::Shift));
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

// ---------------------------------------------------------------------
// C1 — accumulateur de paramètre CSI non borné (débordement signé, UB).
// ---------------------------------------------------------------------

// Contre parser.cpp non corrigé, __ubsan_default_options() (tests/main.cpp)
// fait avorter TOUT le binaire de test (SIGABRT) dès que split_params
// déborde — ce test n'échoue pas proprement dans ce cas, il tue le
// processus avant d'atteindre une seule assertion. Voir le rapport pour ce
// qui a été observé en l'exécutant isolément contre le code non corrigé.
TEST(input_absurd_csi_param_does_not_overflow_or_invent_modifiers) {
  InputParser p;
  const std::string wire = "\033[1;" + std::string(20, '9') + "A";
  const auto v = drain(p, wire);
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  const auto& k = std::get<KeyEvent>(v.at(0));
  // La séquence reste un Haut reconnu : un paramètre absurde ne doit pas
  // faire disparaître une touche par ailleurs bien formée.
  CHECK(k.key == Key::Up);
  // Choix délibéré (cf. finding C1, mods_from_param) : un paramètre hors de
  // la plage légitime [1,16] du protocole xterm ne dit rien de fiable sur
  // les modificateurs pressés. Saturer l'accumulateur puis agir quand même
  // dessus par modulo aurait juste rendu, sans crash, une valeur fausse et
  // confiante (la revue observait mods=6 = Alt+Ctrl pour une frappe qui n'a
  // jamais eu Alt) ; 0 dit honnêtement « aucun modificateur fiable ».
  CHECK_EQ(static_cast<int>(k.mods), 0);
}

// ---------------------------------------------------------------------
// I1 — décodeur UTF-8 non validé (désormais partagé avec render/, voir
// common/utf8.*). Cas énumérés par la revue.
// ---------------------------------------------------------------------

TEST(input_utf8_rejects_overlong_nul) {
  // C0 80 encode U+0000 en 2 octets : jamais un NUL silencieux.
  const auto k = one_key("\xc0\x80");
  REQUIRE(k.has_value());
  CHECK_EQ(k->ch, U'�');
}

TEST(input_utf8_rejects_overlong_slash_cve_2000_0884) {
  // C0 AF encode '/' en 2 octets : CVE-2000-0884, traversée de chemin par
  // encodage sur-long. Ne doit jamais redevenir '/'.
  const auto k = one_key("\xc0\xaf");
  REQUIRE(k.has_value());
  CHECK_EQ(k->ch, U'�');
}

TEST(input_utf8_rejects_encoded_surrogate) {
  // ED A0 80 encode le surrogate U+D800 : jamais un scalaire valide.
  const auto k = one_key("\xed\xa0\x80");
  REQUIRE(k.has_value());
  CHECK_EQ(k->ch, U'�');
}

TEST(input_utf8_rejects_lone_continuation_byte) {
  // 0x80 seul est un octet de continuation, jamais un lead byte : ne doit
  // pas redevenir U+0080.
  const auto k = one_key("\x80");
  REQUIRE(k.has_value());
  CHECK_EQ(k->ch, U'�');
}

TEST(input_utf8_never_swallows_a_valid_byte_after_bad_continuation) {
  // C3 suivi de 'A' (continuation invalide, 'A' < 0x80) : 'A' est un octet
  // valide à lui seul, il ne doit jamais être digéré dans un faux scalaire
  // combiné avec le lead byte précédent.
  InputParser p;
  const auto v = drain(p, "\xc3\x41");
  REQUIRE_EQ(v.size(), static_cast<size_t>(2));
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'�');
  CHECK(std::get<KeyEvent>(v.at(1)).key == Key::Char);
  CHECK_EQ(std::get<KeyEvent>(v.at(1)).ch, U'A');
}

// ---------------------------------------------------------------------
// I2 — timeout() ne résolvait que l'état ESC seul.
// ---------------------------------------------------------------------

TEST(input_timeout_flushes_pending_csi_prefix) {
  InputParser p;
  p.feed("\033[");
  CHECK(!p.next().has_value());  // encore incomplet, rien à lire
  p.timeout();

  std::vector<InputEvent> flushed;
  while (auto e = p.next()) flushed.push_back(*e);
  REQUIRE_EQ(flushed.size(), static_cast<size_t>(2));
  CHECK(std::get<KeyEvent>(flushed.at(0)).key == Key::Escape);
  const auto& bracket = std::get<KeyEvent>(flushed.at(1));
  CHECK(bracket.key == Key::Char);
  CHECK_EQ(bracket.ch, U'[');

  // Et surtout : une frappe ultérieure sans rapport avec la séquence
  // avortée ne doit plus jamais disparaître (cf. finding I2 : "\033[" puis
  // 'x' séparément faisait perdre 'x' sans le moindre évènement).
  const auto v = drain(p, "x");
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  CHECK(std::get<KeyEvent>(v.at(0)).key == Key::Char);
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'x');
}

TEST(input_timeout_flushes_pending_ss3_prefix) {
  InputParser p;
  p.feed("\033O");
  p.timeout();
  std::vector<InputEvent> flushed;
  while (auto e = p.next()) flushed.push_back(*e);
  REQUIRE_EQ(flushed.size(), static_cast<size_t>(2));
  CHECK(std::get<KeyEvent>(flushed.at(0)).key == Key::Escape);
  const auto& o = std::get<KeyEvent>(flushed.at(1));
  CHECK(o.key == Key::Char);
  CHECK_EQ(o.ch, U'O');
}

// ---------------------------------------------------------------------
// I3 — paramètres SGR manquants deviennent -1 et forgent un évènement.
// ---------------------------------------------------------------------

TEST(input_sgr_mouse_missing_params_yield_no_event) {
  InputParser p;
  // Un seul paramètre au lieu de trois : cb=-1 avant ce correctif, soit
  // tous les bits à 1 — Shift|Alt|Ctrl, molette, WheelDown, tout à la fois
  // pour une souris qui n'a jamais dit ça.
  const auto v = drain(p, "\033[<;10;5M");
  CHECK(v.empty());
}

TEST(input_sgr_mouse_zero_coordinate_yields_no_event) {
  InputParser p;
  // Coordonnée câble à 0 : hors du protocole SGR (1-indexé sur le fil) —
  // -1 après conversion violerait le contrat 0-indexé de MouseEvent
  // (events.hpp:32) tout autant qu'un paramètre manquant.
  const auto v = drain(p, "\033[<0;0;5M");
  CHECK(v.empty());
}

// ---------------------------------------------------------------------
// I4 — buf_.find(kPasteEnd) rescannait tout le tampon à chaque step().
// ---------------------------------------------------------------------

// Seuil volontairement large : mesuré dans cet environnement (Debug,
// ASan+UBSan — le pire cas, nettement plus lent qu'en Release) à ~810 ms
// avec le correctif contre ~2814 ms sans, pour 300 Ko livrés octet par
// octet. Le seuil ci-dessous reste loin des deux bornes pour éviter qu'un
// test de performance devienne friable sous ASan/UBSan, tout en
// discriminant clairement un retour au O(n²).
TEST(input_paste_byte_by_byte_is_not_quadratic) {
  InputParser p;
  p.feed("\033[200~");
  const std::string payload(300 * 1024, 'x');  // 300 Ko, comme la mesure de revue

  const auto t0 = std::chrono::steady_clock::now();
  for (char c : payload) p.feed(std::string_view(&c, 1));
  p.feed("\033[201~");
  const auto t1 = std::chrono::steady_clock::now();

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::fprintf(stderr, "  [info] collage 300 Ko octet par octet : %.1f ms\n", ms);
  CHECK(ms < 1800.0);

  std::vector<InputEvent> v;
  while (auto e = p.next()) v.push_back(*e);
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text.size(), payload.size());
}

// Ce test ne discrimine pas contre le code non corrigé (un rescan complet
// depuis 0 trouve tout aussi bien un terminateur à cheval sur deux feed() —
// le défaut d'I4 est une question de performance, pas de correction). Gardé
// comme filet de sécurité pour l'optimisation elle-même : reculer de
// kPasteEnd.size()-1 avant de reprendre le scan doit couvrir tout
// chevauchement, pas juste le cas simple testé plus haut.
TEST(input_paste_terminator_straddling_feed_boundary_is_found) {
  InputParser p;
  p.feed("\033[200~");
  p.feed("hello\033[20");  // le terminateur commence, coupé net par le feed()
  CHECK(!p.next().has_value());
  p.feed("1~");  // le complète sur l'appel suivant : ne doit pas être raté

  std::vector<InputEvent> v;
  while (auto e = p.next()) v.push_back(*e);
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  CHECK_EQ(std::get<PasteEvent>(v.at(0)).text, std::string("hello"));
}

// ---------------------------------------------------------------------
// C2 — croissance non bornée du tampon (collage et scan CSI).
// ---------------------------------------------------------------------

TEST(input_garbage_csi_is_bounded_and_resyncs) {
  InputParser p;
  // Bien plus long que toute séquence CSI légitime, et sans octet final :
  // rien qu'un humain n'a tapé. Juste au-dessus du plafond, pour que le
  // test dise précisément où il agit.
  const std::string garbage = "\033[" + std::string(129, '0');
  p.feed(garbage);
  p.feed("\001");  // Ctrl-A : jamais un octet final CSI, sert de sonde

  std::vector<InputEvent> v;
  while (auto e = p.next()) v.push_back(*e);
  // Sans plafond, tout reste bloqué en attente d'un octet final qui ne
  // viendra jamais : v resterait vide indéfiniment. Avec un plafond, la
  // lecture abandonne les 130 premiers octets (ESC, '[', 128 '0') et
  // resynchronise sur le dernier '0' puis la sonde.
  REQUIRE_EQ(v.size(), static_cast<size_t>(2));
  CHECK(std::get<KeyEvent>(v.at(0)).key == Key::Char);
  CHECK_EQ(std::get<KeyEvent>(v.at(0)).ch, U'0');
  const auto& probe = std::get<KeyEvent>(v.at(1));
  CHECK(probe.key == Key::Char);
  CHECK_EQ(probe.ch, U'a');
  CHECK_EQ(static_cast<int>(probe.mods), static_cast<int>(mod::Ctrl));
}

TEST(input_paste_over_cap_does_not_grow_without_bound) {
  InputParser p;
  p.feed("\033[200~");
  const std::string chunk(1024 * 1024, 'A');  // 1 Mio, au plafond
  for (int i = 0; i < 3; ++i) p.feed(chunk);   // 3 Mio, toujours sans terminateur

  std::vector<InputEvent> v;
  while (auto e = p.next()) v.push_back(*e);
  // Sans plafond, rien n'est jamais livré tant que le terminateur n'arrive
  // pas : v resterait vide après 3 Mio retenus en mémoire. Avec un
  // plafond, des fragments sont déjà sortis.
  REQUIRE(!v.empty());

  size_t total = 0;
  for (const auto& e : v) total += std::get<PasteEvent>(e).text.size();

  p.feed("\033[201~");
  while (auto e = p.next()) {
    total += std::get<PasteEvent>(*e).text.size();
  }
  // Fragmenté ou non, aucun octet du collage n'a disparu.
  CHECK_EQ(total, chunk.size() * 3);
}

TEST(input_paste_fragments_flag_completion_correctly) {
  InputParser p;
  p.feed("\033[200~");
  const std::string chunk(1024 * 1024, 'A');
  for (int i = 0; i < 3; ++i) p.feed(chunk);

  std::vector<InputEvent> v;
  while (auto e = p.next()) v.push_back(*e);
  REQUIRE(!v.empty());
  for (const auto& e : v) {
    CHECK(!std::get<PasteEvent>(e).complete);
  }

  p.feed("\033[201~");
  const auto tail = p.next();
  REQUIRE(tail.has_value());
  CHECK(std::get<PasteEvent>(*tail).complete);
}

// ---------------------------------------------------------------------
// M1 — octet final CSI ou code tilde non reconnu : None se confondait
// entre "pas une touche" et "touche non implémentée".
// ---------------------------------------------------------------------

TEST(input_unrecognized_csi_final_is_unknown_not_silently_dropped) {
  InputParser p;
  const auto v = drain(p, "\033[E");  // 'E' n'est mappé dans aucune table
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  CHECK(std::get<KeyEvent>(v.at(0)).key == Key::Unknown);
}

TEST(input_unrecognized_tilde_code_is_unknown_not_silently_dropped) {
  InputParser p;
  const auto v = drain(p, "\033[99~");  // code tilde absent de la table
  REQUIRE_EQ(v.size(), static_cast<size_t>(1));
  CHECK(std::get<KeyEvent>(v.at(0)).key == Key::Unknown);
}
