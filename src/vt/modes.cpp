#include "vt/modes.hpp"

namespace sshos {
namespace {

// LA table des modes. Une seule, partagée par `set`, `knows` et `get`.
// Trois aiguillages séparés se contrediraient le jour où un mode s'ajoute,
// et `DECRQM` répondrait « je ne connais pas » sur un mode qui marche.
struct ModeEntry {
  int code;
  bool Modes::*flag;
};

constexpr ModeEntry kModes[] = {
    {1, &Modes::cursor_keys_application},  // DECCKM
    {7, &Modes::autowrap},                 // DECAWM
    {25, &Modes::cursor_visible},          // DECTCEM
    {1000, &Modes::mouse_click},
    {1002, &Modes::mouse_drag},
    {1003, &Modes::mouse_any},
    {1006, &Modes::mouse_sgr},
    {1049, &Modes::alt_screen},
    {2004, &Modes::bracketed_paste},
};

const ModeEntry* find(int mode) {
  for (const ModeEntry& e : kModes) {
    if (e.code == mode) return &e;
  }
  return nullptr;
}

}  // namespace

MouseTracking Modes::tracking() const {
  // Du plus permissif au moins : les trois drapeaux sont indépendants, et
  // une application qui pose 1002 puis 1003 attend le mouvement à vide.
  if (mouse_any) return MouseTracking::Any;
  if (mouse_drag) return MouseTracking::Drag;
  if (mouse_click) return MouseTracking::Click;
  return MouseTracking::None;
}

void Modes::set(int mode, bool on) {
  // Un mode inconnu est ignoré en silence, comme le fait tout terminal :
  // une application qui demande un mode que nous n'avons pas doit pouvoir
  // continuer, pas se voir refuser sa séquence.
  if (const ModeEntry* e = find(mode)) this->*(e->flag) = on;
}

bool Modes::knows(int mode) const { return find(mode) != nullptr; }

bool Modes::get(int mode) const {
  const ModeEntry* e = find(mode);
  return e != nullptr && this->*(e->flag);
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
