#pragma once

#include "common/platform.hpp"
#include "input/events.hpp"
#include "render/surface.hpp"

namespace sshos {

// Bouchon du jalon 1 : un panneau, une boîte à bordure, une horloge. Sa
// seule raison d'être est de prouver que la chaîne complète fonctionne.
// Le jalon 2 remplace cette classe par le vrai gestionnaire de fenêtres.
class Session {
 public:
  Session(Platform& plat, int cols, int rows);

  // A3 : cols_/rows_ n'existent plus (état mort — resize() les écrivait,
  // render() ne les a jamais lus, il dérive toute sa géométrie de la
  // Surface qu'on lui passe). Ce point d'entrée reste dans l'interface
  // publique sans corps utile : le démon l'appelle déjà à chaque Hello /
  // Resize, et le jalon 2 (vraies fenêtres, redimensionnement au contour)
  // en aura besoin. Paramètres sans nom : sinon -Wunused-parameter sous
  // -Werror, puisque rien ici ne les lit.
  void resize(int, int);
  void on_input(const InputEvent& e);
  void render(Surface& out);
  bool wants_quit() const { return quit_; }

 private:
  Platform* plat_;
  bool quit_ = false;
  int clicks_ = 0;
};

}  // namespace sshos
