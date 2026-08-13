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

// ------------------------- douze trous montrés par les mutations

// Le PREMIER échantillon passe toujours, quelle que soit l'heure qu'il
// est. Le refuser parce qu'« il est trop tôt » interdirait le second,
// donc tout delta, donc tout pourcentage.
TEST(monitor_always_takes_its_first_sample) {
  Monitor m;
  m.sample_for_tests(100, kStat1, kMem, kLoad, procs1());
  CHECK_EQ(m.last_sample_for_tests(), int64_t{100});
}

// Un seul échantillon ne donne JAMAIS de delta -- même quand il n'y a
// aucun cœur à comparer, cas où les deux listes sont vides et se
// ressemblent trompeusement.
TEST(monitor_has_no_delta_after_a_single_empty_sample) {
  Monitor m;
  m.sample_for_tests(1000, "", kMem, kLoad, {});
  CHECK(!m.has_delta_for_tests());
}

// Le nombre de cœurs peut CHANGER (machine virtuelle qui en gagne un).
// Comparer deux listes de tailles différentes lirait hors du vecteur.
TEST(monitor_skips_the_delta_when_the_core_count_changed) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  const char* three =
      "cpu  200 0 200 900 0 0 0 0 0 0\n"
      "cpu0 100 0 100 400 0 0 0 0 0 0\n"
      "cpu1 50 0 50 500 0 0 0 0 0 0\n"
      "cpu2 50 0 50 500 0 0 0 0 0 0\n";
  m.sample_for_tests(2000, three, kMem, kLoad, procs2());

  CHECK(!m.has_delta_for_tests());
  for (int p : m.cores_for_tests()) CHECK_EQ(p, 0);
}

// Le processus se retrouve par son PID, jamais par son nom : deux
// processus peuvent porter le même, et leur prêter mutuellement leur temps
// donnerait des pourcentages inventés.
TEST(monitor_matches_a_process_by_its_pid_not_its_name) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad,
                     {{10, "meme", 0, 100}, {20, "meme", 1000, 100}});
  m.sample_for_tests(2000, kStat2, kMem, kLoad,
                     {{10, "meme", 100, 100}, {20, "meme", 1000, 100}});

  REQUIRE_EQ(m.rows_for_tests().size(), size_t{2});
  // Celui qui a bougé est le 10, pas le 20.
  for (const auto& r : m.rows_for_tests()) {
    if (r.pid == 20) CHECK_EQ(r.cpu_percent, 0);
    if (r.pid == 10) CHECK(r.cpu_percent > 0);
  }
}

// Un processus multi-fils peut consommer PLUS que le temps écoulé sur un
// cœur. Sans plafond, la colonne afficherait 340 %.
TEST(monitor_caps_a_process_at_a_hundred_percent) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, {{10, "fils", 0, 100}});
  m.sample_for_tests(2000, kStat2, kMem, kLoad, {{10, "fils", 100000, 100}});

  REQUIRE_EQ(m.rows_for_tests().size(), size_t{1});
  CHECK_EQ(m.rows_for_tests()[0].cpu_percent, 100);
}

// Deux processus à égalité gardent un ordre DÉTERMINÉ : sinon la liste
// saute d'une seconde à l'autre sous les yeux.
TEST(monitor_settles_a_tie_by_pid) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad,
                     {{50, "b", 0, 100}, {10, "a", 0, 100}});
  m.sample_for_tests(2000, kStat2, kMem, kLoad,
                     {{50, "b", 0, 100}, {10, "a", 0, 100}});

  REQUIRE_EQ(m.rows_for_tests().size(), size_t{2});
  CHECK_EQ(m.rows_for_tests()[0].pid, 10);
}

// Seule une TOUCHE DE CARACTÈRE change le tri : une flèche ne doit pas
// réordonner la liste sous les doigts de qui la parcourt.
TEST(monitor_ignores_a_key_that_is_not_a_character) {
  Monitor m;
  m.on_key(KeyEvent{Key::Up, U'm', 0});
  CHECK(m.sort_for_tests() == Monitor::Sort::Cpu);
}

// La liste est REFAITE à chaque échantillon : garder les anciennes lignes
// afficherait des processus morts, en double.
TEST(monitor_rebuilds_its_list_at_every_sample) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());
  REQUIRE_EQ(m.rows_for_tests().size(), size_t{2});

  m.sample_for_tests(3000, kStat2, kMem, kLoad, {{10, "gourmand", 400, 500}});
  CHECK_EQ(m.rows_for_tests().size(), size_t{1});
}

// Le TOTAL n'est pas un cœur : l'afficher en plus donnerait une barre de
// trop, et un utilisateur qui compte ses cœurs sur l'écran.
TEST(monitor_never_paints_the_total_as_a_core) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  const std::string g = painted(m, 50, 12);
  CHECK(g.find("0[") != std::string::npos);
  CHECK(g.find("1[") != std::string::npos);
  CHECK(g.find("2[") == std::string::npos);
}

TEST(monitor_names_the_load_it_paints) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, kLoad, procs1());
  m.sample_for_tests(2000, kStat2, kMem, kLoad, procs2());

  CHECK(painted(m, 50, 12).find("charge") != std::string::npos);
}

// Les centièmes gardent leur zéro de tête : « 1.05 » n'est pas « 1.5 ».
TEST(monitor_keeps_the_leading_zero_of_the_hundredths) {
  Monitor m;
  m.sample_for_tests(1000, kStat1, kMem, "1.05 0.09 0.00 1/1 1", procs1());
  m.sample_for_tests(2000, kStat2, kMem, "1.05 0.09 0.00 1/1 1", procs2());

  const std::string g = painted(m, 60, 12);
  CHECK(g.find("1.05") != std::string::npos);
  CHECK(g.find("0.09") != std::string::npos);
}
