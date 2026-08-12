#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sshos {

// Une cellule de la grille. Le style arrive à la tâche 5 (SGR) : ici, le
// texte seul. `width` vaut 2 sur la première moitié d'un caractère pleine
// chasse et 0 sur la seconde, jamais 2 deux fois de suite -- c'est ce qui
// permet au rendu de sauter la moitié droite sans la deviner.
struct ScreenCell {
  char32_t ch = U' ';
  uint8_t width = 1;
};

struct CursorPos {
  int x = 0;
  int y = 0;
};

// La grille et son curseur. Rien ici ne connaît le parseur : l'écran reçoit
// des ordres déjà décodés, ce qui le rend testable sans passer par un seul
// octet d'échappement.
class Screen {
 public:
  Screen(int cols, int rows);

  int cols() const { return cols_; }
  int rows() const { return rows_; }

  // Bornée : lire hors grille rend une cellule vide plutôt que de planter.
  const ScreenCell& at(int x, int y) const;
  CursorPos cursor() const { return {cx_, cy_}; }

  // Le retour à la ligne DIFFÉRÉ, exposé pour les tests : écrire dans la
  // dernière colonne ne descend pas, il pose ce drapeau.
  bool wrap_pending() const { return wrap_pending_; }

  void print(char32_t cp);

  // Les commandes de mouvement du C0.
  void line_feed();        // LF : descend, défile en bas
  void carriage_return();  // CR
  void backspace();        // BS
  void tab();              // HT

  // Les échappements simples.
  void index();          // IND : comme LF
  void reverse_index();  // RI : monte, défile en haut
  void next_line();      // NEL : CR + LF

  // Les CSI de position. Tous bornés, tous en coordonnées 0-indexées : la
  // conversion depuis le 1-indexé du fil se fait chez l'appelant, en un
  // seul endroit.
  void move_to(int x, int y);  // CUP
  void move_up(int n);         // CUU
  void move_down(int n);       // CUD
  void move_right(int n);      // CUF
  void move_left(int n);       // CUB
  void set_column(int x);      // CHA
  void set_row(int y);         // VPA

  // Les taquets de tabulation.
  void set_tab();         // HTS
  void clear_tab();       // TBC 0
  void clear_all_tabs();  // TBC 3

  // Le texte d'une ligne, en UTF-8, sans les blancs de fin. Pour les tests
  // et pour le scrollback, qui range des lignes ROGNÉES.
  std::string line_text(int y) const;

 private:
  ScreenCell& cell(int x, int y);
  void scroll_up();    // le haut s'en va, une ligne vierge en bas
  void scroll_down();  // le bas s'en va, une ligne vierge en haut
  void reset_tabs();

  // Efface la MOITIÉ ORPHELINE d'un caractère pleine chasse qu'on vient de
  // recouvrir : sans cela, écrire un caractère simple sur la gauche d'un
  // double laisse sa moitié droite en place, et le rendu affiche un demi
  // idéogramme.
  void clear_wide_at(int x, int y);

  int cols_;
  int rows_;
  std::vector<ScreenCell> grid_;
  std::vector<bool> tabs_;
  int cx_ = 0;
  int cy_ = 0;
  bool wrap_pending_ = false;
  ScreenCell blank_{};
};

}  // namespace sshos
