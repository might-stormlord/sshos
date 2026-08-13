#include <string>
#include <vector>

#include "apps/monitor/procstat.hpp"
#include "harness.hpp"

using sshos::CpuTimes;
using sshos::cpu_percent;
using sshos::MemInfo;
using sshos::parse_cpu_times;
using sshos::parse_loadavg;
using sshos::parse_meminfo;
using sshos::parse_process_stat;
using sshos::ProcInfo;

// ------------------------------------------------------------ /proc/stat

// Le premier élément est le TOTAL, les suivants les cœurs : les confondre
// afficherait la charge globale sur la barre du cœur 0.
TEST(procstat_reads_the_total_then_each_core) {
  const std::string text =
      "cpu  100 20 30 800 10 0 5 0 0 0\n"
      "cpu0 50 10 15 400 5 0 2 0 0 0\n"
      "cpu1 50 10 15 400 5 0 3 0 0 0\n"
      "intr 12345\n";
  const std::vector<CpuTimes> v = parse_cpu_times(text);

  REQUIRE_EQ(v.size(), size_t{3});
  CHECK_EQ(v[0].total, uint64_t{965});
  // Occupé = tout sauf le repos (le 4e champ) et l'attente (le 5e).
  CHECK_EQ(v[0].busy, uint64_t{155});
  CHECK_EQ(v[1].total, uint64_t{482});
}

// Une ligne qui n'est pas un cœur ne doit pas entrer dans la liste : le
// nombre de barres serait faux.
TEST(procstat_ignores_the_lines_that_are_not_cpus) {
  // La ligne intruse porte AUTANT DE CHAMBRES qu'un cœur : sans cela,
  // c'est la garde de longueur qui l'écarte, et le test ne dit rien du
  // préfixe.
  const std::string text =
      "cpu  1 1 1 1 1 1 1 0 0 0\n"
      "intr 10 11 12 13 14 15 16 17\n"
      "ctxt 999\n"
      "processes 42\n";
  CHECK_EQ(parse_cpu_times(text).size(), size_t{1});
}

TEST(procstat_reads_nothing_from_an_empty_file) {
  CHECK(parse_cpu_times("").empty());
}

// -------------------------------------------------------- le pourcentage

TEST(procstat_computes_the_percentage_between_two_samples) {
  const CpuTimes before{100, 1000};
  const CpuTimes after{150, 1100};  // 50 occupés sur 100 écoulés
  CHECK_EQ(cpu_percent(before, after), 50);
}

TEST(procstat_reports_a_full_core_as_a_hundred) {
  CHECK_EQ(cpu_percent(CpuTimes{100, 1000}, CpuTimes{200, 1100}), 100);
}

// Deux échantillons identiques : rien ne s'est écoulé. Diviser par zéro
// est le premier réflexe, et il plante.
TEST(procstat_survives_two_identical_samples) {
  CHECK_EQ(cpu_percent(CpuTimes{100, 1000}, CpuTimes{100, 1000}), 0);
}

// Un compteur qui RECULE -- machine suspendue, cœur disparu, échantillons
// inversés -- rend zéro. Un pourcentage négatif afficherait une barre à
// l'envers, et la valeur brute non signée afficherait quatre milliards.
TEST(procstat_reports_zero_when_a_counter_went_backwards) {
  CHECK_EQ(cpu_percent(CpuTimes{200, 2000}, CpuTimes{100, 1000}), 0);
  CHECK_EQ(cpu_percent(CpuTimes{100, 1000}, CpuTimes{50, 1100}), 0);
}

// Et jamais au-delà de cent, quoi qu'on lui donne.
TEST(procstat_never_reports_more_than_a_hundred) {
  CHECK_EQ(cpu_percent(CpuTimes{0, 1000}, CpuTimes{500, 1100}), 100);
}

// --------------------------------------------------------- /proc/meminfo

