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

// La couleur d'une jauge, et elle SEULE porte de la couleur : les cadres
// et les titres restent sobres. Une couleur qui signifie « tout va bien »
// partout ne signifie plus rien ; ici elle ne sert qu'a alerter.
Color gauge_color(int percent) {
  if (percent >= 85) return Color::indexed(1);   // rouge
  if (percent >= 60) return Color::indexed(3);   // jaune
  return Color::indexed(2);                      // vert
}

std::string bar(int percent, int width, Border b) {
  const std::string full = b == Border::Unicode ? "\u2588" : "#";
  const std::string empty = b == Border::Unicode ? "\u2591" : "-";
  std::string out;
  const int filled = width * std::clamp(percent, 0, 100) / 100;
  for (int i = 0; i < width; ++i) out += (i < filled) ? full : empty;
  return out;
}

// Un cadre titre. Le titre s'incruste DANS le trait du haut : une ligne de
// titre separee couterait une ligne sur quatre dans une boite de quatre.
void frame(View v, const Rect& r, std::string_view title, const Style& st,
           const Style& bg, Border b) {
  if (r.w < 4 || r.h < 2) return;
  // ON EFFACE D'ABORD. La trame du fond passe sous les boites, et sans ce
  // nettoyage ses points transparaissent dans les creux -- entre le
  // chiffre et la jauge, entre deux lignes de processus -- ce qui rend
  // illisible exactement ce qu'on est venu lire.
  v.fill(r, bg);
  const bool uni = b == Border::Unicode;
  const std::string tl = uni ? "\u250c" : "+", tr = uni ? "\u2510" : "+";
  const std::string bl = uni ? "\u2514" : "+", br = uni ? "\u2518" : "+";
  const std::string h = uni ? "\u2500" : "-", vv = uni ? "\u2502" : "|";

  std::string top = tl + " " + std::string(title) + " ";
  const int used = 1 + 1 + static_cast<int>(title.size()) + 1;
  for (int i = used; i < r.w - 1; ++i) top += h;
  top += tr;
  v.text(r.x, r.y, top, st);

  for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
    v.text(r.x, y, vv, st);
    v.text(r.x + r.w - 1, y, vv, st);
  }
  std::string bottom = bl;
  for (int i = 1; i < r.w - 1; ++i) bottom += h;
  bottom += br;
  v.text(r.x, r.y + r.h - 1, bottom, st);
}

