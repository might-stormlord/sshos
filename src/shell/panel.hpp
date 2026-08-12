#pragma once

#include <string>
#include <vector>

#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/layout.hpp"
#include "wm/manager.hpp"

namespace sshos {

enum class PanelHit { None, Body, MenuButton, Pinned, Task, Overflow, Clock, Hint };

struct PanelHitResult {
  PanelHit what = PanelHit::None;
  int index = -1;   // rang de l'épinglée, de la tâche, ou nombre replié
  WindowId win = 0;  // renseigné pour Task uniquement
};

// Une entrée de barre : UNE application, ouverte ou non. Épingler et lancer
// ne produisent pas deux entrées mais une seule, qui change d'état -- c'est
// ce que font les barres modernes, et c'est ce qui empêche « Bloc » de
// figurer deux fois à trente centimètres d'écart dès qu'on le lance.
struct PanelEntry {
  int catalog_index = -1;  // -1 : fenêtre sans entrée au catalogue
  std::string label;
  // La fenêtre qu'un clic active. Zéro quand rien n'est ouvert : le clic
  // lance alors l'application. Quand plusieurs fenêtres partagent l'entrée,
  // c'est la SUIVANTE après celle qui a la main -- cliquer plusieurs fois
  // fait donc le tour du groupe.
  WindowId target = 0;
  int count = 0;
  bool focused = false;    // l'une d'elles a la main
  bool minimized = false;  // toutes sont réduites
};

// La barre des tâches. Elle calcule sa disposition UNE fois, dans layout(),
// et draw() comme hit() lisent cette même liste : c'est la seule façon de
// garantir que ce qu'on clique est ce qu'on voit. La même discipline que
// pour les décorations de fenêtre (wm/decor.hpp).
class Panel {
 public:
  void set_edge(PanelEdge e) { edge_ = e; }
  PanelEdge edge() const { return edge_; }

  // Le rappel de la touche leader, posé juste avant l'horloge. Il cède la
  // place dès que la barre des tâches en a besoin : c'est un rappel, pas
  // une information, et un bureau chargé appartient à quelqu'un qui n'en a
  // plus besoin. À l'inverse, un bureau vide -- l'état du débutant -- a
  // toute la place du monde et le montre donc toujours.
  void set_hint(std::string h) { hint_ = std::move(h); }

  // Une ligne sur un bord horizontal, seize colonnes sur un bord vertical :
  // une entrée de tâche ne tient pas dans moins.
  int thickness() const;

  Rect rect(int cols, int rows) const;

  void layout(const WindowManager& wm, int cols, int rows, bool utf8);
  void draw(View v, const Theme& th, const std::string& clock_text,
            const std::string& date_text = std::string()) const;
  PanelHitResult hit(int x, int y) const;

 private:
  struct Item {
    PanelHit what = PanelHit::Body;
    int index = -1;
    WindowId win = 0;
    bool focused = false;
    Rect r{};
    std::string text;
  };

  bool horizontal() const {
    return edge_ == PanelEdge::Top || edge_ == PanelEdge::Bottom;
  }
  void layout_horizontal(const WindowManager& wm);
  void layout_vertical(const WindowManager& wm);

  std::vector<PanelEntry> build_entries(const WindowManager& wm) const;
  std::string entry_text(const PanelEntry& e, int label_cells) const;

  PanelEdge edge_ = PanelEdge::Bottom;
  Rect rect_{};
  bool utf8_ = false;
  std::string hint_;
  std::vector<Item> items_;
};

}  // namespace sshos
