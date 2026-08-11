#pragma once

#include <string>

#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/window.hpp"

namespace sshos {

enum class ModalHit { None, Body, Cancel, Confirm };

// Le dialogue de confirmation. Une seule question à la fois : empiler des
// dialogues sur un bureau texte ne mène nulle part, et l'utilisateur ne
// saurait plus auquel il répond.
class Modal {
 public:
  void ask(std::string question, WindowId target);
  void dismiss();

  bool is_open() const { return open_; }
  WindowId target() const { return target_; }
  const std::string& question() const { return question_; }

  // Annuler a le focus par défaut : la réponse sûre à une question
  // destructrice ne doit jamais être celle qu'on donne par inadvertance,
  // d'un Entrée réflexe.
  bool confirm_focused() const { return confirm_; }
  void focus_next() { confirm_ = !confirm_; }

  Rect rect(int cols, int rows) const;
  void layout(int cols, int rows);
  void draw(View v, const Theme& th, Border b) const;
  ModalHit hit(int x, int y) const;

 private:
  Rect cancel_rect() const;
  Rect confirm_rect() const;

  bool open_ = false;
  std::string question_;
  WindowId target_ = 0;
  bool confirm_ = false;
  Rect rect_{};
};

}  // namespace sshos
