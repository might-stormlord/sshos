#include "shell/modal.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "render/width.hpp"

namespace sshos {
namespace {

constexpr char kCancel[] = "[ Annuler ]";
constexpr char kConfirm[] = "[ Confirmer ]";
constexpr char kAcknowledge[] = "[ OK ]";
constexpr int kCancelW = sizeof(kCancel) - 1;
constexpr int kConfirmW = sizeof(kConfirm) - 1;

// Cadre, LES LIGNES DE TEXTE, ligne vide, boutons, ligne vide, cadre. Le
// corps peut tenir sur plusieurs lignes : « il existe 7 mises a jour » et
// « cce9d11 -> 3512ffe » ne se lisent pas serrees sur une seule.
constexpr int kChromeHeight = 5;
constexpr int kMinWidth = kCancelW + kConfirmW + 5;

// Decoupe le corps sur les retours a la ligne. Rendre un vecteur plutot que
// d'indexer a la volee : rect(), draw() et les boutons ont tous besoin du
// MEME compte de lignes, et le recalculer trois fois invite a la divergence.
std::vector<std::string> body_lines(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t nl = s.find('\n', start);
    if (nl == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  if (out.empty()) out.push_back(std::string());
  return out;
}

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
  const std::vector<std::string> lines = body_lines(question_);
  std::size_t longest = 0;
  for (const std::string& l : lines) longest = std::max(longest, l.size());

  const int want = static_cast<int>(longest) + 4;
  int w = std::max(kMinWidth, want);
  w = std::min(w, std::max(kMinWidth, cols - 4));
  w = std::min(w, cols);
  const int h = std::min(kChromeHeight + static_cast<int>(lines.size()), rows);
  return Rect{(cols - w) / 2, (rows - h) / 2, w, h};
}

// Les boutons descendent avec le corps : une ligne de plus les pousse d'une
// ligne. Sans ce calcul commun, hit() cliquerait ailleurs que ce que draw()
// a peint -- la discipline du panneau, appliquee ici.
int Modal::buttons_y() const {
  return rect_.y + static_cast<int>(body_lines(question_).size()) + 2;
}

void Modal::layout(int cols, int rows) { rect_ = rect(cols, rows); }

Rect Modal::confirm_rect() const {
  return Rect{rect_.x + rect_.w - 2 - kConfirmW, buttons_y(), kConfirmW, 1};
}

Rect Modal::cancel_rect() const {
  return Rect{confirm_rect().x - 1 - kCancelW, buttons_y(), kCancelW, 1};
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
  // mais draw() reçoit une View pleine largeur : sans cette coupe, une ligne
  // plus longue que la boîte débordait par-dessus le bureau jusqu'au bord
  // droit. Le tilde plutôt qu'une ellipse Unicode : Modal ne sait pas si le
  // client accepte l'UTF-8.
  const std::vector<std::string> lines = body_lines(question_);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    v.text(rect_.x + 2, rect_.y + 1 + static_cast<int>(i),
           elide_to_cells(lines[i], rect_.w - 4, "~"), st);
  }
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
