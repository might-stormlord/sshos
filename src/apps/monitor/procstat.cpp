#include "apps/monitor/procstat.hpp"

#include <algorithm>
#include <cstdlib>

namespace sshos {
namespace {

// Les entiers d'une ligne, dans l'ordre. Écrit à la main plutôt
// qu'emprunté à `strtok` : `/proc` est une entrée comme une autre, et un
// analyseur qui suppose un format donne des chiffres faux au lieu d'un
// refus.
std::vector<uint64_t> numbers_of(std::string_view s) {
  std::vector<uint64_t> out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] < '0' || s[i] > '9') {
      ++i;
      continue;
    }
    uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + static_cast<uint64_t>(s[i] - '0');
      ++i;
    }
    out.push_back(v);
  }
  return out;
}

std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      if (start < text.size()) out.push_back(text.substr(start));
      break;
    }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

}  // namespace

std::vector<CpuTimes> parse_cpu_times(std::string_view text) {
  std::vector<CpuTimes> out;
  for (std::string_view line : lines_of(text)) {
    if (line.substr(0, 3) != "cpu") continue;
    const std::vector<uint64_t> n = numbers_of(line);
    // Les quatre premiers champs suffisent, mais il en faut au moins cinq
    // pour distinguer l'attente d'E/S du repos.
    if (n.size() < 5) continue;
    CpuTimes t;
    for (uint64_t v : n) t.total += v;
    // OCCUPÉ = tout sauf le repos (`idle`, 4e) et l'attente (`iowait`,
    // 5e). Compter l'attente comme du travail ferait afficher 100 % sur
    // une machine qui ne fait qu'attendre son disque.
    t.busy = t.total - n[3] - n[4];
    out.push_back(t);
  }
  return out;
}

int cpu_percent(const CpuTimes& before, const CpuTimes& after) {
  // Un compteur qui RECULE -- machine suspendue, cœur disparu,
  // échantillons inversés -- ne donne pas un pourcentage négatif : il
  // donne zéro. La soustraction non signée, elle, donnerait quatre
  // milliards.
  if (after.total <= before.total || after.busy < before.busy) return 0;
  const uint64_t elapsed = after.total - before.total;
  const uint64_t busy = after.busy - before.busy;
  const uint64_t pct = busy * 100 / elapsed;
  return static_cast<int>(std::min<uint64_t>(pct, 100));
}

MemInfo parse_meminfo(std::string_view text) {
  MemInfo m;
  uint64_t free_kb = 0;
  bool saw_available = false;
  for (std::string_view line : lines_of(text)) {
    const std::vector<uint64_t> n = numbers_of(line);
    if (n.empty()) continue;
    if (line.substr(0, 9) == "MemTotal:") m.total_kb = n[0];
    if (line.substr(0, 8) == "MemFree:") free_kb = n[0];
    if (line.substr(0, 13) == "MemAvailable:") {
      m.available_kb = n[0];
      saw_available = true;
    }
  }
  // `MemAvailable` n'existe pas sur les vieux noyaux. `MemFree`
  // sous-estime -- il ignore le cache récupérable -- mais ne ment pas sur
  // l'ordre de grandeur, et vaut mieux que zéro.
  if (!saw_available) m.available_kb = free_kb;
  return m;
}

std::vector<int> parse_loadavg(std::string_view text) {
  std::vector<int> out{0, 0, 0};
  size_t i = 0;
  for (int k = 0; k < 3; ++k) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    int whole = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      whole = whole * 10 + (text[i] - '0');
      ++i;
      any = true;
    }
    if (!any) break;
    int frac = 0;
    if (i < text.size() && text[i] == '.') {
      ++i;
      for (int d = 0; d < 2; ++d) {
        frac *= 10;
        if (i < text.size() && text[i] >= '0' && text[i] <= '9') {
          frac += text[i] - '0';
          ++i;
        }
      }
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
    out[static_cast<size_t>(k)] = whole * 100 + frac;
  }
  return out;
}

bool parse_process_stat(std::string_view text, ProcInfo& out) {
  const size_t open = text.find('(');
  // LA DERNIÈRE parenthèse fermante, pas la première : un nom comme
  // « (Web Content (isolated)) » en contient plusieurs, et s'arrêter à la
  // première tronquerait le nom ET décalerait tous les champs suivants --
  // le temps CPU deviendrait le numéro de session.
  const size_t close = text.rfind(')');
  // Garde de forme, ÉQUIVALENTE aujourd'hui : sans elle, `npos` fait lire
  // la ligne entière des deux côtés, et le compte de champs plus bas
  // refuse de toute façon. Elle reste parce qu'elle dit ce que la fonction
  // attend, et parce que le compte de champs, lui, pourrait changer.
  if (open == std::string_view::npos || close == std::string_view::npos ||
      close < open) {
    return false;
  }

  const std::vector<uint64_t> head = numbers_of(text.substr(0, open));
  if (head.empty()) return false;

  // Après la parenthèse : l'état, puis les champs numérotés à partir de 3.
  // `utime` est le 14e champ du fichier, `stime` le 15e, `rss` le 24e --
  // soit, comptés depuis le premier nombre qui suit le nom, les indices
  // 10, 11 et 20.
  const std::vector<uint64_t> tail = numbers_of(text.substr(close + 1));
  if (tail.size() < 21) return false;

  out.pid = static_cast<int>(head[0]);
  out.name = std::string(text.substr(open + 1, close - open - 1));
  out.cpu_ticks = tail[10] + tail[11];
  out.rss_pages = tail[20];
  return true;
}

}  // namespace sshos
