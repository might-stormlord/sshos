#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "vt/parser.hpp"

using sshos::Params;
using sshos::Parser;
using sshos::ParserSink;
using sshos::VtState;

namespace {

// Un mouchard qui écrit tout ce qu'on lui dit dans une seule chaîne. Une
// transcription se compare d'un coup d'œil et s'imprime en entier quand
// elle diffère -- ce qu'une pile d'objets ne fait pas.
//
//   p:41      un caractère imprimable, en hexa
//   x:0a      un octet de commande
//   c:1;2?h   un CSI : paramètres, puis intermédiaires, puis final
//   e:(0      un ESC : intermédiaires, puis final
//   o:2;titre un OSC
//   d:q  D:x  /d    ouverture, données, fermeture d'un DCS
struct Recorder : ParserSink {
  std::string log;

  // Deux chiffres au minimum, pas de zéros en tête au-delà : « 0a » pour un
  // saut de ligne, « fffd » pour le caractère de remplacement.
  static std::string hex(uint32_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    while (v != 0) {
      out.insert(out.begin(), kDigits[v & 0xF]);
      v >>= 4;
    }
    while (out.size() < 2) out.insert(out.begin(), '0');
    return out;
  }

  static std::string show(const Params& p) {
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
      if (i > 0) out.push_back(p[i].sub ? ':' : ';');
      if (p[i].value >= 0) out += std::to_string(p[i].value);
    }
    return out;
  }

  void print(char32_t c) override { log += "p:" + hex(c) + " "; }
  void execute(uint8_t b) override { log += "x:" + hex(b) + " "; }
  void csi(const Params& p, std::string_view inter, uint8_t final_byte) override {
    log += "c:" + show(p) + std::string(inter) + static_cast<char>(final_byte) + " ";
  }
  void esc(std::string_view inter, uint8_t final_byte) override {
    log += "e:" + std::string(inter) + static_cast<char>(final_byte) + " ";
  }
  void osc(std::string_view data) override { log += "o:" + std::string(data) + " "; }
  void dcs_start(const Params& p, std::string_view inter, uint8_t f) override {
    log += "d:" + show(p) + std::string(inter) + static_cast<char>(f) + " ";
  }
  void dcs_data(std::string_view chunk) override {
    log += "D:" + std::string(chunk) + " ";
  }
  void dcs_end() override { log += "/d "; }
};

std::string run(std::string_view input) {
  Recorder r;
  Parser p(r);
  p.feed(input);
  return r.log;
}

// Le même flux, mais un octet par appel à feed().
std::string run_byte_by_byte(std::string_view input) {
  Recorder r;
  Parser p(r);
  for (const char c : input) p.feed(std::string_view(&c, 1));
  return r.log;
}

struct Case {
  const char* input;
  const char* expected;
};

