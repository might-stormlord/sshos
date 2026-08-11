#pragma once

#include <chrono>
#include <string_view>
#include <variant>

#include "common/platform.hpp"
#include "daemon/host.hpp"
#include "input/events.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/clock.hpp"
#include "shell/panel.hpp"
#include "wm/hittest.hpp"
#include "wm/layout.hpp"
#include "wm/manager.hpp"
#include "wm/window.hpp"

namespace sshos {

class Session {
 public:
  // Le registrar est la seule chose que la session sache de l'epoll du
  // démon : elle lui tend des clés, il les rend telles quelles.
  Session(Platform& plat, FdRegistrar& fds, int cols, int rows);

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

  // Ouvre une application du catalogue. Rend 0 si l'identifiant est inconnu
  // ou si le plafond de fenêtres est atteint.
  WindowId open_from_catalog(std::string_view id);

  // Un événement sur un descripteur applicatif. Une clé que plus aucune
  // fenêtre ne reconnaît s'y jette sans bruit -- c'est le cas NORMAL d'un
  // réveil en retard sur une surveillance déjà retirée.
  void on_fd_event(uint64_t key, uint32_t events);

  // Relève -- et consomme -- la demande de repeint que la session a pu
  // former sans qu'aucune touche n'ait été frappée. Le démon l'interroge
  // une fois par tour. Elle relit l'horloge au passage : c'est ce qui rend
  // le canal utile, puisque render() n'est appelée que sur une frame déjà
  // sale et ne pourrait donc jamais se salir elle-même.
  bool take_dirty();

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

  Border border() const;
  void ensure_window(const Rect& work);

  // Ferme une fenêtre en retirant d'abord ses surveillances : aucune entrée
  // epoll ne doit survivre au descripteur qu'elle désigne.
  void close_window(Window& w);
  void on_mouse(const MouseEvent& m);
  void watchdog();

  // Clôt le geste en gardant ce qu'il a produit -- l'inverse exact de
  // cancel_drag(), qui le défait.
  void finish_drag() { drag_ = Idle{}; }

  DragState drag_{Idle{}};
  std::chrono::steady_clock::time_point drag_stamp_{};

  Platform* plat_;
  FdRegistrar* fds_;
  OutputProfile out_;
  Theme theme_;
  bool quit_ = false;

  WindowManager wm_;

  // Compteur de générations de la session, distinct de celui du démon : il
  // s'incrémente à chaque watch(), pas à chaque fenêtre, pour qu'une
  // application qui referme et rouvre son tuyau -- et récupère le même
  // numéro de descripteur du noyau -- obtienne malgré tout une clé neuve.
  uint32_t fd_gen_ = kGenFirstDynamic;

  // Le panneau porte son bord et son épaisseur ; la zone de travail s'en
  // déduit. Ancré en bas par défaut : les trois autres bords marchent, mais
  // rien ne les choisit encore -- le réglage arrive avec la configuration.
  Panel panel_;
  Clock clock_;

  // Repeint demandé hors de toute entrée utilisateur : l'horloge qui change
  // de minute, une application qui appelle Host::invalidate(). HostImpl en
  // tient l'adresse, d'où sa position parmi les membres -- il doit survivre
  // aux fenêtres, et les fenêtres vivent dans wm_, déclaré plus haut.
  bool dirty_ = false;

  // Dernière zone de travail composée. A3 tient toujours -- render() dérive
  // sa géométrie de la Surface qu'on lui passe et la réécrit ici -- mais le
  // traitement des clics en a besoin hors composition : maximiser une
  // fenêtre demande de connaître la zone, et on_mouse() ne voit aucune
  // Surface.
  Rect last_work_{0, 0, 80, 23};
};

}  // namespace sshos
