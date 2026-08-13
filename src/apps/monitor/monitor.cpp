#include "apps/monitor/monitor.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "render/surface.hpp"

namespace sshos {
namespace {

// Le plafond de rafraîchissement. Sans lui, chaque trame relirait `/proc`
// -- trente fois par seconde -- et le moniteur deviendrait la charge qu'il
// mesure.
constexpr int64_t kSamplePeriodMs = 1000;

// La taille d'une page, pour convertir la mémoire résidente. Lue une fois :
// elle ne change pas sous nos pieds.
uint64_t page_kb() {
  static const uint64_t v =
      static_cast<uint64_t>(::sysconf(_SC_PAGESIZE)) / 1024;
  return v == 0 ? 4 : v;
}

std::string read_file(const std::string& path) {
  std::string out;
  FILE* f = ::fopen(path.c_str(), "re");
  if (f == nullptr) return out;
  char buf[8192];
  size_t n = 0;
  while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  ::fclose(f);
  return out;
}

// Une barre d'occupation, en cellules pleines et vides.
std::string bar(int percent, int width) {
  std::string out;
  const int filled = width * std::clamp(percent, 0, 100) / 100;
  for (int i = 0; i < width; ++i) out += (i < filled) ? "|" : " ";
  return out;
}

std::string two_decimals(int hundredths) {
  return std::to_string(hundredths / 100) + "." +
         (hundredths % 100 < 10 ? "0" : "") + std::to_string(hundredths % 100);
}

}  // namespace

Monitor::Monitor() : load_{0, 0, 0} {}

void Monitor::attach(Host& host) {
  host_ = &host;
  host.set_title("Moniteur");
}

void Monitor::sample_for_tests(int64_t now_ms, std::string_view stat,
                               std::string_view meminfo,
                               std::string_view loadavg,
                               const std::vector<ProcInfo>& procs) {
  apply_sample(now_ms, stat, meminfo, loadavg, procs);
}

void Monitor::apply_sample(int64_t now_ms, std::string_view stat,
                           std::string_view meminfo, std::string_view loadavg,
                           const std::vector<ProcInfo>& procs) {
  // Au plus une fois par seconde. Le premier échantillon passe toujours :
  // sans lui, il n'y aurait jamais de second, donc jamais de delta.
  if (sampled_once_ && now_ms - last_ms_ < kSamplePeriodMs) return;

  const std::vector<CpuTimes> cpu = parse_cpu_times(stat);
  mem_ = parse_meminfo(meminfo);
  load_ = parse_loadavg(loadavg);

  core_percent_.assign(cpu.size(), 0);
  rows_.clear();
  if (sampled_once_ && prev_cpu_.size() == cpu.size()) {
    for (size_t i = 0; i < cpu.size(); ++i) {
      core_percent_[i] = cpu_percent(prev_cpu_[i], cpu[i]);
    }
    // Le pourcentage d'un processus se rapporte au temps ÉCOULÉ sur toute
    // la machine, pas au sien : sans ce dénominateur commun, deux
    // processus à 50 % ne voudraient pas dire la même chose.
    const uint64_t elapsed =
        cpu.empty() ? 0 : (cpu[0].total > prev_cpu_[0].total
                               ? cpu[0].total - prev_cpu_[0].total
                               : 0);
    for (const ProcInfo& p : procs) {
      ProcRow r;
      r.pid = p.pid;
      r.name = p.name;
      r.rss_kb = p.rss_pages * page_kb();
      // Un processus APPARU entre deux échantillons n'a pas de passé : lui
      // prêter le temps d'un autre le placerait en tête à tort.
      const auto it = std::find_if(
          prev_procs_.begin(), prev_procs_.end(),
          [&p](const ProcInfo& q) { return q.pid == p.pid; });
      if (it != prev_procs_.end() && elapsed > 0 && p.cpu_ticks >= it->cpu_ticks) {
        const uint64_t used = p.cpu_ticks - it->cpu_ticks;
        r.cpu_percent = static_cast<int>(
            std::min<uint64_t>(used * 100 / elapsed, 100));
      }
      rows_.push_back(std::move(r));
    }
    has_prev_ = true;
  }

  prev_cpu_ = cpu;
  prev_procs_ = procs;
  last_ms_ = now_ms;
  sampled_once_ = true;
  resort();
}

void Monitor::resort() {
  // Tri STABLE et départagé par le pid : deux processus à égalité doivent
  // garder un ordre déterminé, sinon la liste saute d'une seconde à
  // l'autre sous les yeux.
  std::stable_sort(rows_.begin(), rows_.end(),
                   [this](const ProcRow& a, const ProcRow& b) {
                     if (sort_ == Sort::Memory) {
                       if (a.rss_kb != b.rss_kb) return a.rss_kb > b.rss_kb;
                     } else {
                       if (a.cpu_percent != b.cpu_percent) {
                         return a.cpu_percent > b.cpu_percent;
                       }
                     }
                     return a.pid < b.pid;
                   });
}

void Monitor::on_key(const KeyEvent& k) {
  if (k.key != Key::Char) return;
  if (k.ch == U'm' || k.ch == U'M') {
    sort_ = Sort::Memory;
    resort();
    return;
  }
  if (k.ch == U'c' || k.ch == U'C') {
    sort_ = Sort::Cpu;
    resort();
  }
}

void Monitor::render(View v) {
  const int w = v.w();
  const int h = v.h();
  if (w <= 0 || h <= 0) return;

  // LE RAFRAÎCHISSEMENT SE DÉCLENCHE ICI. Une fenêtre cachée n'est pas
  // dessinée, donc rien n'est lu : la règle « un moniteur minimisé ne
  // consomme rien » devient structurelle au lieu d'être une discipline.
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::vector<ProcInfo> procs;
  if (!frozen_ && (!sampled_once_ || now_ms - last_ms_ >= kSamplePeriodMs)) {
    DIR* d = ::opendir("/proc");
    if (d != nullptr) {
      for (;;) {
        const dirent* e = ::readdir(d);
        if (e == nullptr) break;
        const std::string name = e->d_name;
        if (name.empty() || name[0] < '0' || name[0] > '9') continue;
        ProcInfo p;
        if (parse_process_stat(read_file("/proc/" + name + "/stat"), p)) {
          procs.push_back(std::move(p));
        }
      }
      ::closedir(d);
    }
    apply_sample(now_ms, read_file("/proc/stat"), read_file("/proc/meminfo"),
                 read_file("/proc/loadavg"), procs);
  }

  Style head;
  head.attrs = attr::Bold;
  int y = 0;
  v.text(0, y++, "charge " + two_decimals(load_.size() > 0 ? load_[0] : 0) +
                     "  " + two_decimals(load_.size() > 1 ? load_[1] : 0) +
                     "  " + two_decimals(load_.size() > 2 ? load_[2] : 0),
         head);

  const uint64_t used_kb =
      mem_.total_kb > mem_.available_kb ? mem_.total_kb - mem_.available_kb : 0;
  const int mem_pct =
      mem_.total_kb == 0 ? 0
                         : static_cast<int>(used_kb * 100 / mem_.total_kb);
  if (y < h) {
    v.text(0, y++,
           "Mem[" + bar(mem_pct, std::max(4, w / 3)) + "] " +
               std::to_string(mem_pct) + "%",
           Style{});
  }

  // Une barre par cœur. L'indice 0 est le TOTAL : on ne l'affiche pas deux
  // fois, les cœurs le disent déjà.
  for (size_t i = 1; i < core_percent_.size() && y < h - 1; ++i) {
    Style st;
    st.fg = core_percent_[i] >= 80 ? Color::indexed(1) : Color::indexed(2);
    v.text(0, y++,
           std::to_string(i - 1) + "[" +
               bar(core_percent_[i], std::max(4, w / 3)) + "] " +
               std::to_string(core_percent_[i]) + "%",
           st);
  }

  if (y < h) {
    Style cols;
    cols.attrs = attr::Reverse;
    v.fill(Rect{0, y, w, 1}, cols);
    v.text(0, y++, "  PID  CPU%   MEM  COMMANDE", cols);
  }
  for (const ProcRow& r : rows_) {
    if (y >= h) break;
    const std::string line = std::to_string(r.pid) + "  " +
                             std::to_string(r.cpu_percent) + "%  " +
                             std::to_string(r.rss_kb / 1024) + "M  " + r.name;
    v.text(0, y++, line, Style{});
  }
}

}  // namespace sshos
