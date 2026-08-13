#include "vt/modes.hpp"

namespace sshos {

MouseTracking Modes::tracking() const {
  // Du plus permissif au moins : les trois drapeaux sont indépendants, et
  // une application qui pose 1002 puis 1003 attend le mouvement à vide.
  if (mouse_any) return MouseTracking::Any;
  if (mouse_drag) return MouseTracking::Drag;
  if (mouse_click) return MouseTracking::Click;
  return MouseTracking::None;
}

void Modes::set(int mode, bool on) {
  switch (mode) {
    case 1:
      cursor_keys_application = on;
      break;
    case 7:
      autowrap = on;
      break;
    case 25:
      cursor_visible = on;
      break;
    case 1000:
      mouse_click = on;
      break;
    case 1002:
      mouse_drag = on;
      break;
    case 1003:
      mouse_any = on;
      break;
    case 1006:
      mouse_sgr = on;
      break;
    case 1049:
      alt_screen = on;
      break;
    case 2004:
      bracketed_paste = on;
      break;
    default:
      // Ignoré en silence. Un invité qui demande un mode que nous n'avons
      // pas doit pouvoir continuer : refuser la séquence entière lui
      // ferait perdre les modes voisins, qui, eux, existent.
      break;
  }
}

void apply_dec_private(const Params& params, bool on, Modes& modes) {
  // Un paramètre ABSENT vaut -1, pas 0 -- `\033[?h` porte un paramètre
  // vide, pas zéro paramètre. Il n'est PAS filtré ici : aucun mode ne
  // porte un numéro négatif, `set()` le laisse donc tomber dans son
  // `default` comme n'importe quel inconnu. Une garde l'écartant plus tôt
  // a été écrite, puis retirée : la campagne de mutation l'a montrée
  // inobservable, et rien de prévu ne la rendrait porteuse.
  for (const Param& p : params) {
    modes.set(p.value, on);
  }
}

}  // namespace sshos
