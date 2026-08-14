#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <string>
#include <vector>

#include "apps/terminal.hpp"
#include "harness.hpp"
#include "vt/parser.hpp"

using sshos::Size;
using sshos::Terminal;

namespace {

// Générateur DÉTERMINISTE. Pas de `rand()`, pas d'horloge : un échec doit
// se rejouer à l'identique depuis sa seule graine, et une suite qui
// change de flux à chaque exécution ne prouve rien -- elle constate.
// xorshift64*, une ligne, et une période largement suffisante.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed != 0 ? seed : 1) {}

  uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 2685821657736338717ULL;
  }

  int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }

 private:
  uint64_t s_;
};

// Un flux qui MÊLE les quatre familles. Un fuzzer qui n'émettrait que du
// hasard n'atteindrait jamais l'intérieur d'une séquence : c'est le
// mélange de valide, de tronqué et d'absurde qui exerce les transitions.
std::string generate(Rng& rng, int bytes) {
  static const char* kValid[] = {
      "\033[H",        "\033[2J",      "\033[1;31m",   "\033[0m",
      "\033[5;10H",    "\033[K",       "\033[3P",      "\033[2L",
      "\033[?1049h",   "\033[?1049l",  "\033[?25l",    "\033[?7l",
      "\033[6n",       "\033[c",       "\033[?25$p",   "\0337",
      "\0338",         "\033D",        "\033M",        "\033(0",
      "\033(B",        "\033[38;2;10;20;30m",          "\033[38:5:196m",
      "\033]2;titre\033\\",            "\033[1;5A",    "\033[?1002;1006h",
      "\r\n",          "\t",           "\b",           "abc",
      "une ligne de texte ordinaire",  "\303\251t\303\251",
      "\344\270\200",  "\360\237\230\200",
  };
  // Tronqués : des séquences qui commencent et ne finissent pas. C'est ce
  // qu'un `tmux` imbriqué produit à chaque requête de capacité.
  static const char* kTruncated[] = {
      "\033[",  "\033[38;5", "\033[?",  "\033]2;sans fin",
      "\033",   "\033[1;",   "\033P",   "\033[38;2;10;20",
      "\303",   "\344\270",  "\360\237\230",
  };
  // Absurdes : des nombres qui débordent, des piles d'intermédiaires, des
  // sous-paramètres à n'en plus finir.
  static const char* kAbsurd[] = {
      "\033[999999999999999999999m",
      "\033[99999999999;99999999999H",
      "\033[!!!!!!!!!!!!!!!!p",
      "\033[::::::::::::::m",
      "\033[;;;;;;;;;;;;;;;m",
      "\033[-1;-1H",
      "\033[38;5;99999m",
      "\033[?99999999h",
  };

  std::string out;
  out.reserve(static_cast<size_t>(bytes) + 64);
  while (static_cast<int>(out.size()) < bytes) {
    switch (rng.below(10)) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
        out += kValid[rng.below(static_cast<int>(std::size(kValid)))];
        break;
      case 5:
      case 6:
        out += kTruncated[rng.below(static_cast<int>(std::size(kTruncated)))];
        break;
      case 7:
        out += kAbsurd[rng.below(static_cast<int>(std::size(kAbsurd)))];
        break;
      default: {
        // Des octets au hasard, y compris de l'UTF-8 invalide et des C0.
        const int n = 1 + rng.below(8);
        for (int i = 0; i < n; ++i) {
          out.push_back(static_cast<char>(rng.below(256)));
        }
        break;
      }
    }
  }
  return out;
}

// La transcription d'un écran, curseur compris. C'est l'observable qu'on
// compare entre deux découpages du même flux.
std::string transcript(const Terminal& t) {
  const auto& s = t.screen_for_tests();
  std::string out;
  for (int y = 0; y < s.rows(); ++y) {
    out += s.line_text(y);
    out.push_back('/');
  }
  out += "@" + std::to_string(s.cursor().x) + "," + std::to_string(s.cursor().y);
  return out;
}

constexpr int kCols = 40;
constexpr int kRows = 12;
constexpr int kRounds = 120;
constexpr int kBytesPerRound = 1500;

}  // namespace

