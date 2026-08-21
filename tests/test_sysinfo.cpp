#include <string>
#include <vector>

#include "harness.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/sysinfo.hpp"

using sshos::ProcInfo;
using sshos::SysInfo;

namespace {

const char* kStat1 =
    "cpu  100 0 100 800 0 0 0 0 0 0\n"
    "cpu0 50 0 50 400 0 0 0 0 0 0\n"
    "cpu1 50 0 50 400 0 0 0 0 0 0\n";
const char* kStat2 =
    "cpu  200 0 200 900 0 0 0 0 0 0\n"
    "cpu0 150 0 150 400 0 0 0 0 0 0\n"
    "cpu1 50 0 50 500 0 0 0 0 0 0\n";
const char* kMem = "MemTotal: 1000000 kB\nMemAvailable: 250000 kB\n";
const char* kLoad = "2.50 1.00 0.50 1/100 200\n";
const char* kNet1 = "  eth0: 1000 1 0 0 0 0 0 0 2000 2 0 0 0 0 0 0\n";
const char* kNet2 = "  eth0: 3048 1 0 0 0 0 0 0 6096 2 0 0 0 0 0 0\n";

std::string painted(const SysInfo& s, int w, int h) {
  sshos::Surface surf(w, h);
  s.draw(sshos::View(surf, sshos::Rect{0, 0, w, h}), sshos::Theme::mono16(),
         sshos::Border::Unicode);
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = surf.text_row(y);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

}  // namespace

// Les QUATRE sections, dans l'ordre : ce que la machine subit d'abord, ce
// qui l'explique ensuite.
TEST(sysinfo_shows_its_four_sections) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {{7, "gourmand", 0, 100}});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {{7, "gourmand", 200, 100}});

  const std::string g = painted(s, 40, 24);
  const size_t cpu = g.find("CPU");
  const size_t mem = g.find("MEM");
  const size_t net = g.find("RESEAU");
  const size_t proc = g.find("PROCESSUS");
  CHECK(cpu != std::string::npos);
  CHECK(mem != std::string::npos);
  CHECK(net != std::string::npos);
  CHECK(proc != std::string::npos);
  CHECK(cpu < mem);
  CHECK(mem < net);
  CHECK(net < proc);
}

TEST(sysinfo_shows_no_percentage_before_it_has_two_samples) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  for (int p : s.cores_for_tests()) CHECK_EQ(p, 0);
  CHECK_EQ(s.rx_rate_for_tests(), uint64_t{0});
}

// LE DÉBIT SE RAPPORTE AU TEMPS RÉEL ÉCOULÉ, pas au tick nominal : deux
// échantillons séparés de deux secondes -- ce qui arrive dès que le bureau
// n'est pas dessiné -- donneraient sinon un débit double.
TEST(sysinfo_computes_the_rate_over_the_time_that_really_passed) {
  SysInfo a;
  a.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  a.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {});
  CHECK_EQ(a.rx_rate_for_tests(), uint64_t{2048});

  SysInfo b;
  b.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  b.sample_for_tests(3000, kStat2, kMem, kLoad, kNet2, {});
  CHECK_EQ(b.rx_rate_for_tests(), uint64_t{1024});
}

// Un compteur qui RECULE -- interface redémarrée -- ne donne pas un débit
// de quatre milliards.
TEST(sysinfo_reports_no_rate_when_a_counter_went_backwards) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet2, {});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet1, {});
  CHECK_EQ(s.rx_rate_for_tests(), uint64_t{0});
}

// Les processus sont TRIÉS par CPU : c'est la question qu'on se pose en
// regardant le fond d'écran.
TEST(sysinfo_sorts_the_processes_by_cpu) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1,
                     {{7, "calme", 0, 100}, {9, "gourmand", 0, 100}});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2,
                     {{7, "calme", 5, 100}, {9, "gourmand", 150, 100}});

  REQUIRE_EQ(s.rows_for_tests().size(), size_t{2});
  CHECK_EQ(s.rows_for_tests()[0].name, std::string("gourmand"));
}

// LA TRONCATURE EST DÉLIBÉRÉE : quand la place manque, ce sont les
// processus qui sautent. Les trois premières sections répondent à « la
// machine souffre-t-elle ? », et c'est la question qui vient d'abord.
TEST(sysinfo_drops_the_processes_first_when_it_lacks_room) {
  SysInfo s;
  std::vector<ProcInfo> a;
  std::vector<ProcInfo> b;
  for (int i = 0; i < 30; ++i) {
    a.push_back({i + 100, "proc" + std::to_string(i), 0, 100});
    b.push_back({i + 100, "proc" + std::to_string(i), 50, 100});
  }
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, a);
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, b);

  const std::string g = painted(s, 40, 12);
  CHECK(g.find("CPU") != std::string::npos);
  CHECK(g.find("MEM") != std::string::npos);
  CHECK(g.find("RESEAU") != std::string::npos);
  CHECK(g.find("proc29") == std::string::npos);
}

// Trop étroit, il se tait : un widget qui écrit à moitié dans une colonne
// est pire qu'un fond vide.
TEST(sysinfo_says_nothing_in_a_space_too_narrow) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {});
  CHECK_EQ(painted(s, 10, 20), std::string(std::string(19, '/')));
}

// Le débit s'écrit en unités lisibles : les octets bruts sont illisibles
// au-delà du millier, et c'est justement là qu'ils deviennent
// intéressants.
TEST(sysinfo_writes_the_rate_in_readable_units) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {});
  CHECK(painted(s, 40, 24).find("2Ko/s") != std::string::npos);
}

// ------------------------------------------- les compteurs encadrés

