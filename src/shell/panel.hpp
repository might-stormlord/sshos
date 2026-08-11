#pragma once

#include <string>
#include <vector>

#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/layout.hpp"
#include "wm/manager.hpp"

namespace sshos {

enum class PanelHit { None, Body, MenuButton, Pinned, Task, Overflow, Clock };

struct PanelHitResult {
  PanelHit what = PanelHit::None;
  int index = -1;   // rang de l'épinglée, de la tâche, ou nombre replié
  WindowId win = 0;  // renseigné pour Task uniquement
};

// La barre des tâches. Elle calcule sa disposition UNE fois, dans layout(),
// et draw() comme hit() lisent cette même liste : c'est la seule façon de
// garantir que ce qu'on clique est ce qu'on voit. La même discipline que
// pour les décorations de fenêtre (wm/decor.hpp).
class Panel {
 public:
  void set_edge(PanelEdge e) { edge_ = e; }
  PanelEdge edge() const { return edge_; }

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

  PanelEdge edge_ = PanelEdge::Bottom;
  Rect rect_{};
  bool utf8_ = false;
  std::vector<Item> items_;
};

}  // namespace sshos