TEST(procstat_reads_the_memory_it_needs) {
  const std::string text =
      "MemTotal:       16384000 kB\n"
      "MemFree:         1000000 kB\n"
      "MemAvailable:    8000000 kB\n"
      "Buffers:          200000 kB\n";
  const MemInfo m = parse_meminfo(text);
  CHECK_EQ(m.total_kb, uint64_t{16384000});
  CHECK_EQ(m.available_kb, uint64_t{8000000});
}

// `MemAvailable` n'existe pas sur les vieux noyaux : `MemFree` sous-estime
// mais ne ment pas sur l'ordre de grandeur, et vaut mieux que zéro.
TEST(procstat_falls_back_to_free_when_available_is_missing) {
  const std::string text =
      "MemTotal:       1000 kB\n"
      "MemFree:         400 kB\n";
  const MemInfo m = parse_meminfo(text);
  CHECK_EQ(m.total_kb, uint64_t{1000});
  CHECK_EQ(m.available_kb, uint64_t{400});
}

// Mais `MemAvailable` GAGNE quand les deux sont là : c'est la seule des
// deux qui tienne compte du cache récupérable.
TEST(procstat_prefers_available_over_free) {
  const std::string text =
      "MemTotal:       1000 kB\n"
      "MemFree:         100 kB\n"
      "MemAvailable:    700 kB\n";
  CHECK_EQ(parse_meminfo(text).available_kb, uint64_t{700});
}

// --------------------------------------------------------- /proc/loadavg

TEST(procstat_reads_the_three_load_figures_in_hundredths) {
  const std::vector<int> l = parse_loadavg("1.25 0.50 4.08 2/1234 5678\n");
  REQUIRE_EQ(l.size(), size_t{3});
  CHECK_EQ(l[0], 125);
  CHECK_EQ(l[1], 50);
  CHECK_EQ(l[2], 408);
}

TEST(procstat_reads_zeros_from_an_unreadable_loadavg) {
  const std::vector<int> l = parse_loadavg("");
  REQUIRE_EQ(l.size(), size_t{3});
  CHECK_EQ(l[0], 0);
}

// ------------------------------------------------------ /proc/[pid]/stat

TEST(procstat_reads_a_process_line) {
  ProcInfo p;
  REQUIRE(parse_process_stat(
      "1234 (bash) S 1 1234 1234 0 -1 4194304 100 200 0 0 "
      "11 22 0 0 20 0 1 0 999 4096000 512 ",
      p));
  CHECK_EQ(p.pid, 1234);
  CHECK_EQ(p.name, std::string("bash"));
  CHECK_EQ(p.cpu_ticks, uint64_t{33});  // 11 + 22
  CHECK_EQ(p.rss_pages, uint64_t{512});
}

// LE PIÈGE. Le nom est entre parenthèses et peut contenir des ESPACES et
// des PARENTHÈSES. Découper sur les espaces, ou chercher la première
// parenthèse fermante, donne un nom tronqué et surtout DÉCALE tous les
// champs suivants -- le temps CPU d'un processus deviendrait son numéro
// de session.
TEST(procstat_reads_a_name_that_contains_spaces_and_parentheses) {
  ProcInfo p;
  REQUIRE(parse_process_stat(
      "42 (Web Content (isolated)) S 1 42 42 0 -1 0 0 0 0 0 "
      "7 8 0 0 20 0 1 0 999 4096000 256 ",
      p));
  CHECK_EQ(p.pid, 42);
  CHECK_EQ(p.name, std::string("Web Content (isolated)"));
  CHECK_EQ(p.cpu_ticks, uint64_t{15});
  CHECK_EQ(p.rss_pages, uint64_t{256});
}

TEST(procstat_refuses_a_line_it_cannot_read) {
  ProcInfo p;
  CHECK(!parse_process_stat("", p));
  CHECK(!parse_process_stat("42 sans-parentheses S 1", p));
  CHECK(!parse_process_stat("42 (tronque) S 1", p));
}