// Chaque compteur porte son propre cadre TITRÉ, et le titre est incrusté
// dans le trait du haut : une ligne de titre séparée coûterait une ligne
// sur quatre dans une boîte qui en fait quatre.
TEST(sysinfo_frames_each_counter_with_its_title) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {});

  const std::string g = painted(s, 40, 20);
  CHECK(g.find("┌ CPU ") != std::string::npos);
  CHECK(g.find("┌ MEM ") != std::string::npos);
  CHECK(g.find("┌ RESEAU ") != std::string::npos);
  CHECK(g.find("┌ CHARGE ") != std::string::npos);
  CHECK(g.find("┌ PROCESSUS ") != std::string::npos);
}

// Un client sans UTF-8 reçoit des cadres ASCII, comme partout ailleurs
// dans le bureau : des points d'interrogation à la place des traits
// seraient pires que pas de cadre du tout.
TEST(sysinfo_falls_back_to_ascii_frames) {
  SysInfo s;
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, {});
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, {});

  sshos::Surface surf(40, 20);
  s.draw(sshos::View(surf, sshos::Rect{0, 0, 40, 20}), sshos::Theme::mono16(),
         sshos::Border::Ascii);
  std::string g;
  for (int y = 0; y < 20; ++y) g += surf.text_row(y);
  CHECK(g.find("+ CPU ") != std::string::npos);
  CHECK(g.find("┌") == std::string::npos);
}

// CINQ processus au plus. Au-delà, la liste cesse de répondre à « à cause
// de qui ? » pour devenir un mur de texte -- c'est exactement ce qui
// rendait le fond illisible.
TEST(sysinfo_never_lists_more_than_five_processes) {
  SysInfo s;
  std::vector<ProcInfo> a;
  std::vector<ProcInfo> b;
  for (int i = 0; i < 20; ++i) {
    a.push_back({i + 100, "proc" + std::to_string(i), 0, 100});
    b.push_back({i + 100, "proc" + std::to_string(i), 50, 100});
  }
  s.sample_for_tests(1000, kStat1, kMem, kLoad, kNet1, a);
  s.sample_for_tests(2000, kStat2, kMem, kLoad, kNet2, b);

  const std::string g = painted(s, 40, 40);
  int seen = 0;
  for (int i = 0; i < 20; ++i) {
    if (g.find("proc" + std::to_string(i) + " ") != std::string::npos ||
        g.find("proc" + std::to_string(i) + "|") != std::string::npos) {
      ++seen;
    }
  }
  CHECK(seen <= 5);
}

// LA SIGNATURE : présente, mais dans une teinte proche du fond. Une
// signature qui se lit aussi bien que le contenu détourne l'œil de ce
// qu'on est venu faire.
TEST(sysinfo_draws_its_signature_in_block_letters) {
  sshos::Surface surf(40, 12);
  SysInfo::draw_banner(sshos::View(surf, sshos::Rect{0, 0, 40, 12}),
                       sshos::Theme::mono16(), sshos::Border::Unicode);
  std::string g;
  for (int y = 0; y < 12; ++y) g += surf.text_row(y);
  CHECK(g.find("█") != std::string::npos);
  CHECK(!(surf.at(0, 0).fg == sshos::Theme::mono16().desktop_bg));
}

// CHAQUE LETTRE DU MOT DOIT PEINDRE QUELQUE CHOSE, et ce cas existe parce
// que l'oubli est SILENCIEUX. `glyph_rows()` finit par `default: return
// kSpace[row]` : une lettre sans dessin ne casse rien, ne produit aucun
// avertissement sous -Werror, et sort simplement blanche. Le cas voisin
// (`..._draws_its_signature_in_block_letters`) ne cherche qu'un seul bloc
// dans toute la vue -- deux lettres definies sur six lui suffisent, et il
// resterait vert avec quatre trous au milieu du mot.
//
// On verifie donc bande par bande : chaque lettre occupe kGlyphW colonnes a
// x0 + i * (kGlyphW + kGap), et chacune doit porter au moins un bloc. Le mot
// n'a pas d'espace, sinon il faudrait exempter sa bande.
TEST(sysinfo_paints_every_single_letter_of_its_signature) {
  constexpr int kW = 40, kH = 12;
  constexpr int kGlyphW = 3, kGap = 1, kRows = 5;
  constexpr int kLetters = 6;  // « TERMOS »
  const int word_w = kLetters * (kGlyphW + kGap) - kGap;

  sshos::Surface surf(kW, kH);
  SysInfo::draw_banner(sshos::View(surf, sshos::Rect{0, 0, kW, kH}),
                       sshos::Theme::mono16(), sshos::Border::Unicode);

  const int x0 = (kW - word_w) / 2;
  const int y0 = (kH - kRows) / 2;
  for (int i = 0; i < kLetters; ++i) {
    int blocs = 0;
    for (int row = 0; row < kRows; ++row) {
      for (int c = 0; c < kGlyphW; ++c) {
        if (surf.at(x0 + i * (kGlyphW + kGap) + c, y0 + row).ch == U'\u2588') ++blocs;
      }
    }
    // CHECK plutot que REQUIRE : on veut savoir COMBIEN de lettres manquent,
    // pas seulement qu'il en manque une. REQUIRE sortirait a la premiere.
    CHECK(blocs > 0);
  }
}

// Trop à l'étroit, elle ne s'écrit pas : une signature tronquée est un
// défaut d'affichage, pas une décoration.
TEST(sysinfo_skips_its_signature_when_the_space_is_too_small) {
  sshos::Surface surf(10, 3);
  SysInfo::draw_banner(sshos::View(surf, sshos::Rect{0, 0, 10, 3}),
                       sshos::Theme::mono16(), sshos::Border::Unicode);
  for (int y = 0; y < 3; ++y) {
    CHECK_EQ(surf.text_row(y).find("█"), std::string::npos);
  }
}
