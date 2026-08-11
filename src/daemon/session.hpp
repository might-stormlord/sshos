#pragma once

#include <memory>

#include "common/platform.hpp"
#include "input/events.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/window.hpp"

namespace sshos {

class Session {
 public:
  Session(Platform& plat, int cols, int rows);

  // Appelée par le démon à chaque attache, là où il construit déjà le
  // profil pour le Differ. Détermine le thème ET le jeu de bordures : un
  // client sans UTF-8 reçoit des cadres ASCII, pas des points
  // d'interrogation.
  void set_output(const OutputProfile& p);

  // A3 : les arguments du constructeur et de resize() restent ignorés,
  // render() dérive TOUTE sa géométrie de la Surface qu'on lui passe. Ce
  // point d'entrée reste dans l'interface publique parce que le démon
  // l'appelle déjà à chaque Hello / Resize.
  void resize(int, int);
  void on_input(const InputEvent& e);
  void render(Surface& out);
  bool wants_quit() const { return quit_; }

 private:
  Rect work_area(int cols, int rows) const;
  Border border() const;
  void ensure_window(const Rect& work);
  void draw_panel(View& v, int cols, int rows);

  Platform* plat_;
  OutputProfile out_;
  Theme theme_;
  bool quit_ = false;

  // Une seule fenêtre jusqu'à la tâche 6, derrière un unique_ptr : son
  // adresse doit rester stable, HostImpl la référence.
  std::unique_ptr<Window> win_;
  WindowId next_id_ = 1;
};

}  // namespace sshos
