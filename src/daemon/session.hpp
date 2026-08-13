#pragma once

#include <chrono>
#include <string_view>
#include <variant>

#include "common/platform.hpp"
#include "daemon/host.hpp"
#include "daemon/reap.hpp"
#include "input/events.hpp"
#include "input/shortcuts.hpp"
#include "render/profile.hpp"
#include "render/surface.hpp"
#include "render/theme.hpp"
#include "shell/clock.hpp"
#include "shell/help.hpp"
#include "shell/menu.hpp"
#include "shell/modal.hpp"
#include "shell/panel.hpp"
#include "wm/hittest.hpp"
#include "wm/layout.hpp"
#include "wm/manager.hpp"
#include "wm/window.hpp"

namespace sshos {

class Session : public ChildSink {
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

  // Un enfant est mort. Le pid est cherché dans la table ; s'il n'y est
  // pas -- enfant d'une fenêtre déjà fermée, ou pid qui ne nous appartient
  // pas -- il n'y a rien à faire, et surtout rien à signaler : c'est un
  // cas NORMAL.
  void on_child_exit(pid_t pid, int status) override;

  // Diagnostics réservés aux tests, comme `paste_scan_bytes_for_tests()`
  // dans `input/parser.hpp` : l'acheminement d'une mort d'enfant n'a AUCUN
  // effet observable de l'extérieur -- il appelle une méthode d'une
  // application que la session ne connaît pas -- et le seul moyen de le
  // vérifier sans monter un démon entier est de tendre la fenêtre. Aucun
  // code de production ne doit les lire.
  // CE QUI S'OUVRE AU PREMIER CONTACT. Un Terminal en production -- c'est
  // la raison d'etre du bureau. Le binaire de test y substitue UNE FOIS sa
  // propre fabrique : sans quoi chaque cas de session lancerait un vrai
  // shell, ce qui est lent, salissant, et rend illisible toute reference
  // de rendu -- et surtout, la moitie de ces cas lisent a l'ecran l'etat
  // d'une application factice qu'aucune vraie n'imite.
  using AppFactory = std::unique_ptr<App> (*)();
  static void set_seed_factory_for_tests(AppFactory make);

  Window* window_for_tests(WindowId id);
  const std::vector<std::unique_ptr<Window>>& windows_for_tests() const {
    return wm_.stack();
  }
  size_t watched_children_for_tests() const { return children_.size(); }
  void close_window_for_tests(WindowId id);

  // Relève -- et consomme -- la demande de repeint que la session a pu
  // former sans qu'aucune touche n'ait été frappée. Le démon l'interroge
  // une fois par tour. Elle relit l'horloge au passage : c'est ce qui rend
  // le canal utile, puisque render() n'est appelée que sur une frame déjà
  // sale et ne pourrait donc jamais se salir elle-même.
  bool take_dirty();

  // Octets à écrire tels quels sur le terminal du client, devant la trame
  // suivante. La bascule souris n'a pas de message de protocole : elle
  // voyage ici. Consommée une seule fois.
  std::string take_out_of_band();

  // Le client doit-il tout repeindre ? Consommé une seule fois.
  bool take_repaint();

  // Le client doit-il être congédié ? La session, elle, continue de vivre
  // dans le démon avec toutes ses fenêtres -- c'est toute la différence
  // avec wants_quit(), qui arrête le démon. Consommé une seule fois.
  bool take_detach();

  // Millisecondes avant que l'aide ne doive s'ouvrir toute seule : -1 si
  // rien ne l'attend, 0 si c'est dû maintenant. Le démon la replie dans le
  // délai de son epoll_wait, exactement comme il le fait déjà pour le
  // chien de garde de glissement -- sans quoi l'aide n'apparaîtrait qu'à la
  // frappe suivante, c'est-à-dire jamais, puisqu'elle existe précisément
  // pour celui qui ne sait pas quoi taper.
  int help_delay_ms() const;

