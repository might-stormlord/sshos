#include "shell/sysinfo.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace sshos {
namespace {

constexpr int64_t kPeriodMs = 1000;

bool& frozen() {
  static bool f = false;
  return f;
}

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

std::string bar(int percent, int width) {
  std::string out;
  const int filled = width * std::clamp(percent, 0, 100) / 100;
  for (int i = 0; i < width; ++i) out += (i < filled) ? "|" : " ";
  return out;
}

// Un débit lisible. Les octets bruts sont illisibles au-delà du millier, et
// c'est justement là que le chiffre devient intéressant.
std::string rate(uint64_t bytes_per_s) {
  if (bytes_per_s >= 1024 * 1024) {
    return std::to_string(bytes_per_s / (1024 * 1024)) + "Mo/s";
  }
  if (bytes_per_s >= 1024) return std::to_string(bytes_per_s / 1024) + "Ko/s";
  return std::to_string(bytes_per_s) + "o/s";
}

std::string two_decimals(int hundredths) {
  return std::to_string(hundredths / 100) + "." +
         (hundredths % 100 < 10 ? "0" : "") + std::to_string(hundredths % 100);
}

}  // namespace

void SysInfo::sample_for_tests(int64_t now_ms, std::string_view stat,
                               std::string_view meminfo,
                               std::string_view loadavg,
                               std::string_view netdev,
                               const std::vector<ProcInfo>& procs) {
  apply(now_ms, stat, meminfo, loadavg, netdev, procs);
}

void SysInfo::freeze_for_tests() { frozen() = true; }

void SysInfo::refresh(int64_t now_ms) {
  if (frozen()) return;
  if (sampled_ && now_ms - last_ms_ < kPeriodMs) return;

  std::vector<ProcInfo> procs;
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
  apply(now_ms, read_file("/proc/stat"), read_file("/proc/meminfo"),
        read_file("/proc/loadavg"), read_file("/proc/net/dev"), procs);
}

void SysInfo::apply(int64_t now_ms, std::string_view stat,
                    std::string_view meminfo, std::string_view loadavg,
                    std::string_view netdev,
                    const std::vector<ProcInfo>& procs) {
  if (sampled_ && now_ms - last_ms_ < kPeriodMs) return;

  const std::vector<CpuTimes> cpu = parse_cpu_times(stat);
  const NetTotals net = parse_netdev(netdev);
  mem_ = parse_meminfo(meminfo);
  load_ = parse_loadavg(loadavg);

  cores_.assign(cpu.size(), 0);
  rows_.clear();
  rx_per_s_ = 0;
  tx_per_s_ = 0;

  if (sampled_ && prev_cpu_.size() == cpu.size()) {
    for (size_t i = 0; i < cpu.size(); ++i) {
      cores_[i] = cpu_percent(prev_cpu_[i], cpu[i]);
    }
    // Le débit se rapporte au TEMPS RÉEL écoulé, pas au tick nominal :
    // deux échantillons séparés de trois secondes -- ce qui arrive dès que
    // le bureau n'est pas dessiné -- donneraient sinon un débit triple.
    const int64_t ms = std::max<int64_t>(1, now_ms - last_ms_);
    if (net.rx >= prev_net_.rx) {
      rx_per_s_ = (net.rx - prev_net_.rx) * 1000 / static_cast<uint64_t>(ms);
    }
    if (net.tx >= prev_net_.tx) {
      tx_per_s_ = (net.tx - prev_net_.tx) * 1000 / static_cast<uint64_t>(ms);
    }

    const uint64_t elapsed =
        cpu.empty() ? 0
                    : (cpu[0].total > prev_cpu_[0].total
                           ? cpu[0].total - prev_cpu_[0].total
                           : 0);
    for (const ProcInfo& p : procs) {
      ProcRow r;
      r.pid = p.pid;
      r.name = p.name;
      r.rss_kb = p.rss_pages * page_kb();
      const auto it = std::find_if(
          prev_procs_.begin(), prev_procs_.end(),
          [&p](const ProcInfo& q) { return q.pid == p.pid; });
      if (it != prev_procs_.end() && elapsed > 0 &&
          p.cpu_ticks >= it->cpu_ticks) {
        const uint64_t used = p.cpu_ticks - it->cpu_ticks;
        r.cpu_percent =
            static_cast<int>(std::min<uint64_t>(used * 100 / elapsed, 100));
      }
      rows_.push_back(std::move(r));
    }
    std::stable_sort(rows_.begin(), rows_.end(),
                     [](const ProcRow& a, const ProcRow& b) {
                       if (a.cpu_percent != b.cpu_percent) {
                         return a.cpu_percent > b.cpu_percent;
                       }
                       return a.pid < b.pid;
                     });
  }

  prev_cpu_ = cpu;
  prev_procs_ = procs;
  prev_net_ = net;
  last_ms_ = now_ms;
  sampled_ = true;
}

void SysInfo::draw(View v, const Theme& th) const {
  const int w = v.w();
  const int h = v.h();
  if (w < 16 || h < 4) return;  // trop etroit : mieux vaut ne rien dire

  Style title;
  title.fg = th.panel_fg;
  title.attrs = attr::Bold;
  Style body;
  body.fg = th.panel_fg;

  const int barw = std::max(4, w / 3);
  int y = 0;

  // --- le processeur
  v.text(0, y++, "PROCESSEUR", title);
  if (y < h) {
    v.text(0, y++,
           "charge " + two_decimals(load_.size() > 0 ? load_[0] : 0) + "  " +
               two_decimals(load_.size() > 1 ? load_[1] : 0) + "  " +
               two_decimals(load_.size() > 2 ? load_[2] : 0),
           body);
  }
  // L'indice 0 est le TOTAL : les coeurs le disent deja.
  for (size_t i = 1; i < cores_.size() && y < h; ++i) {
    Style st = body;
    if (cores_[i] >= 80) st.fg = Color::indexed(1);
    v.text(0, y++,
           std::to_string(i - 1) + "[" + bar(cores_[i], barw) + "] " +
               std::to_string(cores_[i]) + "%",
           st);
  }

  // --- la memoire
  if (y < h) ++y;
  if (y < h) v.text(0, y++, "MEMOIRE", title);
  if (y < h) {
    const uint64_t used = mem_.total_kb > mem_.available_kb
                              ? mem_.total_kb - mem_.available_kb
                              : 0;
    const int pct = mem_.total_kb == 0
                        ? 0
                        : static_cast<int>(used * 100 / mem_.total_kb);
    v.text(0, y++,
           "[" + bar(pct, barw) + "] " + std::to_string(used / 1024) + "/" +
               std::to_string(mem_.total_kb / 1024) + "Mo",
           body);
  }

  // --- le reseau
  if (y < h) ++y;
  if (y < h) v.text(0, y++, "RESEAU", title);
  if (y < h) v.text(0, y++, "recu  " + rate(rx_per_s_), body);
  if (y < h) v.text(0, y++, "emis  " + rate(tx_per_s_), body);

  // --- les processus, tronques les premiers quand la place manque : ils
  // repondent a « a cause de qui ? », les autres a « la machine
  // souffre-t-elle ? », et c'est la seconde question qui vient d'abord.
  if (y < h) ++y;
  if (y < h) v.text(0, y++, "PROCESSUS", title);
  for (const ProcRow& r : rows_) {
    if (y >= h) break;
    v.text(0, y++,
           std::to_string(r.cpu_percent) + "%  " +
               std::to_string(r.rss_kb / 1024) + "Mo  " + r.name,
           body);
  }
}

}  // namespace sshos
