#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <utility>
#include <memory>
#include <vector>

#include "app/app.hpp"
#include "daemon/config.hpp"
#include "wm/window.hpp"

namespace sshos {

// Couture entre la session et l'epoll du démon. La session ne voit jamais
// le descripteur de l'epoll ; le démon ne sait jamais ce qu'une clé
// désigne.
struct FdRegistrar {
  virtual ~FdRegistrar() = default;
  virtual void watch(uint64_t key, int fd, uint32_t events) = 0;
  // Toujours appelée AVANT close(). L'ordre inverse laisse une entrée sur
  // un numéro que le noyau peut réattribuer aussitôt.
  virtual void unwatch(int fd) = 0;
};

// Objet nul, pas échafaudage : il donne un registrar valide aux cas
// unitaires de Session, qui n'ont pas d'epoll et n'en veulent pas.
struct NullFdRegistrar : FdRegistrar {
  void watch(uint64_t, int, uint32_t) override {}
  void unwatch(int) override {}
};

// epoll_event.data est une UNION : impossible d'y ranger un descripteur
// pour les fds du démon et autre chose pour ceux des applications. Tout
// passe donc par un u64, découpé en (fenêtre, génération). La génération
// est ce qui distingue deux surveillances successives d'un même numéro de
// descripteur -- le noyau recycle les numéros libres immédiatement.
constexpr uint64_t make_key(WindowId win, uint32_t gen) {
  return (static_cast<uint64_t>(win) << 32) | gen;
}
constexpr WindowId key_window(uint64_t key) {
  return static_cast<WindowId>(key >> 32);
}

// Générations réservées au démon lui-même (window_id == 0). Les trois
// premiers descripteurs vivent aussi longtemps que le processus ; les
// suivants (client, connexion en attente) prennent une génération issue
// d'un compteur qui démarre ici.
inline constexpr uint32_t kGenListener = 1;
inline constexpr uint32_t kGenTimer = 2;
inline constexpr uint32_t kGenSignal = 3;
inline constexpr uint32_t kGenFirstDynamic = 16;

// Un enfant surveillé, et la fenêtre à prévenir quand il meurt. La table
// vit dans la Session parce que la récolte est GLOBALE au processus : le
// démon reçoit un `SIGCHLD` qui ne dit pas de qui, récolte tout, et doit
// pouvoir retrouver à qui appartenait chaque pid.
struct ChildWatch {
  pid_t pid = -1;
  WindowId win = 0;
};

// Une application qu'une AUTRE application a demandé d'ouvrir. Elle
// attend là que la session la prenne : `HostImpl` ne connaît ni le
// gestionnaire de fenêtres ni la zone de travail, et ouvrir une fenêtre au
// milieu du traitement d'un clic ferait bouger la pile sous les pieds de
// celui qui l'a demandé.
struct PendingApp {
  std::unique_ptr<App> app;
  std::string app_id;
};

// Le seul objet qui traverse la frontière entre une application et le
// bureau. Il tient une référence sur SA fenêtre, dont l'adresse est stable
// parce que les fenêtres vivent derrière des unique_ptr, jamais dans un
// vector d'objets déplaçables.
class HostImpl : public Host {
 public:
  HostImpl(Window& win, FdRegistrar& fds, uint32_t& gen, bool& dirty,
           std::vector<ChildWatch>& children,
           std::vector<PendingApp>& pending, Settings& settings)
      : win_(&win),
        fds_(&fds),
        gen_(&gen),
        dirty_(&dirty),
        children_(&children),
        pending_(&pending),
        settings_(&settings) {}

  void set_title(std::string title) override;
  void request_close() override;
  void invalidate() override;

  uint64_t watch(int fd, uint32_t events) override;
  void unwatch(uint64_t token) override;
  void watch_child(pid_t pid) override;
  void open_app(std::unique_ptr<App> app, std::string app_id) override;

  // Les reglages traversent ici, et seulement ici : ils vivent dans la
  // session, un exemplaire pour tout le demon.
  std::string start_dir() const override;
  std::string configured_start_dir() const override;
  void set_start_dir(std::string dir) override;

  // Cette clé désigne-t-elle encore une surveillance vivante ? Répondre non
  // est tout l'intérêt des générations : un événement livré par epoll pour
  // une surveillance déjà retirée porte une clé que plus personne ne
  // reconnaît, et se jette au lieu d'être servi à l'application comme s'il
  // était le sien.
  bool owns(uint64_t key) const;

  // Achemine un événement vers l'application. Une clé périmée est jetée
  // sans bruit -- c'est un cas NORMAL, pas une anomalie.
  IoStatus deliver(uint64_t key, uint32_t events);

  // Appelée avant de détruire une fenêtre encore surveillée : les entrées
  // epoll doivent partir avant les descripteurs qu'elles désignent.
  void unwatch_all();

 private:
  Window* win_;
  FdRegistrar* fds_;
  uint32_t* gen_;
  bool* dirty_;
  std::vector<ChildWatch>* children_;
  std::vector<PendingApp>* pending_;
  Settings* settings_;
  std::vector<std::pair<uint64_t, int>> watched_;
};

}  // namespace sshos
