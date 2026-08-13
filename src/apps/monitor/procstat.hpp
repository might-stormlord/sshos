#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sshos {

// Les compteurs d'un cœur, tels que `/proc/stat` les donne : CUMULÉS
// DEPUIS LE DÉMARRAGE. Un pourcentage ne se lit donc jamais dans un
// échantillon, il se calcule entre DEUX.
struct CpuTimes {
  uint64_t busy = 0;
  uint64_t total = 0;
  bool operator==(const CpuTimes&) const = default;
};

struct MemInfo {
  uint64_t total_kb = 0;
  uint64_t available_kb = 0;
  bool operator==(const MemInfo&) const = default;
};

struct ProcInfo {
  int pid = 0;
  std::string name;
  uint64_t cpu_ticks = 0;  // utime + stime
  uint64_t rss_pages = 0;
  bool operator==(const ProcInfo&) const = default;
};

// `/proc/stat`. Le premier élément est le TOTAL (`cpu`), les suivants sont
// les cœurs (`cpu0`, `cpu1`, …) dans l'ordre du fichier.
std::vector<CpuTimes> parse_cpu_times(std::string_view text);

// Le pourcentage occupé entre deux échantillons, borné à [0, 100].
//
// Un compteur qui RECULE -- cœur qui disparaît, machine suspendue,
// échantillon plus vieux que l'autre -- rend zéro. Rendre un pourcentage
// négatif afficherait une barre à l'envers ; rendre la valeur brute
// afficherait 4 milliards.
int cpu_percent(const CpuTimes& before, const CpuTimes& after);

// `/proc/meminfo`. `MemAvailable` n'existe pas sur les vieux noyaux : on
// se rabat sur `MemFree`, qui sous-estime mais ne ment pas sur l'ordre de
// grandeur.
MemInfo parse_meminfo(std::string_view text);

// `/proc/loadavg`, en centièmes pour éviter le flottant dans l'affichage.
// Rend {0,0,0} sur une ligne illisible.
std::vector<int> parse_loadavg(std::string_view text);

// `/proc/[pid]/stat`.
//
// LE PIÈGE : le nom du processus est entre parenthèses et peut contenir
// des espaces ET des parenthèses -- « (Web Content) », « (a (b) c) ».
// Découper sur les espaces donne alors n'importe quoi, et lire le champ 3
// donne un morceau de nom au lieu de l'état. On cherche donc la DERNIÈRE
// parenthèse fermante.
bool parse_process_stat(std::string_view text, ProcInfo& out);

}  // namespace sshos
