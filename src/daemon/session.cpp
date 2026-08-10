#include "daemon/session.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <variant>

namespace sshos {
namespace {

constexpr int kMinCols = 40;
constexpr int kMinRows = 12;

// A1 : deux formulations du même avertissement. Le message complet ne
// s'affiche que lorsque le terminal est trop petit — et se retrouvait donc
// tronqué précisément quand il servait le plus, sur les largeurs les plus
// étroites (View::text clippe à la largeur de la surface, sans notion de
// mot entier). En dessous de la longueur du message complet, la forme
// courte prend le relais ; elle est garantie de tenir jusqu'à 12 colonnes,
// la plus petite surface qu'un test de cette suite construit.
constexpr char kFullWarning[] = "terminal trop petit - 40x12 minimum";
constexpr char kShortWarning[] = "trop petit";
constexpr int kFullWarningLen = sizeof(kFullWarning) - 1;

std::string clock_text(const Platform& plat) {
  const std::time_t t = std::chrono::system_clock::to_time_t(plat.now());
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

}  // namespace

Session::Session(Platform& plat, int, int) : plat_(&plat) {}

void Session::resize(int, int) {}

void Session::on_input(const InputEvent& e) {
  if (const auto* k = std::get_if<KeyEvent>(&e)) {
    if (k->key == Key::Char && k->ch == U'q' && (k->mods & mod::Ctrl) != 0) {
      quit_ = true;
    }
  } else if (const auto* m = std::get_if<MouseEvent>(&e)) {
    if (m->action == MouseAction::Press) ++clicks_;
  }
}

void Session::render(Surface& out) {
  View v = out.root();
  Style bg;
  bg.bg = Color::indexed(4);
  v.fill(Rect{0, 0, out.w(), out.h()}, bg);

  if (out.w() < kMinCols || out.h() < kMinRows) {
    Style warn;
    warn.fg = Color::indexed(7);
    if (out.w() >= kFullWarningLen) {
      v.text(0, 0, kFullWarning, warn);
    } else {
      v.text(0, 0, kShortWarning, warn);
    }
    return;
  }

  // Panneau ancré en bas.
  Style panel;
  panel.bg = Color::indexed(0);
  panel.fg = Color::indexed(7);
  const int py = out.h() - 1;
  v.fill(Rect{0, py, out.w(), 1}, panel);
  v.text(1, py, "ssh_os", panel);
  const std::string t = clock_text(*plat_);
  v.text(out.w() - static_cast<int>(t.size()) - 1, py, t, panel);

  // Boîte centrée.
  const int bw = 24;
  const int bh = 6;
  const int bx = (out.w() - bw) / 2;
  const int by = (out.h() - 1 - bh) / 2;
  Style box;
  box.bg = Color::indexed(0);
  box.fg = Color::indexed(15);
  v.fill(Rect{bx, by, bw, bh}, box);
  for (int x = 1; x < bw - 1; ++x) {
    v.put(bx + x, by, U'-', box);
    v.put(bx + x, by + bh - 1, U'-', box);
  }
  for (int y = 1; y < bh - 1; ++y) {
    v.put(bx, by + y, U'|', box);
    v.put(bx + bw - 1, by + y, U'|', box);
  }
  v.put(bx, by, U'+', box);
  v.put(bx + bw - 1, by, U'+', box);
  v.put(bx, by + bh - 1, U'+', box);
  v.put(bx + bw - 1, by + bh - 1, U'+', box);

  v.text(bx + 2, by + 1, "ssh_os 2.0", box);
  v.text(bx + 2, by + 2, "clics: " + std::to_string(clicks_), box);
  v.text(bx + 2, by + 4, "Ctrl+Q pour quitter", box);
}

}  // namespace sshos
