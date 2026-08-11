#pragma once

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

}  // namespace sshos
