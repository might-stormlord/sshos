#include "apps/files/files.hpp"

#include <algorithm>

#include "common/utf8.hpp"
#include "render/surface.hpp"

namespace sshos {

Files::Files() : Files("/") {}

Files::Files(std::string start) {
  listing_.path = std::move(start);
  reload();
}

void Files::attach(Host& host) {
  host_ = &host;
  host.set_title("Fichiers");
}

void Files::on_resize(Size s) {
  if (s.w <= 0 || s.h <= 0) return;
  size_ = s;
  settle();
}

int Files::rows_for_list() const {
  // La barre de chemin en haut, la ligne d'état en bas. Une fenêtre trop
  // petite pour les deux montre au moins une ligne de liste : rendre zéro
  // ferait disparaître le contenu au lieu de le serrer.
  return std::max(1, size_.h - 2);
}

void Files::reload() {
  listing_ = read_dir(listing_.path, show_hidden_);
  status_ = listing_.error;
  refilter();
}

void Files::refilter() {
  visible_ = filter_entries(listing_.entries, filter_);
  settle();
}

void Files::settle() {
  // La sélection D'ABORD : borner le défilement sur une sélection hors
  // bornes le poserait n'importe où, et la fenêtre montrerait une page qui
  // ne contient pas la ligne choisie.
  if (visible_.empty()) {
    sel_ = 0;
    top_ = 0;
    return;
  }
  if (sel_ >= visible_.size()) sel_ = visible_.size() - 1;

  const size_t rows = static_cast<size_t>(rows_for_list());
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
  // Et jamais de page vide en bas : quand la liste rétrécit sous le
  // défilement, celui-ci doit remonter.
  if (top_ + rows > visible_.size()) {
    top_ = visible_.size() > rows ? visible_.size() - rows : 0;
  }
}

void Files::go_up() {
  const std::string up = parent_path(listing_.path);
  if (up == listing_.path) {
    // La racine n'a pas de parent. Ne rien faire est la seule réponse
    // honnête -- et surtout pas effacer le filtre au passage.
    return;
  }
  // Le répertoire qu'on quitte devient la sélection dans son parent :
  // remonter puis redescendre doit ramener au même endroit, sans avoir à
  // rechercher des yeux d'où l'on vient.
  std::string leaving = listing_.path;
  while (leaving.size() > 1 && leaving.back() == '/') leaving.pop_back();
  const size_t cut = leaving.rfind('/');
  const std::string name =
      cut == std::string::npos ? std::string() : leaving.substr(cut + 1);

  listing_.path = up;
  filter_.clear();
  sel_ = 0;
  top_ = 0;
  reload();

  for (size_t i = 0; i < visible_.size(); ++i) {
    if (visible_[i].name == name) {
      sel_ = i;
      break;
    }
  }
  settle();
}

void Files::activate() {
  if (visible_.empty()) return;
  const DirEntry& e = visible_[sel_];
  if (e.kind != EntryKind::Dir) {
    // Un fichier s'ouvrirait dans l'éditeur, qui est le jalon 6. D'ici là,
    // le dire vaut mieux que ne rien faire : une touche sans effet et sans
    // explication passe pour une panne.
    status_ = "l'editeur arrive au jalon 6";
    return;
  }
  if (e.name == "..") {
    go_up();
    return;
  }

  const std::string target = join_path(listing_.path, e.name);
  const DirListing probe = read_dir(target, show_hidden_);
  if (!probe.error.empty()) {
    // On RESTE où l'on est. Descendre dans un répertoire illisible pour y
    // afficher une liste vide donnerait l'impression d'un dossier vide.
    status_ = probe.error;
    return;
  }
  listing_ = probe;
  status_.clear();
  filter_.clear();
  sel_ = 0;
  top_ = 0;
  refilter();
}

void Files::on_key(const KeyEvent& k) {
  const size_t rows = static_cast<size_t>(rows_for_list());

  switch (k.key) {
    case Key::Up:
      if (sel_ > 0) --sel_;
      settle();
      return;
    case Key::Down:
      if (sel_ + 1 < visible_.size()) ++sel_;
      settle();
      return;
    case Key::PgUp:
      sel_ = sel_ > rows ? sel_ - rows : 0;
      settle();
      return;
    case Key::PgDn:
      sel_ = std::min(sel_ + rows, visible_.empty() ? 0 : visible_.size() - 1);
      settle();
      return;
    case Key::Home:
      sel_ = 0;
      settle();
      return;
    case Key::End:
      sel_ = visible_.empty() ? 0 : visible_.size() - 1;
      settle();
      return;
    case Key::Enter:
      activate();
      return;
    case Key::Escape:
      // L'échappement efface le filtre. C'est le seul geste d'annulation
      // de l'application, et il ne doit pas fermer la fenêtre : on perdrait
      // le répertoire courant pour une frappe de trop.
      if (!filter_.empty()) {
        filter_.clear();
        refilter();
      }
      return;
    case Key::Backspace:
      // Le retour arrière EFFACE LE FILTRE tant qu'il en reste, et remonte
      // seulement ensuite. Remonter avec un filtre à moitié tapé ferait
      // perdre le répertoire pour une faute de frappe.
      if (!filter_.empty()) {
        filter_.pop_back();
        refilter();
        return;
      }
      go_up();
      return;
    case Key::Char:
      break;
    default:
      return;
  }

  if (k.ch == U'.' && filter_.empty()) {
    // `.` bascule les cachés -- mais seulement hors filtre, sinon on ne
    // pourrait jamais chercher un nom qui en contient un.
    show_hidden_ = !show_hidden_;
    reload();
    return;
  }
  if (k.ch >= U' ') {
    filter_ += encode_utf8(k.ch);
    refilter();
  }
}

void Files::on_mouse(const MouseEvent& m) {
  const size_t rows = static_cast<size_t>(rows_for_list());
  if (m.action == MouseAction::WheelUp) {
    sel_ = sel_ > 3 ? sel_ - 3 : 0;
    settle();
    return;
  }
  if (m.action == MouseAction::WheelDown) {
    sel_ = std::min(sel_ + 3, visible_.empty() ? 0 : visible_.size() - 1);
    settle();
    return;
  }
  if (m.action != MouseAction::Press) return;

  // La première ligne est la barre de chemin ; la liste commence en 1.
  const int row = m.y - 1;
  if (row < 0 || static_cast<size_t>(row) >= rows) return;
  const size_t hit = top_ + static_cast<size_t>(row);
  if (hit >= visible_.size()) return;

  // Cliquer SÉLECTIONNE ; recliquer la ligne déjà choisie l'OUVRE. Pas de
  // double-clic : l'application n'a pas à savoir compter les clics, et
  // « cliquer deux fois » se découvre tout seul.
  if (hit == sel_) {
    activate();
    return;
  }
  sel_ = hit;
  settle();
}

bool Files::wants_cursor(Pos& out) const {
  if (visible_.empty()) return false;
  out = Pos{0, 1 + static_cast<int>(sel_ - top_)};
  return true;
}

void Files::render(View v) { (void)v; }

}  // namespace sshos
