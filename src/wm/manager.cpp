#include "wm/manager.hpp"

#include <algorithm>
#include <utility>

namespace sshos {
namespace {

// Géométrie de départ de toute nouvelle fenêtre, avant décalage en cascade.
constexpr Rect kDefaultRect{2, 1, 44, 14};

// Décalage d'une marche de cascade. Deux colonnes et une ligne : assez pour
// que la barre de titre de la fenêtre de dessous reste visible et cliquable,
// pas assez pour gaspiller l'écran.
constexpr int kCascadeDx = 2;
constexpr int kCascadeDy = 1;

int distance(int a, int b) { return a > b ? a - b : b - a; }

}  // namespace

Rect snap(Rect r, const Rect& work, int tolerance) {
  if (distance(r.x, work.x) <= tolerance) {
    r.x = work.x;
  } else if (distance(r.x + r.w, work.x + work.w) <= tolerance) {
    r.x = work.x + work.w - r.w;
  }
  if (distance(r.y, work.y) <= tolerance) {
    r.y = work.y;
  } else if (distance(r.y + r.h, work.y + work.h) <= tolerance) {
    r.y = work.y + work.h - r.h;
  }
  return r;
}

Window* WindowManager::open(std::unique_ptr<App> app, const Rect& work) {
  if (stack_.size() >= kMaxWindows) return nullptr;
  if (!app) return nullptr;

  auto w = std::make_unique<Window>();
  w->id = next_id_++;
  w->user_rect = Rect{kDefaultRect.x + kCascadeDx * cascade_,
                      kDefaultRect.y + kCascadeDy * cascade_, kDefaultRect.w,
                      kDefaultRect.h};
  w->app = std::move(app);
  w->display_rect = clamp_to(w->user_rect, work, frame_min(*w->app));

  // La cascade repart à zéro dès que la marche SUIVANTE sortirait de la
  // zone. Sans ce retour, user_rect continuerait de descendre vers le
  // bas-droite pendant que clamp_to() saturerait display_rect contre le
  // coin : passé la dix-septième fenêtre, toutes se superposeraient
  // exactement, ce qui est précisément ce que la cascade existe pour
  // éviter.
  ++cascade_;
  const int next_x = kDefaultRect.x + kCascadeDx * cascade_;
  const int next_y = kDefaultRect.y + kCascadeDy * cascade_;
  if (next_x + kDefaultRect.w > work.x + work.w ||
      next_y + kDefaultRect.h > work.y + work.h) {
    cascade_ = 0;
  }

  Window* raw = w.get();
  stack_.push_back(std::move(w));
  focus(raw->id);
  return raw;
}

bool WindowManager::close(WindowId id) {
  const auto it = std::find_if(
      stack_.begin(), stack_.end(),
      [id](const std::unique_ptr<Window>& w) { return w->id == id; });
  if (it == stack_.end()) return false;

  const bool was_focused = (focused_ == id);
  stack_.erase(it);
  if (was_focused) {
    focused_ = stack_.empty() ? 0 : stack_.back()->id;
  }
  return true;
}

Window* WindowManager::find(WindowId id) {
  const auto it = std::find_if(
      stack_.begin(), stack_.end(),
      [id](const std::unique_ptr<Window>& w) { return w->id == id; });
  return it == stack_.end() ? nullptr : it->get();
}

const Window* WindowManager::find(WindowId id) const {
  const auto it = std::find_if(
      stack_.begin(), stack_.end(),
      [id](const std::unique_ptr<Window>& w) { return w->id == id; });
  return it == stack_.end() ? nullptr : it->get();
}

void WindowManager::raise(WindowId id) {
  const auto it = std::find_if(
      stack_.begin(), stack_.end(),
      [id](const std::unique_ptr<Window>& w) { return w->id == id; });
  if (it == stack_.end() || it + 1 == stack_.end()) return;
  // std::rotate ne déplace que les unique_ptr ; les Window elles-mêmes ne
  // bougent pas d'un octet, et les HostImpl qui les pointent restent
  // valides.
  std::rotate(it, it + 1, stack_.end());
}

void WindowManager::focus(WindowId id) {
  // Une seule fenêtre plein écran à la fois. Celle qui perd la main sort du
  // mode et retrouve l'état où elle était avant d'y entrer -- sans quoi une
  // fenêtre plein écran resterait à couvrir tout l'écran derrière celle qui
  // vient de prendre le focus.
  for (auto& w : stack_) {
    if (w->id != id && w->mode == WinMode::Fullscreen) {
      w->mode = w->before_fullscreen;
    }
  }
  raise(id);
  focused_ = id;
}

void WindowManager::set_rect(WindowId id, const Rect& user, const Rect& work) {
  Window* w = find(id);
  if (w == nullptr || w->app == nullptr) return;
  w->user_rect = user;
  w->display_rect = clamp_to(w->user_rect, work, frame_min(*w->app));
}

void WindowManager::step(int delta) {
  const int n = static_cast<int>(stack_.size());
  if (n == 0) return;

  int start = n - 1;
  for (int i = 0; i < n; ++i) {
    if (stack_[static_cast<size_t>(i)]->id == focused_) {
      start = i;
      break;
    }
  }

  for (int k = 1; k <= n; ++k) {
    const int i = ((start + delta * k) % n + n) % n;
    Window* w = stack_[static_cast<size_t>(i)].get();
    if (w->mode == WinMode::Minimized) continue;
    // L'identifiant est relevé AVANT focus(), qui réordonne la pile sous
    // nos pieds.
    focus(w->id);
    return;
  }
}

void WindowManager::focus_next() { step(1); }
void WindowManager::focus_prev() { step(-1); }

Window* WindowManager::hit(int x, int y) {
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    Window* w = it->get();
    if (w->mode == WinMode::Minimized) continue;
    if (w->display_rect.contains(x, y)) return w;
  }
  return nullptr;
}

void WindowManager::set_mode(WindowId id, WinMode m, const Rect& work) {
  Window* w = find(id);
  if (w == nullptr) return;

  if (m == WinMode::Fullscreen && w->mode != WinMode::Fullscreen) {
    // Relevé AVANT la bascule, sinon un second passage en plein écran
    // écraserait l'état à retrouver par « plein écran ».
    w->before_fullscreen = w->mode;
    for (auto& other : stack_) {
      if (other->id != id && other->mode == WinMode::Fullscreen) {
        other->mode = other->before_fullscreen;
      }
    }
  }

  w->mode = m;
  switch (m) {
    case WinMode::Normal:
      w->display_rect = clamp_to(w->user_rect, work, frame_min(*w->app));
      break;
    case WinMode::Maximized:
    case WinMode::Fullscreen:
      // user_rect n'est JAMAIS touché : c'est lui qui rend le rétablissement
      // exact, à la cellule près.
      w->display_rect = work;
      break;
    case WinMode::Minimized:
      // La fenêtre n'est plus composée mais garde tout son état, géométrie
      // comprise.
      break;
  }
}

}  // namespace sshos
