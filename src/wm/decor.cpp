#include "wm/decor.hpp"

#include <algorithm>

namespace sshos {
namespace {

// Trois cellules par bouton. Le titre a besoin d'au moins 3 colonnes pour
// rester lisible, avec 2 colonnes de marge à gauche, 1 de séparation avant
// les boutons et 1 de marge à droite : n boutons demandent donc 7 + 3n
// colonnes de cadre.
constexpr int kButtonWidth = 3;
constexpr int kTitleMargin = 2;

}  // namespace

DecorMetrics decor_metrics(const Rect& frame) {
  DecorMetrics m;
  m.title_bar = Rect{frame.x, frame.y, frame.w, 1};
  m.client = client_rect(frame);

  int n = 0;
  for (int candidate = 3; candidate >= 1; --candidate) {
    if (frame.w >= 7 + kButtonWidth * candidate) {
      n = candidate;
      break;
    }
  }
  m.button_count = n;

  const int bx = frame.x + frame.w - 1 - kButtonWidth * n;
  m.buttons = Rect{bx, frame.y, kButtonWidth * n, 1};

  const int title_x = frame.x + kTitleMargin;
  const int title_end = (n > 0) ? bx - 1 : frame.x + frame.w - 1;
  m.title_text = Rect{title_x, frame.y, std::max(0, title_end - title_x), 1};
  return m;
}

void draw_decor(View v, const Window& w, bool focused, const Theme& th, Border b) {
  const Rect f = w.display_rect;
  if (f.w <= 0 || f.h <= 0) return;

  const bool uni = (b == Border::Unicode);
  const DecorMetrics m = decor_metrics(f);

  // Une fenêtre est opaque : on remplit tout le cadre avant de dessiner,
  // sinon le bureau transparaîtrait partout où l'application ne peint pas.
  Style frame_st;
  frame_st.bg = th.panel_bg;
  frame_st.fg = focused ? th.border_focus : th.border_blur;
  v.fill(f, frame_st);
  v.box(f, b, frame_st);

  Style title_st;
  title_st.bg = focused ? th.title_focus_bg : th.title_blur_bg;
  title_st.fg = focused ? th.title_focus_fg : th.title_blur_fg;
  v.fill(m.title_bar, title_st);

  // Le titre s'écrit dans SA vue : le clip fait la troncature, il n'y a pas
  // de comptage de caractères à tenir juste.
  if (m.title_text.w > 0) {
    View tv = v.sub(m.title_text);
    tv.text(0, 0, w.title, title_st);
  }

  // Élidés dans l'ordre inverse de leur utilité : réduire part en premier,
  // fermer reste en dernier.
  const char* labels[3] = {"[_]", uni ? "[□]" : "[o]", uni ? "[×]" : "[x]"};
  for (int i = 0; i < m.button_count; ++i) {
    const int which = 3 - m.button_count + i;
    View bv = v.sub(Rect{m.buttons.x + kButtonWidth * i, m.buttons.y, kButtonWidth, 1});
    bv.text(0, 0, labels[which], title_st);
  }

  // La poignée occupe le coin bas-droit, qui n'existe que si le cadre a au
  // moins deux lignes -- sur une seule, la barre de titre l'a déjà pris.
  if (f.h >= 2) {
    Style grip = frame_st;
    grip.fg = th.accent;
    v.put(f.x + f.w - 1, f.y + f.h - 1, uni ? U'◢' : U'#', grip);
  }
}

}  // namespace sshos