// La table de référence. Chaque ligne est vérifiée DEUX fois : en un bloc,
// puis octet par octet.
const Case kCases[] = {
    // Le socle
    {"A", "p:41 "},
    {"\n", "x:0a "},
    {"\r\n", "x:0d x:0a "},
    {"a\tb", "p:61 x:09 p:62 "},

    // CSI, avec et sans paramètres
    {"\033[H", "c:H "},
    {"\033[2J", "c:2J "},
    {"\033[1;5H", "c:1;5H "},
    {"\033[;5H", "c:;5H "},      // premier paramètre ABSENT, pas zéro
    {"\033[m", "c:m "},
    {"\033[0m", "c:0m "},
    {"\033[38;5;196m", "c:38;5;196m "},
    {"\033[38:2::255:0:0m", "c:38:2::255:0:0m "},  // forme ISO 8613-6

    // Marqueur privé : il voyage avec les intermédiaires
    {"\033[?25h", "c:25?h "},
    {"\033[?1049l", "c:1049?l "},
    {"\033[?1000;1006h", "c:1000;1006?h "},

    // ESC simples et à intermédiaire
    {"\0337", "e:7 "},
    {"\033M", "e:M "},
    {"\033(0", "e:(0 "},
    {"\033)B", "e:)B "},

    // OSC, terminé par BEL ou par ST
    {"\033]0;titre\007", "o:0;titre "},
    {"\033]2;autre\033\\", "o:2;autre e:\\ "},

    // Ce qui doit être avalé sans dommage
    {"\033[>c", "c:>c "},
    {"\033[1$p", "c:1$p "},

    // Un CSI mal formé part à la poubelle, et ce qui suit vit sa vie
    {"\033[1;2\x1f""3m", "x:1f c:1;23m "},

    // Mélange : du texte autour d'une séquence
    {"ab\033[31mcd", "p:61 p:62 c:31m p:63 p:64 "},

    // Un deux-points EN TÊTE : le premier séparateur est alors lu par
    // CsiEntry et non par CsiParam, une branche que « 38:2:… » n'atteint
    // jamais puisque les chiffres l'ont déjà quittée.
    {"\033[:5m", "c::5m "},
    {"\033[;5m", "c:;5m "},

    // Au-delà du plan 16. Atteignable : 0xF4 mène jusqu'à 0x13FFFF, bien
    // au-dessus du 0x10FFFF où s'arrête Unicode.
    {"\xf4\x90\x80\x80", "p:fffd "},

    // Surlong à trois octets DANS la plage des deux octets. Le cas nul
    // (« \xe0\x80\x80 ») ne prouve rien : il tombe sous tous les
    // minimums qu'on pourrait écrire par erreur.
    {"\xe0\x82\xac", "p:fffd "},

    // Trop d'intermédiaires : la séquence est JETÉE, pas tronquée --
    // tronquée, « \033[!!!!!!p » deviendrait « \033[!!!!p », qui existe.
    {"\033[!!!!!!p""A", "p:41 "},
    {"\033[!!p""A", "c:!!p p:41 "},
};

}  // namespace

TEST(vt_parser_reads_the_reference_table) {
  for (const Case& c : kCases) {
    const std::string got = run(c.input);
    if (got != c.expected) {
      th::fail(__FILE__, __LINE__,
               std::string("pour « ") + c.input + " » attendu « " + c.expected +
                   " » obtenu « " + got + " »");
    }
  }
}

// L'ÉTAT SURVIT ENTRE LES APPELS. Le parseur est nourri de morceaux
// arbitraires venant de read() : une séquence coupée en deux doit produire
// exactement les mêmes appels qu'en un bloc. C'est le test de première
// classe de cette tâche, pas un raffinement.
TEST(vt_parser_gives_the_same_reading_byte_by_byte) {
  for (const Case& c : kCases) {
    const std::string whole = run(c.input);
    const std::string split = run_byte_by_byte(c.input);
    if (whole != split) {
      th::fail(__FILE__, __LINE__,
               std::string("pour « ") + c.input + " » en bloc « " + whole +
                   " » mais octet par octet « " + split + " »");
    }
  }
}

// Un caractère accentué coupé par une frontière de read() se recolle. Sans
// ça, chaque « é » tombant au mauvais endroit d'un tampon deviendrait deux
// caractères de remplacement.
TEST(vt_parser_glues_a_utf8_character_split_across_reads) {
  Recorder r;
  Parser p(r);
  p.feed("\xc3");  // premier octet de « é »
  CHECK_EQ(r.log, std::string());
  p.feed("\xa9");
  CHECK_EQ(r.log, std::string("p:e9 "));

  // Et un emoji, coupé en trois.
  r.log.clear();
  p.feed("\xf0\x9f");
  p.feed("\x92");
  p.feed("\xa9");
  CHECK_EQ(r.log, std::string("p:1f4a9 "));
}

TEST(vt_parser_decodes_the_widths_of_utf8) {
  CHECK_EQ(run("\xc3\xa9"), std::string("p:e9 "));            // 2 octets
  CHECK_EQ(run("\xe6\x97\xa5"), std::string("p:65e5 "));      // 3 octets
  CHECK_EQ(run("\xf0\x9f\x92\xa9"), std::string("p:1f4a9 ")); // 4 octets
}

// Trois façons d'encoder ce qui n'existe pas -- surlong, substitut UTF-16,
// au-delà du plan 16 -- et trois vecteurs d'évasion connus si on les laisse
// passer. Chacune rend un caractère de remplacement, et l'état survit.
TEST(vt_parser_refuses_the_three_ways_of_encoding_nothing) {
  CHECK_EQ(run("\xc0\xaf"), std::string("p:fffd p:fffd "));       // surlong
  CHECK_EQ(run("\xed\xa0\x80"), std::string("p:fffd "));          // substitut
  CHECK_EQ(run("\xf5\x80\x80\x80"),
           std::string("p:fffd p:fffd p:fffd p:fffd "));          // hors plan
  CHECK_EQ(run("\xe0\x80\x80"), std::string("p:fffd "));          // surlong à 3
}

