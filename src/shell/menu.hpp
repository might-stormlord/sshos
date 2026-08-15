#pragma once

#include <string>
#include <vector>

#include "render/surface.hpp"
#include "render/theme.hpp"

namespace sshos {

struct MenuItem {
  std::string id;
  std::string label;
};

enum class MenuHit { None, Body, Search, Item };

struct MenuHitResult {
  MenuHit what = MenuHit::None;
  int index = -1;
};

// Le lanceur. Il ne fait rien lui-même : il rend un identifiant, et la
// session sait ce qu'il veut dire. C'est ce qui permet d'y mettre des
// entrées qui n'ouvrent aucune fenêtre -- déplacer le panneau, quitter.
class Menu {
 public:
  void open();

  // Ancré au CURSEUR plutôt qu'au bouton du panneau : c'est ce qu'attend un
  // clic droit, et c'est la sortie d'un bureau vide sans toucher au clavier.
  void open_at(int x, int y);

  void close();
  bool is_open() const { return open_; }

  void type(char32_t c);
  void backspace();
  void move(int delta);

  const std::vector<MenuItem>& visible() const { return shown_; }
  const MenuItem* selected() const;
  int selection() const { return sel_; }

  // La géométrie pure, sans effet de bord. layout() la mémorise pour que
  // draw() et hit() lisent la MÊME chose -- la discipline du panneau et des
  // décorations de fenêtre.
  Rect rect(int cols, int rows) const;
  void layout(int cols, int rows);
  void draw(View v, const Theme& th, Border b) const;
  MenuHitResult hit(int x, int y) const;

 private:
  void refilter();

  bool open_ = false;
  bool anchored_ = false;
  int anchor_x_ = 0;
  int anchor_y_ = 0;
  std::string query_;
  std::vector<MenuItem> all_;
  std::vector<MenuItem> shown_;
  int sel_ = 0;
  Rect rect_{};
};

}  // namespace sshos