// Une boite « compteur » : un grand chiffre, puis sa jauge.
void counter_box(View v, const Rect& r, std::string_view title,
                 const std::string& value, int percent, const Style& chrome,
                 const Style& body, const Style& bg, Border b) {
  frame(v, r, title, chrome, bg, b);
  if (r.h < 3 || r.w < 6) return;
  v.text(r.x + 2, r.y + 1, value, body);
  Style g = body;
  g.fg = gauge_color(percent);
  v.text(r.x + 2, r.y + 2, bar(percent, r.w - 4, b), g);
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

void SysInfo::draw(View v, const Theme& th, Border b) const {
  const int w = v.w();
  const int h = v.h();
  if (w < 24 || h < 8) return;  // trop etroit : mieux vaut ne rien dire

  Style chrome;
  chrome.fg = th.panel_fg;
  Style body;
  body.fg = th.panel_fg;
  body.attrs = attr::Bold;
  Style bg;
  bg.bg = th.desktop_bg;

  // DEUX COLONNES de compteurs quand la place le permet, une sinon. Les
  // boites font quatre lignes : le trait du haut porte le titre, puis le
  // chiffre, puis la jauge, puis le trait du bas.
  const bool two = w >= 26;
  const int bw = two ? w / 2 : w;
  const int bh = 4;

  const int cpu_pct = cores_.empty() ? 0 : cores_[0];
  const uint64_t used_kb =
      mem_.total_kb > mem_.available_kb ? mem_.total_kb - mem_.available_kb : 0;
  const int mem_pct =
      mem_.total_kb == 0 ? 0 : static_cast<int>(used_kb * 100 / mem_.total_kb);

  int y = 0;
  counter_box(v, Rect{0, y, bw, bh}, "CPU", std::to_string(cpu_pct) + "%",
              cpu_pct, chrome, body, bg, b);
  if (two) {
    counter_box(v, Rect{bw, y, w - bw, bh}, "MEM",
                std::to_string(mem_pct) + "%", mem_pct, chrome, body, bg, b);
    y += bh;
  } else {
    y += bh;
    counter_box(v, Rect{0, y, bw, bh}, "MEM", std::to_string(mem_pct) + "%",
                mem_pct, chrome, body, bg, b);
    y += bh;
  }

  // Le reseau et la charge : deux chiffres chacun, pas de jauge -- un debit
  // n'a pas de maximum connu, et une jauge sans plafond ment.
  if (y + bh <= h) {
    const std::string down = b == Border::Unicode ? "\u2193" : "v";
    const std::string up = b == Border::Unicode ? "\u2191" : "^";
    frame(v, Rect{0, y, bw, bh}, "RESEAU", chrome, bg, b);
    v.text(2, y + 1, down + " " + rate(rx_per_s_), body);
    v.text(2, y + 2, up + " " + rate(tx_per_s_), body);
    if (two) {
      frame(v, Rect{bw, y, w - bw, bh}, "CHARGE", chrome, bg, b);
      v.text(bw + 2, y + 1, two_decimals(load_.size() > 0 ? load_[0] : 0), body);
      v.text(bw + 2, y + 2,
             two_decimals(load_.size() > 1 ? load_[1] : 0) + " " +
                 two_decimals(load_.size() > 2 ? load_[2] : 0),
             body);
    }
    y += bh;
  }

  // Les processus, CINQ au plus. Au-dela, la liste cesse de repondre a « a
  // cause de qui ? » pour devenir un mur de texte -- c'est ce qui rendait
  // le fond illisible.
  constexpr size_t kMaxRows = 5;
  const int want = static_cast<int>(std::min(kMaxRows, rows_.size())) + 2;
  if (y + 3 <= h) {
    const int box_h = std::min(want, h - y);
    frame(v, Rect{0, y, w, box_h}, "PROCESSUS", chrome, bg, b);
    int line = y + 1;
    for (size_t i = 0; i < rows_.size() && i < kMaxRows; ++i) {
      if (line >= y + box_h - 1) break;
      const ProcRow& r = rows_[i];
      Style pct = body;
      pct.fg = gauge_color(r.cpu_percent);
      const std::string head = std::to_string(r.cpu_percent) + "%";
      v.text(2, line, head, pct);
      v.text(2 + 5, line,
             std::to_string(r.rss_kb / 1024) + "Mo  " + r.name, chrome);
      ++line;
    }
  }
}

namespace {

// Une police de blocs, cinq lignes par lettre. Ecrite a la main : trois
// colonnes suffisent a rendre S, H et O lisibles, et une police plus large
// deborderait de la moitie gauche sur un terminal de quatre-vingts
// colonnes.
const char* glyph_rows(char c, int row) {
  static const char* kS[5] = {"###", "#  ", "###", "  #", "###"};
  static const char* kH[5] = {"# #", "# #", "###", "# #", "# #"};
  static const char* kO[5] = {"###", "# #", "# #", "# #", "###"};
  static const char* kSpace[5] = {"   ", "   ", "   ", "   ", "   "};
  switch (c) {
    case 'S': return kS[row];
    case 'H': return kH[row];
    case 'O': return kO[row];
    default: return kSpace[row];
  }
}

}  // namespace

void SysInfo::draw_banner(View v, const Theme& th, Border b) {
  const char* kWord = "SSH OS";
  constexpr int kRows = 5;
  constexpr int kGlyphW = 3;
  constexpr int kGap = 1;
  const int word_w = static_cast<int>(std::string(kWord).size()) * (kGlyphW + kGap) - kGap;
  if (v.w() < word_w || v.h() < kRows) return;

  // Centree dans ce qu'on lui donne, et posee dans une teinte PROCHE DU
  // FOND : une signature qui se lit aussi bien que le contenu detourne
  // l'oeil de ce qu'on est venu faire.
  const int x0 = (v.w() - word_w) / 2;
  const int y0 = (v.h() - kRows) / 2;
  Style st;
  st.fg = th.desktop_sign;
  const std::string block = b == Border::Unicode ? "\u2588" : "#";

  for (int row = 0; row < kRows; ++row) {
    int x = x0;
    for (const char* p = kWord; *p != 0; ++p) {
      const char* bits = glyph_rows(*p, row);
      for (int i = 0; i < kGlyphW; ++i) {
        if (bits[i] == '#') v.text(x + i, y0 + row, block, st);
      }
      x += kGlyphW + kGap;
    }
  }
}

}  // namespace sshos
