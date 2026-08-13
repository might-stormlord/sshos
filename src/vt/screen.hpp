#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/cell.hpp"

namespace sshos {

class Scrollback;

// Une cellule de la grille. `width` vaut 2 sur la première moitié d'un
// caractère pleine chasse et 0 sur la seconde, jamais 2 deux fois de suite
// -- c'est ce qui permet au rendu de sauter la moitié droite sans la
// deviner.
//
// Le style est celui du RENDU, pas un type VT parallèle : le pont vers la
// `View` (tâche 13) devient une copie de champ à champ. C'est le sens
// inverse du choix fait pour le texte, où `ScreenCell` ne partage rien avec
// `Cell` -- une cellule de grille et une cellule de rendu ont des
// invariants différents, une couleur n'en a qu'un.
struct ScreenCell {
  char32_t ch = U' ';
  uint8_t width = 1;
  Style style{};
  bool operator==(const ScreenCell&) const = default;
};

struct CursorPos {
  int x = 0;
  int y = 0;
};

// Ce que DECSC met de côté : la position, le retour différé et le STYLE
// COURANT. La tâche 10 y ajoutera le jeu de caractères, car DECRC restitue
// les trois -- une application qui sauve le curseur au milieu d'un passage
// en gras attend de le retrouver en gras.
struct SavedCursor {
  int x = 0;
  int y = 0;
  bool wrap_pending = false;
  Style style{};
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

  // Le STYLO : le style que `print()` dépose dans chaque cellule qu'il
  // écrit. C'est le seul état que SGR modifie, et il ne repeint jamais ce
  // qui est déjà là -- une couleur posée maintenant ne concerne que la
  // suite.
  const Style& pen() const { return pen_; }
  void set_pen(const Style& s) { pen_ = s; }

  // Le RETOUR AUTOMATIQUE (DECAWM, mode 7). Éteint, écrire dans la
  // dernière colonne y écrase le caractère précédent au lieu de descendre
  // -- c'est ce dont se sert une application qui dessine un cadre jusqu'au
  // bord droit sans vouloir faire défiler la page.
  bool autowrap() const { return autowrap_; }
  void set_autowrap(bool on) { autowrap_ = on; }

  // L'ÉCRAN ALTERNÉ (mode 1049). Entrer sauve le curseur ET le style, met
  // la page principale de côté et efface la nouvelle ; sortir rend les
  // trois. La page principale revient au caractère près : c'est ce qui
  // fait qu'un `vim` quitté rend le shell tel qu'on l'avait laissé.
  //
  // Les deux appels sont idempotents. Une application qui pose 1049 deux
  // fois -- ou un `tmux` imbriqué qui le pose alors qu'il l'est déjà --
  // ne doit pas perdre la page principale sous la seconde sauvegarde.
  void enter_alt_screen();
  void leave_alt_screen();
  // Lisible par le scrollback (tâche 7), qui ne doit RIEN recevoir de
  // l'écran alterné : le défilement de `vim` n'appartient pas à
  // l'historique du shell.
  bool alt_screen() const { return alt_; }

  // L'HISTORIQUE, s'il y en a un. Nul par défaut : une grille sans
  // historique se comporte exactement comme avant, et c'est ce qui permet
  // de la tester seule.
  //
  // Une SEULE chose l'alimente : la ligne qui sort par le haut d'un
  // défilement naturel, celui que provoque un saut de ligne au bas de la
  // page. Ni `IL`, ni `DL`, ni un défilement de région, ni l'écran
  // alterné -- voir `scroll_up()`.
  void set_scrollback(Scrollback* sb) { scrollback_ = sb; }

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

  // Les effacements. `mode` reprend la numérotation du fil : 0 efface du
  // curseur vers la fin, 1 du début jusqu'au curseur inclus, 2 la totalité.
  // Tout autre mode ne fait rien -- le mode 3 (scrollback) appartient à la
  // tâche 7 et sera traité au-dessus, pas ici.
  void erase_display(int mode);  // ED
  void erase_line(int mode);     // EL
  void erase_chars(int n);       // ECH : n cellules sur place, rien ne bouge

