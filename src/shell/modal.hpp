#pragma once

#include <string>

#include "render/surface.hpp"
#include "render/theme.hpp"
#include "wm/window.hpp"

namespace sshos {

enum class ModalHit { None, Body, Cancel, Confirm };

// Ce que la boite propose.
//
//   Question : deux boutons, et Annuler a le focus.
//   Info     : un seul bouton -- il n'y a rien a decider.
//   Progress : AUCUN bouton. Un travail court ; la boite reste sous les yeux
//              et son corps se met a jour. Echap la referme, le travail
//              continue -- il tourne dans un processus a part.
enum class ModalStyle { Question, Info, Progress };

// Le dialogue de confirmation. Une seule question à la fois : empiler des
// dialogues sur un bureau texte ne mène nulle part, et l'utilisateur ne
// saurait plus auquel il répond.
//
// Le corps accepte des RETOURS À LA LIGNE : la boîte grandit d'autant et les
// boutons descendent avec. Une mise à jour a plusieurs choses à dire -- ce
// qu'elle apporte, d'où elle vient, ce qu'elle coûte -- et les serrer sur une
// seule ligne les rend illisibles.
class Modal {
 public:
  void ask(std::string question, WindowId target);

  // Les memes deux boutons, mais nommes. « Plus tard / Redemarrer » dit ce
  // qui va se passer ; « Annuler / Confirmer » oblige a le deviner.
  void ask(std::string question, WindowId target, std::string cancel_label,
           std::string confirm_label);

  // Un travail en cours. Aucun bouton, et le corps se remplace au fil des
  // etapes par set_body().
  void progress(std::string message);

  // OU EN EST LE TRAVAIL, EN POUR CENT. -1 -- le defaut -- veut dire qu'on
  // ne sait pas, et alors AUCUNE barre n'est dessinee : une barre a zero
  // laisserait croire qu'il ne se passe rien, ce qui est pire que de ne
  // rien montrer. Sans effet hors d'une progression : une question n'a rien
  // a mesurer, et un chiffre pose la se lirait comme un travail en cours.
  void set_progress(int percent) { percent_ = percent; }

  // Remplace le corps d'une boite DEJA ouverte. C'est ce qui permet a une
  // fenetre de progression de rester la pendant qu'elle raconte ce qui se
  // passe -- sans elle il faudrait la fermer et la rouvrir, ce qui la ferait
  // clignoter.
  void set_body(std::string message);


  // UNE RÉPONSE, PAS UNE QUESTION. L'utilisateur a demandé quelque chose --
  // vérifier les mises à jour, par exemple -- et il attend qu'on lui dise ce
  // qu'il en est. Un seul bouton, parce qu'il n'y a rien à décider : lui
  // montrer « Annuler / Confirmer » lui ferait chercher ce qu'il annule.
  void inform(std::string message);

  void dismiss();

  bool is_open() const { return open_; }
  WindowId target() const { return target_; }
  const std::string& question() const { return question_; }

  // Annuler a le focus par défaut : la réponse sûre à une question
  // destructrice ne doit jamais être celle qu'on donne par inadvertance,
  // d'un Entrée réflexe.
  bool confirm_focused() const { return confirm_; }
  // Sans effet quand il n'y a pas deux boutons a parcourir.
  void focus_next() {
    if (style_ == ModalStyle::Question) confirm_ = !confirm_;
  }

  Rect rect(int cols, int rows) const;
  void layout(int cols, int rows);
  void draw(View v, const Theme& th, Border b) const;
  ModalHit hit(int x, int y) const;

 private:
  // La largeur minimale du cadre, calculee sur les libelles REELS des deux
  // boutons -- pas sur « Annuler / Confirmer ».
  int min_width() const;
  int buttons_y() const;
  Rect cancel_rect() const;
  Rect confirm_rect() const;

  bool open_ = false;
  ModalStyle style_ = ModalStyle::Question;
  std::string cancel_label_;
  std::string confirm_label_;
  std::string question_;
  WindowId target_ = 0;
  bool confirm_ = false;
  int percent_ = -1;
  Rect rect_{};
};

}  // namespace sshos
