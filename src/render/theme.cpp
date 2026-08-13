#include "render/theme.hpp"

namespace sshos {

Theme Theme::defaults() {
  Theme t;
  t.desktop_bg = Color::rgb(0x20, 0x30, 0x50);
  t.desktop_sign = Color::rgb(0x2c, 0x40, 0x68);
  t.panel_bg = Color::rgb(0x0c, 0x0e, 0x14);
  t.panel_fg = Color::rgb(0xc8, 0xd0, 0xdc);
  t.accent = Color::rgb(0x4a, 0x9e, 0xff);
  t.title_focus_bg = Color::rgb(0x1e, 0x5a, 0xa8);
  t.title_focus_fg = Color::rgb(0xff, 0xff, 0xff);
  t.title_blur_bg = Color::rgb(0x2c, 0x32, 0x40);
  t.title_blur_fg = Color::rgb(0x9a, 0xa4, 0xb4);
  t.border_focus = Color::rgb(0x4a, 0x9e, 0xff);
  t.border_blur = Color::rgb(0x50, 0x58, 0x68);
  t.modal_bg = Color::rgb(0x28, 0x2e, 0x3c);
  t.modal_fg = Color::rgb(0xf0, 0xf4, 0xfa);
  return t;
}

Theme Theme::indexed256() {
  Theme t;
  t.desktop_bg = Color::indexed(17);
  t.desktop_sign = Color::indexed(18);
  t.panel_bg = Color::indexed(234);
  t.panel_fg = Color::indexed(252);
  t.accent = Color::indexed(75);
  t.title_focus_bg = Color::indexed(25);
  t.title_focus_fg = Color::indexed(231);
  t.title_blur_bg = Color::indexed(238);
  t.title_blur_fg = Color::indexed(145);
  t.border_focus = Color::indexed(75);
  t.border_blur = Color::indexed(240);
  t.modal_bg = Color::indexed(236);
  t.modal_fg = Color::indexed(255);
  return t;
}

Theme Theme::mono16() {
  // Seuls les index 0 à 7 sont atteignables : quantize_16() n'utilise pas le
  // bit brillant. Choisir un index 8-15 ici donnerait une couleur qui
  // s'effondre silencieusement sur son homologue sombre.
  Theme t;
  t.desktop_bg = Color::indexed(4);
  t.desktop_sign = Color::indexed(12);
  t.panel_bg = Color::indexed(0);
  t.panel_fg = Color::indexed(7);
  t.accent = Color::indexed(6);
  t.title_focus_bg = Color::indexed(6);
  t.title_focus_fg = Color::indexed(0);
  t.title_blur_bg = Color::indexed(0);
  t.title_blur_fg = Color::indexed(7);
  t.border_focus = Color::indexed(7);
  t.border_blur = Color::indexed(4);
  t.modal_bg = Color::indexed(7);
  t.modal_fg = Color::indexed(0);
  return t;
}

Theme Theme::for_profile(const OutputProfile& p) const {
  switch (p.depth) {
    case ColorDepth::TrueColor:
      return *this;
    case ColorDepth::Indexed256:
      return indexed256();
    case ColorDepth::Mono16:
      return mono16();
  }
  return *this;
}

}  // namespace sshos
