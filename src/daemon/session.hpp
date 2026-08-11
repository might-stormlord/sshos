#pragma once

#include <chrono>
#include <memory>
#include <variant>

#include "common/platform.hpp"
#include "input/events.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/hittest.hpp"
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

  WinHitResult hit_window_at(int x, int y) const;

  // Appelée par le démon au détachement d'un client et à l'attache du
  // suivant. Un glissement engagé n'a plus de sens quand la souris qui le
  // tenait a disparu.
  //
  // Annuler RESTAURE : la fenêtre revient là où le geste l'avait prise.
  // C'est ce que « annulation » veut dire, et c'est ce que fait Échap dans
  // tout gestionnaire de fenêtres. Le seul chemin qui garde la nouvelle
  // position est le relâchement, qui passe par finish_drag().
  void cancel_drag();

 private:
  struct Idle {};
  struct Moving {
    WindowId win = 0;
    int grab_dx = 0;
    int grab_dy = 0;
    Rect origin{};  // à restaurer si le geste est annulé
  };
  struct Resizing {
    WindowId win = 0;
    Rect outline{};
  };
  using DragState = std::variant<Idle, Moving, Resizing>;

  Rect work_area(int cols, int rows) const;
  Border border() const;
  void ensure_window(const Rect& work);
  void draw_panel(View& v, int cols, int rows);
  void on_mouse(const MouseEvent& m);
  void watchdog();

  // Clôt le geste en gardant ce qu'il a produit -- l'inverse exact de
  // cancel_drag(), qui le défait.
  void finish_drag() { drag_ = Idle{}; }

  DragState drag_{Idle{}};
  std::chrono::steady_clock::time_point drag_stamp_{};

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
