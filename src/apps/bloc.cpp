#include "apps/bloc.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace sshos {

void Bloc::attach(Host& host) {
  host_ = &host;
  host_->set_title("Bloc");
}

void Bloc::clamp_cursor() {
  // La zone cliente peut rétrécir sous le curseur. Une position hors zone
  // serait transmise telle quelle au client, qui placerait son curseur
  // matériel sur une cellule qui n'existe plus.
  cursor_.x = std::max(0, std::min(cursor_.x, size_.w - 1));
  cursor_.y = std::max(0, std::min(cursor_.y, size_.h - 1));
}

void Bloc::on_resize(Size s) {
  ++resizes_;
  size_ = s;
  clamp_cursor();
}

void Bloc::on_key(const KeyEvent& k) {
  switch (k.key) {
    case Key::Left:
      --cursor_.x;
      break;
    case Key::Right:
      ++cursor_.x;
      break;
    case Key::Up:
      --cursor_.y;
      break;
    case Key::Down:
      ++cursor_.y;
      break;
    case Key::Home:
      cursor_.x = 0;
      break;
    case Key::End:
      cursor_.x = size_.w - 1;
      break;
    default:
      // Toute autre touche est une « saisie » : c'est elle qui rend le
      // document modifié, pas un simple déplacement du curseur. Sans cette
      // distinction, ouvrir une fenêtre et appuyer sur une flèche
      // suffirait à déclencher un dialogue de confirmation à la fermeture.
      if (!modified_) {
        modified_ = true;
        if (host_ != nullptr) host_->set_title("Bloc *");
      }
      break;
  }
  clamp_cursor();
}

void Bloc::on_mouse(const MouseEvent& m) {
  if (m.action != MouseAction::Press) return;
  ++clicks_;
  cursor_.x = m.x;
  cursor_.y = m.y;
  clamp_cursor();
}

bool Bloc::wants_cursor(Pos& out) const {
  out = cursor_;
  return true;
}

CloseCheck Bloc::can_close() const {
  if (!modified_) return CloseCheck::allow();
  return CloseCheck::ask("Bloc a des modifications non enregistrees.");
}

void Bloc::render(View v) {
  Style st;
  st.fg = Color::indexed(7);
  st.bg = Color::indexed(0);
  v.fill(Rect{0, 0, v.w(), v.h()}, st);

  // Les trois lignes sont retenues plutôt qu'écrites directement : le
  // curseur ci-dessous a besoin de savoir quel caractère se trouve sous
  // lui. Elles sont purement ASCII, donc l'index d'octet vaut la colonne.
  const std::string lines[3] = {
      "taille: " + std::to_string(size_.w) + "x" + std::to_string(size_.h),
      "clics: " + std::to_string(clicks_),
      "resize: " + std::to_string(resizes_),
  };
  for (int i = 0; i < 3; ++i) v.text(0, i, lines[i], st);

  // Le curseur INVERSE la cellule sous lui au lieu de la remplacer, comme
  // le fait un vrai curseur de terminal. Un curseur qui remplacerait
  // effacerait le caractère qu'il recouvre, et les relevés « taille: »,
  // « clics: » et « resize: » -- sur lesquels reposent les tests de
  // session -- deviendraient illisibles dès que le curseur passe dessus.
  Style cur = st;
  cur.attrs |= attr::Reverse;
  char32_t under = U' ';
  if (cursor_.y >= 0 && cursor_.y < 3) {
    const std::string& l = lines[cursor_.y];
    if (cursor_.x >= 0 && static_cast<std::size_t>(cursor_.x) < l.size())
      under = static_cast<char32_t>(static_cast<unsigned char>(l[cursor_.x]));
  }
  v.put(cursor_.x, cursor_.y, under, cur);
}

}  // namespace sshos