// LES INVARIANTS, à chaque tour. Aucun n'est une opinion : le curseur hors
// grille est un écran corrompu, un parseur bloqué hors de `Ground` mange
// tout ce qui suit caractère par caractère, et une écriture hors grille
// est un débordement que le build sanitize transforme en échec franc.
TEST(fuzz_keeps_the_parser_and_the_grid_sane) {
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t seed = 0x5150ULL + static_cast<uint64_t>(round);
    Rng rng(seed);
    Terminal t;
    // La fenêtre fait une ligne de plus que la grille : la barre
    // d'onglets du Terminal la prend.
    t.on_resize(Size{kCols, kRows + 1});
    t.feed_for_tests(generate(rng, kBytesPerRound));

    const auto& s = t.screen_for_tests();
    // La graine est dans le message : un échec se rejoue sans deviner.
    const bool inside = s.cursor().x >= 0 && s.cursor().x < s.cols() &&
                        s.cursor().y >= 0 && s.cursor().y < s.rows();
    CHECK(inside);
    CHECK_EQ(s.cols(), kCols);
    CHECK_EQ(s.rows(), kRows);
    // La graine est imprimée PAR L'ÉCHEC : un tour qui tombe se rejoue
    // sans avoir à deviner lequel c'était.
    if (!inside) {
      CHECK_EQ("graine " + std::to_string(seed), std::string("curseur dedans"));
    }
  }
}

// LA propriété d'un parseur nourri par `read()` : le DÉCOUPAGE ne change
// rien. Le même flux livré d'un bloc et octet par octet doit produire
// exactement le même écran -- c'est la seule chose qui garantisse qu'une
// séquence coupée en deux par une frontière de paquet se recolle.
TEST(fuzz_gives_the_same_screen_whatever_the_chunking) {
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t seed = 0xC0FFEEULL + static_cast<uint64_t>(round);
    Rng rng(seed);
    const std::string stream = generate(rng, kBytesPerRound);

    Terminal whole;
    whole.on_resize(Size{kCols, kRows + 1});
    whole.feed_for_tests(stream);

    Terminal byte_by_byte;
    byte_by_byte.on_resize(Size{kCols, kRows + 1});
    for (char c : stream) byte_by_byte.feed_for_tests(std::string_view(&c, 1));

    CHECK_EQ(transcript(byte_by_byte), transcript(whole));
  }
}

// Et avec des coupures AU HASARD, pas seulement aux deux extrêmes : c'est
// ce que produit vraiment un `read()` sur un tuyau.
TEST(fuzz_gives_the_same_screen_under_random_chunking) {
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t seed = 0xBADC0DEULL + static_cast<uint64_t>(round);
    Rng rng(seed);
    const std::string stream = generate(rng, kBytesPerRound);

    Terminal whole;
    whole.on_resize(Size{kCols, kRows + 1});
    whole.feed_for_tests(stream);

    Terminal chopped;
    chopped.on_resize(Size{kCols, kRows + 1});
    size_t at = 0;
    while (at < stream.size()) {
      const size_t n = std::min<size_t>(stream.size() - at,
                                        static_cast<size_t>(1 + rng.below(37)));
      chopped.feed_for_tests(std::string_view(stream.data() + at, n));
      at += n;
    }

    CHECK_EQ(transcript(chopped), transcript(whole));
  }
}

// LE PARSEUR SE RESYNCHRONISE TOUJOURS. Quel que soit l'état où le flux
// l'a laissé -- au milieu d'un CSI, dans un OSC sans fin, dans un DCS,
// dans un UTF-8 tronqué -- un terminateur de chaîne le ramène au repos.
//
// La propriété a d'abord été écrite « un octet final ramène en Ground »,
// et le fuzzer l'a réfutée en trois tours : un flux qui finit DANS un OSC
// y reste légitimement, puisqu'un OSC ne se termine que par ST ou BEL.
// C'était le test qui avait tort, pas le parseur -- et c'est exactement ce
// qu'on attend d'un fuzzer.
//
// Ce qui compte vraiment est ceci : rien ne peut le bloquer pour de bon.
// Sans cette garantie, une séquence mal formée mangerait tout ce qui suit,
// et c'est le symptôme « mon terminal n'affiche plus rien ».
TEST(fuzz_can_always_be_resynchronised) {
  struct Mute : sshos::ParserSink {};
  for (int round = 0; round < kRounds; ++round) {
    Rng rng(0x9E3779B9ULL + static_cast<uint64_t>(round));
    Mute sink;
    sshos::Parser parser(sink);
    parser.feed(generate(rng, kBytesPerRound));
    parser.feed("\033\\");
    CHECK(parser.state() == sshos::VtState::Ground);
  }
}
