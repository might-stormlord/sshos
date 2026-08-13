#pragma once

// LES DEUX APPLICATIONS FACTICES, SORTIES DU PRODUIT.
//
// `Bloc` et `Battement` etaient au catalogue depuis les jalons 1 et 2 :
// l'une montrait qu'une fenetre pouvait dessiner et compter des clics,
// l'autre qu'une application pouvait tenir un descripteur et etre
// reveillee par l'epoll. Les quatre vraies applications les ont
// remplacees, et les garder au menu offrait a l'utilisateur deux entrees
// qui ne servent a rien.
//
// Elles restent ICI parce que la suite en a besoin : une bonne moitie des
// cas du gestionnaire de fenetres, du panneau, de l'hote et de la session
// ont besoin d'une application QUELCONQUE -- deterministe, sans PTY
// dessous, et dont on peut lire l'etat a l'ecran. Les faire disparaitre
// tout a fait aurait demande d'en reecrire une, en moins bien.

#include <cstdint>
#include <string>
#include <string_view>

#include "app/app.hpp"

namespace sshos {



// Application factice sans descripteur. Aucune utilité pour
// l'utilisateur : son rôle est d'exercer chaque méthode du contrat pour
// que les tests aient prise dessus. Elle reste au catalogue après le
// jalon 3 -- c'est le seul moyen de garder ces chemins vivants une fois
// que de vraies applications existeront.
class Bloc : public App {
 public:
  void attach(Host& host) override;
  void render(View v) override;
  void on_key(const KeyEvent& k) override;
  void on_mouse(const MouseEvent& m) override;
  void on_resize(Size s) override;
  bool wants_cursor(Pos& out) const override;
  Size min_size() const override { return {14, 3}; }
  CloseCheck can_close() const override;

  // Relevés pour les tests. Le compteur de redimensionnements est la preuve
  // qu'un geste entier n'en produit qu'un seul (tâche 5).
  int resize_count() const { return resizes_; }
  int click_count() const { return clicks_; }

 private:
  void clamp_cursor();

  Host* host_ = nullptr;
  Size size_{0, 0};
  Pos cursor_{0, 0};
  int resizes_ = 0;
  int clicks_ = 0;
  bool modified_ = false;
};






// Application factice AVEC descripteur. Elle ouvre un tuyau sur elle-même,
// en surveille l'extrémité de lecture et compte ce qui arrive. Aucune
// utilité pour l'utilisateur : son rôle est d'exercer le chemin
// watch/on_io/unwatch de bout en bout, celui qu'aucune application sans fd
// ne peut atteindre.
class Battement : public App {
 public:
  ~Battement() override;

  void attach(Host& host) override;
  void render(View v) override;
  IoStatus on_io(uint64_t token, uint32_t events) override;
  void on_command(std::string_view cmd) override;
  Size min_size() const override { return {16, 2}; }

  // Écrit un octet dans le tuyau. Appelée par les tests et par le menu.
  void beat();

  // Ferme l'extrémité d'écriture : le prochain réveil portera un EPOLLHUP.
  void cut_source();

  int beats() const { return beats_; }
  bool source_alive() const { return read_fd_ >= 0; }

 private:
  void close_pipe();

  Host* host_ = nullptr;
  uint64_t token_ = 0;
  bool watching_ = false;
  int read_fd_ = -1;
  int write_fd_ = -1;
  int beats_ = 0;
};


}  // namespace sshos