// Un octet corrompu ne doit pas emporter le caractère valide qui le suit.
TEST(vt_parser_keeps_reading_after_a_corrupt_byte) {
  CHECK_EQ(run("\xff""A"), std::string("p:fffd p:41 "));
  CHECK_EQ(run("\xc3""A"), std::string("p:fffd p:41 "));  // séquence tronquée
  CHECK_EQ(run("\x80""A"), std::string("p:fffd p:41 "));  // continuation seule
}

// Une séquence UTF-8 tronquée par un ESC : le caractère de remplacement
// sort AVANT que la séquence d'échappement commence, sinon il apparaîtrait
// après le texte qu'elle colore.
TEST(vt_parser_flushes_a_truncated_character_before_an_escape) {
  CHECK_EQ(run("\xc3\033[m"), std::string("p:fffd c:m "));
}

// La machine revient TOUJOURS en Ground après un final. Une machine coincée
// mange tout ce qui suit, et le symptôme -- un écran figé à mi-course -- ne
// dit pas d'où il vient.
TEST(vt_parser_always_comes_back_to_ground) {
  const char* inputs[] = {"\033[0m",     "\033[?25h", "\033(0",
                          "\033]0;x\007", "\033[1$p",  "\033[<0;1;1M",
                          "A",           "\033[\x18", "\033P q\033\\"};
  for (const char* in : inputs) {
    Recorder r;
    Parser p(r);
    p.feed(in);
    if (p.state() != VtState::Ground) {
      th::fail(__FILE__, __LINE__,
               std::string("coince apres « ") + in + " »");
    }
  }
}

// CAN et SUB sont la seule porte de sortie d'un état bloqué, et un tmux
// imbriqué s'en sert pour se resynchroniser.
TEST(vt_parser_lets_can_and_sub_cancel_anything) {
  CHECK_EQ(run("\033[12;\x18""A"), std::string("x:18 p:41 "));
  CHECK_EQ(run("\033]0;titre\x1a""A"), std::string("x:1a p:41 "));
  CHECK_EQ(run("\033P q\x18""A"), std::string("d: q /d x:18 p:41 "));
}

// Un ESC au milieu d'un CSI n'est jamais un paramètre : il recommence.
TEST(vt_parser_restarts_on_an_escape_inside_a_sequence) {
  CHECK_EQ(run("\033[12;\033[3m"), std::string("c:3m "));
}

// Sans plafond, `\033[999999999999m` déborde un int -- comportement
// indéfini, donc UBSan en Debug et n'importe quoi en Release.
TEST(vt_parser_clamps_an_absurd_parameter) {
  CHECK_EQ(run("\033[999999999999m"), std::string("c:65535m "));
  CHECK_EQ(run("\033[65535;99999999m"), std::string("c:65535;65535m "));
}

// Une séquence forgée avec cent paramètres ne fait pas grandir un vecteur
// sans borne : ce serait une fuite de mémoire à la demande de l'invité.
TEST(vt_parser_caps_the_number_of_parameters) {
  std::string many = "\033[";
  for (int i = 0; i < 200; ++i) many += "1;";
  many += "m";
  const std::string got = run(many);
  size_t count = 0;
  for (const char c : got) {
    if (c == ';') ++count;
  }
  CHECK(count < 64);
  CHECK(got.back() == ' ');
  CHECK(got.find("m") != std::string::npos);
}

// Un marqueur privé APRÈS des paramètres est illégal : la séquence entière
// part à la poubelle plutôt que d'être devinée.
TEST(vt_parser_throws_away_a_sequence_it_cannot_read) {
  CHECK_EQ(run("\033[1?2m""A"), std::string("p:41 "));
  // Le marqueur suivi DIRECTEMENT du final : sans ce cas, une machine qui
  // accepterait le marqueur tardif passerait quand même, parce que le
  // chiffre d'après la ferait basculer dans l'état qui avale.
  CHECK_EQ(run("\033[1?m""A"), std::string("p:41 "));
  CHECK_EQ(run("\033[1>m""A"), std::string("p:41 "));
  CHECK_EQ(run("\033[\x7f""1m"), std::string("c:1m "));  // DEL ignoré
}

