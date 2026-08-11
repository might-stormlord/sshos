#pragma once

#include "wm/manager.hpp"
#include "wm/window.hpp"

namespace sshos {

enum class PanelEdge { Top, Bottom, Left, Right };

// Ce qui reste de l'écran une fois le panneau retiré. Les fenêtres n'ont
// pas le droit d'en sortir -- c'est ce qui garantit que la barre des tâches
// reste atteignable quoi qu'il arrive.
Rect work_area(int cols, int rows, PanelEdge edge, int thickness);

// La projection d'une fenêtre selon son mode. Ne modifie rien : c'est une
// fonction pure, et c'est ce qui rend le redimensionnement réversible.
Rect display_rect_for(const Window& w, const Rect& work, int cols, int rows);

void relayout(WindowManager& wm, const Rect& work, int cols, int rows);

}  // namespace sshos
