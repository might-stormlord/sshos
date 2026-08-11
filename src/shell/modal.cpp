#include "shell/modal.hpp"

#include <algorithm>

namespace sshos {
namespace {

constexpr char kCancel[] = "[ Annuler ]";
constexpr char kConfirm[] = "[ Confirmer ]";
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

void Modal::dismiss() {
  open_ = false;
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

  v.text(rect_.x + 2, rect_.y + 1, question_, st);
  const Rect c = cancel_rect();
  const Rect k = confirm_rect();
  v.text(c.x, c.y, kCancel, confirm_ ? st : hot);
  v.text(k.x, k.y, kConfirm, confirm_ ? hot : st);
}

ModalHit Modal::hit(int x, int y) const {
  if (!open_ || !rect_.contains(x, y)) return ModalHit::None;
  if (cancel_rect().contains(x, y)) return ModalHit::Cancel;
  if (confirm_rect().contains(x, y)) return ModalHit::Confirm;
  return ModalHit::Body;
}

}  // namespace sshos
