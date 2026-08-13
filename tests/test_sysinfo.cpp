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
  s.draw(sshos::View(surf, sshos::Rect{0, 0, w, h}), sshos::Theme::mono16());
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
  const size_t cpu = g.find("PROCESSEUR");
  const size_t mem = g.find("MEMOIRE");
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
  CHECK(g.find("PROCESSEUR") != std::string::npos);
  CHECK(g.find("MEMOIRE") != std::string::npos);
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