// Un OSC démesuré est tronqué, pas jeté : perdre le titre à cause d'un
// octet de trop serait pire que l'afficher coupé.
TEST(vt_parser_truncates_a_giant_osc_without_losing_it) {
  std::string big = "\033]0;";
  big.append(9000, 'x');
  big += "\007";
  const std::string got = run(big);
  CHECK(got.rfind("o:0;", 0) == 0);
  CHECK(got.size() < 6000);
  CHECK(got.size() > 1000);
}

// Un DCS non terminé -- ce qu'un tmux imbriqué produit à chaque requête de
// capacité -- ne doit pas se mettre à manger l'écran caractère par
// caractère.
TEST(vt_parser_swallows_a_dcs_without_printing_it) {
  CHECK_EQ(run("\033P1$r0m\033\\""A"), std::string("d:1$r D:0 D:m /d e:\\ p:41 "));
  // Non terminé : rien n'est imprimé, et le texte qui suit reste avalé.
  const std::string got = run("\033P1$r du texte qui ne doit pas paraitre");
  CHECK(got.find("p:") == std::string::npos);
}

// APC, PM et SOS s'avalent en entier. `\033_G…\033\\` est le protocole
// graphique de kitty : un invité qui le tente ne doit pas repeindre notre
// bureau avec.
TEST(vt_parser_swallows_apc_pm_and_sos) {
  CHECK_EQ(run("\033_Gf=100,a=T;charge\033\\""A"), std::string("e:\\ p:41 "));
  CHECK_EQ(run("\033^titre\033\\""A"), std::string("e:\\ p:41 "));
  CHECK_EQ(run("\033Xtexte\033\\""A"), std::string("e:\\ p:41 "));
}

// reset() ramène tout à zéro : le liant s'en sert quand l'invité meurt et
// qu'une nouvelle commande démarre dans la même fenêtre.
TEST(vt_parser_can_be_reset_mid_sequence) {
  Recorder r;
  Parser p(r);
  p.feed("\033[12;");
  CHECK(p.state() != VtState::Ground);
  p.reset();
  CHECK(p.state() == VtState::Ground);
  p.feed("A");
  CHECK_EQ(r.log, std::string("p:41 "));
}

// Le paramètre absent n'est pas zéro : param_or rend le défaut demandé.
TEST(vt_parser_tells_an_absent_parameter_from_a_zero) {
  Params p = {{-1, false}, {0, false}, {7, false}};
  CHECK_EQ(sshos::param_or(p, 0, 1), 1);   // absent -> défaut
  CHECK_EQ(sshos::param_or(p, 1, 1), 0);   // zéro explicite
  CHECK_EQ(sshos::param_or(p, 2, 1), 7);
  CHECK_EQ(sshos::param_or(p, 9, 1), 1);   // hors liste -> défaut
}

// ---------------------------------------------------------------------------
// Propriété, sur des flux engendrés. La table de référence dit ce que le
// parseur doit lire ; ceci dit ce qu'il ne doit JAMAIS faire, sur des
// entrées que personne n'a écrites à la main.
// ---------------------------------------------------------------------------

