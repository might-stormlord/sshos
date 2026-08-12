#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "vt/sink.hpp"

namespace sshos {

// Les états de la machine DEC, dans la forme de Paul Williams. Publics
// parce qu'un test doit pouvoir vérifier qu'on revient bien en Ground : une
// machine qui reste coincée en CsiIgnore mange tout ce qui suit, et le
// symptôme -- un écran qui se fige à mi-course -- ne dit pas d'où il vient.
enum class VtState {
  Ground,
  Escape,
  EscapeIntermediate,
  CsiEntry,
  CsiParam,
  CsiIntermediate,
  CsiIgnore,
  DcsEntry,
  DcsParam,
  DcsIntermediate,
  DcsPassthrough,
  DcsIgnore,
  OscString,
  SosPmApcString,
};

// Machine à états pure. Elle reçoit des octets et appelle un ParserSink ;
// elle ne connaît ni grille, ni couleur, ni curseur.
//
// L'ÉTAT SURVIT ENTRE LES APPELS. Le parseur est nourri de morceaux
// arbitraires venant de read(), et une séquence coupée en deux doit
// fonctionner : c'est un test de première classe, pas un détail.
//
// Pas de C1 sur un seul octet (0x80-0x9F). Notre monde est en UTF-8 --
// TERM=xterm-256color, et c'est nous qui posons la variable -- où ces
// octets sont des continuations de séquence. Les reconnaître comme des
// commandes couperait en deux tout caractère accentué.
class Parser {
 public:
  explicit Parser(ParserSink& sink) : sink_(&sink) {}

  void feed(std::string_view bytes);
  void feed(const char* data, size_t n) { feed(std::string_view(data, n)); }

  void reset();

  VtState state() const { return state_; }

 private:
  void step(uint8_t b);
  void ground_byte(uint8_t b);
  void utf8_byte(uint8_t b);
  void flush_utf8_as_replacement();

  void clear_sequence();
  bool collect(uint8_t b);
  void push_param_digit(uint8_t b);
  void next_param(bool sub);

  ParserSink* sink_;
  VtState state_ = VtState::Ground;

  Params params_;
  std::string intermediates_;
  std::string osc_;

  // Le décodeur UTF-8, qui vit DANS l'état Ground. Un caractère coupé par
  // une frontière de read() se recolle ici.
  uint32_t utf8_acc_ = 0;
  int utf8_need_ = 0;
  int utf8_seen_ = 0;
  uint32_t utf8_min_ = 0;
};

}  // namespace sshos