  // Les éditions. Toutes bornées, toutes sans effet hors de la région de
  // défilement pour celles qui travaillent en lignes.
  void insert_chars(int n);  // ICH : pousse la fin de ligne à droite
  void delete_chars(int n);  // DCH : ramène la fin de ligne à gauche
  void insert_lines(int n);  // IL
  void delete_lines(int n);  // DL

  // La région de défilement (DECSTBM), en lignes 0-indexées INCLUSES. Une
  // région invalide est refusée telle quelle : c'est ce que fait xterm, et
  // une application qui se trompe garde ainsi la région d'avant plutôt que
  // de voir son écran se figer.
  void set_scroll_region(int top, int bottom);
  void reset_scroll_region();  // DECSTBM sans paramètre : pleine page
  int scroll_top() const { return top_; }
  int scroll_bottom() const { return bottom_; }

  // DECSC / DECRC. Un DECRC sans DECSC préalable ramène à l'origine, ce qui
  // est le comportement d'un terminal fraîchement allumé.
  void save_cursor();
  void restore_cursor();

  // Le texte d'une ligne, en UTF-8, sans les blancs de fin. Pour les tests
  // et pour le scrollback, qui range des lignes ROGNÉES.
  std::string line_text(int y) const;

 private:
  // La cellule que laisse un EFFACEMENT : vide, mais peinte du FOND
  // courant. C'est le « background colour erase » du terminfo
  // `xterm-256color` qu'on promet à l'invité -- sans lui, un `clear` sur un
  // fond bleu rend un écran noir, et toute application qui compte dessus
  // pour peindre ses marges se retrouve en damier.
  //
  // Le fond SEUL : une cellule sans glyphe ne montre rien d'autre. Y
  // recopier le premier plan ou les attributs ferait souligner le vide.
  ScreenCell erased() const;

  ScreenCell& cell(int x, int y);
  void scroll_up();    // le haut s'en va, une ligne vierge en bas
  void scroll_down();  // le bas s'en va, une ligne vierge en haut
  void reset_tabs();

  // Le défilement d'une tranche quelconque. La région de DECSTBM n'est
  // qu'un cas d'usage : IL et DL défilent la tranche qui va du curseur au
  // bas de la région, et se ramènent ainsi aux mêmes deux fonctions.
  void scroll_slice_up(int top, int bottom, int n);
  void scroll_slice_down(int top, int bottom, int n);

  // Efface les colonnes [x0, x1] d'une ligne en emportant les caractères
  // pleine chasse que les bornes couperaient en deux.
  void erase_span(int x0, int x1, int y);

  // Efface LES DEUX moitiés du caractère pleine chasse qui occupe cette
  // colonne, quelle que soit la moitié visée. Ce que clear_wide_at() laisse
  // debout, celle-ci l'emporte : un décalage doit briser la paire avant de
  // la déplacer, sinon une moitié voyage sans l'autre.
  void break_wide_at(int x, int y);

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
  int top_ = 0;
  int bottom_ = 0;  // posé à rows_ - 1 par le constructeur
  bool autowrap_ = true;
  Style pen_{};
  SavedCursor saved_{};
  // La page principale mise de côté pendant que l'écran alterné est
  // actif, et le curseur qu'elle attend. Son emplacement de sauvegarde est
  // SÉPARÉ de celui de DECSC : une application qui sauve son curseur dans
  // l'écran alterné ne doit pas écraser celui qui l'attend dehors.
  bool alt_ = false;
  std::vector<ScreenCell> parked_;
  SavedCursor parked_cursor_{};
  // Le DECSC de la page principale, mis à l'abri pendant que l'écran
  // alterné a le sien. C'est ce que fait xterm, qui tient un emplacement
  // par tampon -- sans cela le `\0337` d'un `vim` écrase la position que
  // le shell avait sauvée, et son `\0338` d'après le ramène ailleurs.
  SavedCursor parked_decsc_{};
  Scrollback* scrollback_ = nullptr;
  // Ce que rend une lecture HORS grille. Rien à voir avec erased() : ce
  // n'est pas un effacement, c'est l'absence de cellule, et elle ne prend
  // donc jamais le fond courant.
  ScreenCell blank_{};
};

}  // namespace sshos
