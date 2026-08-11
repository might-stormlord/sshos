#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "wm/window.hpp"

namespace sshos {

// Aimantation : un bord à moins de `tolerance` cellules d'un bord de la
// zone de travail s'y colle. Sans elle, « presque aligné » est le résultat
// le plus probable de tout glissement, et il se voit.
Rect snap(Rect r, const Rect& work, int tolerance);

class WindowManager {
 public:
  static constexpr size_t kMaxWindows = 64;

  // Rend nullptr si le plafond est atteint. N'attache PAS l'hôte : seul
  // l'appelant sait le construire, et il doit le faire avant d'appeler
  // app->attach().
  Window* open(std::unique_ptr<App> app, const Rect& work);
  bool close(WindowId id);

  Window* find(WindowId id);
  const Window* find(WindowId id) const;

  // Index 0 = arrière-plan, back() = premier plan.
  const std::vector<std::unique_ptr<Window>>& stack() const { return stack_; }

  void raise(WindowId id);
  void focus(WindowId id);
  WindowId focused() const { return focused_; }
  void focus_next();
  void focus_prev();

  Window* hit(int x, int y);
  void set_mode(WindowId id, WinMode m, const Rect& work);

 private:
  void step(int delta);

  // unique_ptr et non Window : chaque HostImpl tient un pointeur vers SA
  // fenêtre, et un vector d'objets les invaliderait tous à chaque
  // réallocation comme à chaque raise().
  std::vector<std::unique_ptr<Window>> stack_;
  WindowId next_id_ = 1;
  WindowId focused_ = 0;
  int cascade_ = 0;
};

}  // namespace sshos