namespace {

// Générateur déterministe et sans dépendance. La graine est imprimée en cas
// d'échec, ce qui suffit à rejouer exactement le flux fautif.
struct Seeded {
  uint64_t s;
  explicit Seeded(uint64_t seed) : s(seed ? seed : 1) {}
  uint32_t next() {  // xorshift64*, largement assez pour un fuzz de forme
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return static_cast<uint32_t>((s * 0x2545F4914F6CDD1DULL) >> 32);
  }
  uint32_t below(uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// Mêle du valide, du tronqué et de l'absurde -- les trois formes qu'un
// invité produit vraiment : la première par usage normal, la deuxième par
// frontière de read(), la troisième par bogue ou par malveillance.
std::string brew(Seeded& rng, size_t rounds) {
  static const char* kValid[] = {
      "\033[H",    "\033[2J",   "\033[1;5H",  "\033[0m",   "\033[38;5;9m",
      "\033[?25h", "\033[?25l", "\033[?1049h", "\033[?1049l", "\0337",
      "\0338",     "\033M",     "\033(0",     "\033(B",    "\033]0;t\007",
      "\033P1$r\033\\",         "\r\n",       "\t",        "\b",
      "abc",       "\xc3\xa9",  "\xe6\x97\xa5", "\xf0\x9f\x92\xa9",
  };
  static const char* kBroken[] = {
      "\033[",     "\033[1;",   "\033]0;sans fin", "\033P",  "\033",
      "\xc3",      "\xe6\x97",  "\xf0\x9f",       "\033[?",  "\033_",
  };
  std::string out;
  for (size_t i = 0; i < rounds; ++i) {
    switch (rng.below(10)) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        out += kValid[rng.below(sizeof kValid / sizeof *kValid)];
        break;
      case 6:
      case 7:
        out += kBroken[rng.below(sizeof kBroken / sizeof *kBroken)];
        break;
      case 8:
        out += "\033[" + std::to_string(rng.next()) + "m";  // paramètre absurde
        break;
      default:
        out.push_back(static_cast<char>(rng.below(256)));  // octet pur hasard
        break;
    }
  }
  return out;
}

// Le même flux, coupé à des endroits que le hasard choisit -- ce que fait
// read() sur un tuyau chargé.
std::string run_in_random_chunks(std::string_view input, Seeded& rng) {
  Recorder r;
  Parser p(r);
  size_t at = 0;
  while (at < input.size()) {
    const size_t take = 1 + rng.below(static_cast<uint32_t>(input.size() - at));
    p.feed(input.substr(at, take));
    at += take;
  }
  return r.log;
}

}  // namespace

// LA propriété du parseur : le découpage du flux ne change RIEN. C'est ce
// que dit la spec du jalon -- « une séquence coupée en deux doit
// fonctionner » -- et c'est ce qu'aucune relecture ne garantit.
TEST(vt_parser_reads_the_same_thing_however_the_stream_is_cut) {
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    Seeded gen(seed);
    const std::string stream = brew(gen, 40);

    const std::string whole = run(stream);
    Seeded cutter(seed * 7919 + 13);
    const std::string chunked = run_in_random_chunks(stream, cutter);
    if (whole != chunked) {
      th::fail(__FILE__, __LINE__,
               "graine " + std::to_string(seed) +
                   " : en bloc « " + whole + " » mais en morceaux « " +
                   chunked + " »");
    }
  }
}

// Un flux quelconque ne doit jamais laisser la machine dans un état d'où
// elle ne sort plus : un CAN suffit toujours à la ramener au sol. Sans ça,
// un seul octet malheureux gèlerait la fenêtre pour de bon.
TEST(vt_parser_can_always_be_brought_back_to_ground) {
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    Seeded gen(seed);
    Recorder r;
    Parser p(r);
    p.feed(brew(gen, 40));
    p.feed("\x18");  // CAN
    if (p.state() != VtState::Ground) {
      th::fail(__FILE__, __LINE__,
               "graine " + std::to_string(seed) + " : coincee malgre un CAN");
    }
  }
}

// Rien de ce qui est engendré ne doit faire imprimer un point de code qui
// n'existe pas : ni substitut UTF-16, ni au-delà du plan 16. Un caractère
// hors bornes traverserait tout le rendu jusqu'à la table de largeurs.
TEST(vt_parser_never_prints_a_codepoint_that_cannot_exist) {
  struct Guard : ParserSink {
    bool bad = false;
    char32_t worst = 0;
    void print(char32_t c) override {
      if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        bad = true;
        worst = c;
      }
    }
  };
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    Seeded gen(seed);
    Guard g;
    Parser p(g);
    p.feed(brew(gen, 40));
    if (g.bad) {
      th::fail(__FILE__, __LINE__,
               "graine " + std::to_string(seed) + " : point de code " +
                   std::to_string(static_cast<uint32_t>(g.worst)));
    }
  }
}

// DEL est ignoré au sol, comme dans xterm. L'imprimer poserait une cellule
// parasite là où l'invité n'a rien voulu écrire -- et les vieux terminaux
// en émettent en remplissage.
TEST(vt_parser_ignores_del_at_ground_level) {
  CHECK_EQ(run("a\x7f""b"), std::string("p:61 p:62 "));
  CHECK_EQ(run("\x7f"), std::string());
}