  // Millisecondes avant qu'une application VISIBLE ne demande a etre
  // redessinee, ou -1 si aucune n'en demande. Meme mecanique que
  // help_delay_ms() : le demon la replie dans le delai de son epoll_wait,
  // et marque la trame sale a l'echeance.
  //
  // C'EST ICI que se joue « un moniteur minimise ne consomme rien » : une
  // fenetre Minimized n'entre pas dans le calcul, donc son delai ne
  // reveille personne et son dessin n'a jamais lieu.
  int refresh_delay_ms() const;

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
  // Range les fenêtres visibles pour qu'elles remplissent la zone de
  // travail sans se chevaucher.
  void tile_windows();

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
  void draw_empty_hint(View v, const Rect& work) const;

  // Ferme une fenêtre en retirant d'abord ses surveillances : aucune entrée
  // epoll ne doit survivre au descripteur qu'elle désigne.
  void close_window(Window& w);


  // Le chemin de fermeture, dans cet ordre : annuler le glissement, poser
  // la question à l'application, fermer ou ouvrir le dialogue. Tous les
  // chemins de fermeture passent ici -- bouton, raccourci, demande de
  // l'application elle-même.
  void request_close(Window& w);
  // Répond au dialogue : ferme la cible si l'on a confirmé, referme dans
  // tous les cas.
  void answer_modal(bool confirmed);

  // Exécute une action de la table de raccourcis sur la fenêtre focalisée.
  void do_action(Action a);
  // Exécute une entrée du menu, désignée par son identifiant.
  void run_menu(const std::string& id);
  // Les frappes vont au menu tant qu'il est ouvert, jamais à l'application.
  void menu_key(const KeyEvent& k);
  void focus_or_open(const std::string& app_id);

  // « Ctrl+A » tel qu'il se tape aujourd'hui, pour l'en-tête de l'aide, et
  // « ^A = aide » pour le rappel du panneau. Tous deux dérivés de la touche
  // configurée, jamais écrits en dur.
  std::string leader_label() const;
  std::string panel_hint() const;
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
  // Les enfants surveillés, tous fenêtres confondues. La récolte est
  // globale au processus : le démon apprend qu'un pid est mort sans savoir
  // à qui il était, et c'est ici qu'on le retrouve.
  std::vector<ChildWatch> children_;

  // Le panneau porte son bord et son épaisseur ; la zone de travail s'en
  // déduit. Ancré en bas par défaut : les trois autres bords marchent, mais
  // rien ne les choisit encore -- le réglage arrive avec la configuration.
  Panel panel_;
  Clock clock_;
  Menu menu_;
  Modal modal_;
  Help help_;
  LeaderDispatch leader_;

  // Instant où le leader a armé. Sert au seul compte à rebours de l'aide ;
  // le dispatcheur, lui, reste sans horloge.
  std::chrono::steady_clock::time_point leader_stamp_{};

  // Fin de la série en cours. Relue paresseusement, à la frappe suivante :
  // une série qui expire sans que personne ne tape n'a rien à réveiller, et
  // le démon n'a donc pas à raccourcir son sommeil pour elle.
  std::chrono::steady_clock::time_point repeat_until_{};

  // Une seule fenêtre d'amorce, au tout premier rendu. Un bureau VIDE est
  // un état légitime : le rendu rouvrait une application à chaque trame dès
  // que la pile se vidait, si bien que le [×] de la dernière fenêtre
  // semblait ne rien faire. Signalé à l'usage.
  bool seeded_ = false;

  // Le dernier clic, pour reconnaître un double. La session est le seul
  // endroit qui voie les deux appuis : le hit-testing est sans mémoire.
  std::chrono::steady_clock::time_point last_click_{};
  int last_click_x_ = -1;
  int last_click_y_ = -1;

  // Les modes souris DEC sont posés par le client à l'attache ; la bascule
  // les retire et les remet. TtyGuard les restaure à la sortie, donc une
  // souris laissée coupée ne survit pas à la session.
  bool mouse_on_ = true;
  std::string out_of_band_;
  bool repaint_ = false;
  bool detach_ = false;
  // La modale sert à deux choses : fermer une fenêtre, ou fermer la
  // SESSION. Sans ce drapeau, la réponse « oui » ne saurait pas à quoi
  // elle répond.
  bool modal_quits_session_ = false;

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
