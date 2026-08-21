#pragma once

#include "render/cell.hpp"
#include "render/profile.hpp"

namespace sshos {

// Palette nommée du bureau. Chaque champ porte un rôle, pas une teinte :
// c'est ce qui permet d'en écrire trois versions sans que le reste du code
// ne sache laquelle il utilise.
struct Theme {
  Color desktop_bg;
  // La signature « TERMOS » du fond. PROCHE du fond, pas lisible de loin :
  // une signature qui se lit aussi bien que le contenu detourne l'oeil de
  // ce qu'on est venu faire.
  Color desktop_sign;
  Color panel_bg;
  Color panel_fg;
  Color accent;
  Color title_focus_bg;
  Color title_focus_fg;
  Color title_blur_bg;
  Color title_blur_fg;
  Color border_focus;
  Color border_blur;
  Color modal_bg;
  Color modal_fg;

  // Trois palettes écrites à la main, et non deux dérivées de la première.
  // En 16 couleurs, quantize_color() ramène tout canal sous 128 à zéro :
  // fond de bureau, fond de panneau et titre inactif tomberaient tous les
  // trois sur le même noir, et la structure de l'interface disparaîtrait
  // exactement sur les terminaux les plus pauvres. Le test
  // theme_keeps_every_meaningful_distinction_on_every_depth garde ces trois
  // palettes honnêtes.
  static Theme defaults();  // 24 bits
  static Theme indexed256();
  static Theme mono16();

  Theme for_profile(const OutputProfile& p) const;
};

}  // namespace sshos
