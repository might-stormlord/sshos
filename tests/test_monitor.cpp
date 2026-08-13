#include <string>
#include <vector>

#include "apps/monitor/monitor.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Key;
using sshos::KeyEvent;
using sshos::Monitor;
using sshos::ProcInfo;

namespace {

// Une machine décrite de toutes pièces : deux cœurs, de la mémoire, une
// charge. C'est tout l'intérêt d'analyseurs qui prennent le TEXTE.
const char* kStat1 =
    "cpu  100 0 100 800 0 0 0 0 0 0\n"
    "cpu0 50 0 50 400 0 0 0 0 0 0\n"
    "cpu1 50 0 50 400 0 0 0 0 0 0\n";
const char* kStat2 =
    "cpu  200 0 200 900 0 0 0 0 0 0\n"
    "cpu0 150 0 150 400 0 0 0 0 0 0\n"
    "cpu1 50 0 50 500 0 0 0 0 0 0\n";
const char* kMem =
    "MemTotal:       1000000 kB\n"
    "MemAvailable:    250000 kB\n";
const char* kLoad = "2.50 1.00 0.50 1/100 200\n";

std::vector<ProcInfo> procs1() {
  return {{10, "gourmand", 100, 500}, {20, "gros", 10, 5000}};
}
std::vector<ProcInfo> procs2() {
  // « gourmand » a consommé 200 ticks de plus, « gros » seulement 5.
  return {{10, "gourmand", 300, 500}, {20, "gros", 15, 5000}};
}

std::string painted(Monitor& m, int w, int h) {
  m.freeze_for_tests();
  sshos::Surface s(w, h);
  m.render(sshos::View(s, sshos::Rect{0, 0, w, h}));
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = s.text_row(y);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

}  // namespace

// ------------------------------------------------------ le premier échantillon

// LE PREMIER ÉCHANTILLON NE PEUT RIEN DIRE. `/proc/stat` donne des
// compteurs cumulés depuis le démarrage : afficher un pourcentage dès la
// première lecture donnerait la charge MOYENNE depuis l'allumage, présentée
// comme la charge instantanée.
TEST(monitor_shows_no_percentage_before_it_has_two_samples) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());

  CHECK(!m.has_delta_for_tests());
  for (int p : m.cores_for_tests()) CHECK_EQ(p, 0);
}

TEST(monitor_computes_the_percentages_on_the_second_sample) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  REQUIRE(m.has_delta_for_tests());
  const std::vector<int>& c = m.cores_for_tests();
  REQUIRE_EQ(c.size(), size_t{3});
  // cpu0 : 200 occupés de plus sur 200 écoulés -> 100 %.
  CHECK_EQ(c[1], 100);
  // cpu1 : 0 occupé de plus sur 100 écoulés -> 0 %.
  CHECK_EQ(c[2], 0);
}

// -------------------------------------------------------- une fois par seconde

// Le rafraîchissement est BORNÉ À UNE FOIS PAR SECONDE. Sans ce plafond,
// chaque trame relirait `/proc` -- trente fois par seconde, et le moniteur
// deviendrait la charge qu'il mesure.
TEST(monitor_samples_at_most_once_per_second) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(1500, kStat2, kMem, kLoad, procs2());  // trop tôt

  CHECK_EQ(m.last_sample_for_tests(), int64_t{1000});
  CHECK(!m.has_delta_for_tests());
}

TEST(monitor_samples_again_once_the_second_has_passed) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  CHECK_EQ(m.last_sample_for_tests(), int64_t{2000});
  CHECK(m.has_delta_for_tests());
}

// ------------------------------------------------------------------ le tri

// Par CPU au départ : c'est la question qu'on se pose en ouvrant un
// moniteur.
TEST(monitor_sorts_by_cpu_first) {
  Monitor m;
  CHECK(m.sort_for_tests() == Monitor::Sort::Cpu);

  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  REQUIRE_EQ(m.rows_for_tests().size(), size_t{2});
  CHECK_EQ(m.rows_for_tests()[0].name, std::string("gourmand"));
}

TEST(monitor_switches_to_memory_and_back) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  m.on_key(KeyEvent{Key::Char, U'm', 0});
  CHECK(m.sort_for_tests() == Monitor::Sort::Memory);
  REQUIRE_EQ(m.rows_for_tests().size(), size_t{2});
  CHECK_EQ(m.rows_for_tests()[0].name, std::string("gros"));

  m.on_key(KeyEvent{Key::Char, U'c', 0});
  CHECK(m.sort_for_tests() == Monitor::Sort::Cpu);
  CHECK_EQ(m.rows_for_tests()[0].name, std::string("gourmand"));
}

// Un processus APPARU entre deux échantillons n'a pas de passé : lui
// prêter le temps CPU d'un autre le placerait en tête à tort.
TEST(monitor_gives_a_new_process_no_cpu_share) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  std::vector<ProcInfo> plus = procs2();
  plus.push_back({30, "nouveau", 9999, 100});
  m.sample_for_tests(2000, kStat2, kMem, kLoad, plus);

  for (const auto& r : m.rows_for_tests()) {
    if (r.name == "nouveau") CHECK_EQ(r.cpu_percent, 0);
  }
}

// ---------------------------------------------------------------- le rendu

TEST(monitor_paints_the_load_and_the_memory) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  const std::string g = painted(m, 50, 12);
  CHECK(g.find("2.50") != std::string::npos);
  CHECK(g.find("Mem") != std::string::npos);
}

TEST(monitor_paints_one_bar_per_core) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  const std::string g = painted(m, 50, 12);
  CHECK(g.find("0[") != std::string::npos);
  CHECK(g.find("1[") != std::string::npos);
}

TEST(monitor_paints_the_processes_it_sorted) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  const std::string g = painted(m, 50, 12);
  CHECK(g.find("gourmand") != std::string::npos);
  CHECK(g.find("gros") != std::string::npos);
}

// Une fenêtre trop courte ne doit pas peindre hors d'elle -- la `View`
// clippe, mais la boucle doit s'arrêter d'elle-même plutôt que de
// parcourir mille processus pour rien.
TEST(monitor_paints_nothing_below_a_short_window) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  const std::string g = painted(m, 50, 5);
  CHECK(g.find("gros") == std::string::npos);
}
