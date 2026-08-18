#include "shell/modal.hpp"

#include <algorithm>

#include "render/width.hpp"

namespace sshos {
namespace {

constexpr char kCancel[] = "[ Annuler ]";
constexpr char kConfirm[] = "[ Confirmer ]";
constexpr char kAcknowledge[] = "[ OK ]";
constexpr int kCancelW = sizeof(kCancel) - 1;
constexpr int kConfirmW = sizeof(kConfirm) - 1;

// Cadre, question, ligne vide, boutons, ligne vide, cadre.
constexpr int kHeight = 6;
constexpr int kMinWidth = kCancelW + kConfirmW + 5;

}  // namespace

void Modal::ask(std::string question, WindowId target) {
  // La seconde demande est ignorée, pas mise en file : c'est la première
  // question que l'utilisateur a sous les yeux.
  if (open_) return;
  open_ = true;
  question_ = std::move(question);
  target_ = target;
  confirm_ = false;
}

void Modal::inform(std::string message) {
  // Même règle que ask() : la seconde demande est ignorée, pas mise en file.
  if (open_) return;
  open_ = true;
  info_ = true;
  question_ = std::move(message);
  target_ = 0;
  confirm_ = true;  // le seul bouton a le focus
}

void Modal::dismiss() {
  open_ = false;
  info_ = false;
  question_.clear();
  target_ = 0;
  confirm_ = false;
}

Rect Modal::rect(int cols, int rows) const {
  const int want = static_cast<int>(question_.size()) + 4;
  int w = std::max(kMinWidth, want);
  w = std::min(w, std::max(kMinWidth, cols - 4));
  w = std::min(w, cols);
  const int h = std::min(kHeight, rows);
  return Rect{(cols - w) / 2, (rows - h) / 2, w, h};
}

void Modal::layout(int cols, int rows) { rect_ = rect(cols, rows); }

Rect Modal::confirm_rect() const {
  return Rect{rect_.x + rect_.w - 2 - kConfirmW, rect_.y + 3, kConfirmW, 1};
}

Rect Modal::cancel_rect() const {
  return Rect{confirm_rect().x - 1 - kCancelW, rect_.y + 3, kCancelW, 1};
}

void Modal::draw(View v, const Theme& th, Border b) const {
  if (!open_) return;

  Style st;
  st.bg = th.modal_bg;
  st.fg = th.modal_fg;
  v.fill(rect_, st);
  v.box(rect_, b, st);

  Style hot = st;
  hot.bg = th.accent;
  hot.fg = th.modal_bg;

  // ÉLIDÉ À LA LARGEUR DE LA BOÎTE. rect() borne bien le cadre à l'écran,
  // mais draw() reçoit une View pleine largeur : sans cette coupe, une
  // question plus longue que la boîte débordait par-dessus le bureau
  // jusqu'au bord droit. Le tilde plutôt qu'une ellipse Unicode : Modal ne
  // sait pas si le client accepte l'UTF-8.
  v.text(rect_.x + 2, rect_.y + 1,
         elide_to_cells(question_, rect_.w - 4, "~"), st);
  if (info_) {
    // Un seul bouton, centré : il n'y a rien à décider.
    const int w = static_cast<int>(sizeof(kAcknowledge) - 1);
    v.text(rect_.x + (rect_.w - w) / 2, confirm_rect().y, kAcknowledge, hot);
    return;
  }
  const Rect c = cancel_rect();
  const Rect k = confirm_rect();
  v.text(c.x, c.y, kCancel, confirm_ ? st : hot);
  v.text(k.x, k.y, kConfirm, confirm_ ? hot : st);
}

ModalHit Modal::hit(int x, int y) const {
  if (!open_ || !rect_.contains(x, y)) return ModalHit::None;
  // En mode information il n'y a qu'un bouton, et cliquer n'importe ou sur
  // sa ligne l'atteint : c'est une reconnaissance, pas un choix.
  if (info_) {
    if (confirm_rect().y == y) return ModalHit::Confirm;
    return ModalHit::Body;
  }
  if (cancel_rect().contains(x, y)) return ModalHit::Cancel;
  if (confirm_rect().contains(x, y)) return ModalHit::Confirm;
  return ModalHit::Body;
}

}  // namespace sshos
