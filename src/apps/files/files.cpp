#include "apps/files/files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "common/utf8.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

// Le caractère d'élision. Un seul, et de largeur un : trois points en
// coûteraient trois, sur une ligne où chaque colonne compte.
constexpr char kEllipsis[] = "\u2026";

// La largeur d'AFFICHAGE d'une chaîne UTF-8 : une pleine chasse en vaut
// deux. Compter les octets donnerait des colonnes fausses dès le premier
// accent, et compter les points de code dès le premier idéogramme.
int display_width(std::string_view s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    const size_t used = utf8_decode(s, i, cp);
    if (used == 0) break;
    i += used;
    w += std::max(0, char_width(cp));
  }
  return w;
}

// Élide en gardant la FIN. C'est la fin d'un chemin qui porte
// l'information -- « …/paquet/exemples » se lit, « /usr/share/d… » ne dit
// rien.
std::string elide_left(const std::string& s, int width) {
  if (width <= 0) return {};
  if (display_width(s) <= width) return s;
  if (width == 1) return kEllipsis;

  // On remonte depuis la fin tant que ça tient, en s'arrêtant sur une
  // frontière de caractère : couper au milieu d'une pleine chasse
  // laisserait une moitié orpheline.
  const int room = width - 1;  // la place du caractère d'élision
  std::vector<size_t> starts;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    const size_t used = utf8_decode(s, i, cp);
    if (used == 0) break;
    starts.push_back(i);
    i += used;
  }
  int w = 0;
  size_t cut = s.size();
  for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
    char32_t cp = 0;
    utf8_decode(s, *it, cp);
    const int cw = std::max(0, char_width(cp));
    if (w + cw > room) break;
    w += cw;
    cut = *it;
  }
  return std::string(kEllipsis) + s.substr(cut);
}

// Élide en gardant le DÉBUT : pour un nom de fichier, c'est le début qui
// distingue.
std::string elide_right(const std::string& s, int width) {
  if (width <= 0) return {};
  if (display_width(s) <= width) return s;
  if (width == 1) return kEllipsis;

  const int room = width - 1;
  int w = 0;
  size_t i = 0;
  size_t cut = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    const size_t used = utf8_decode(s, i, cp);
    if (used == 0) break;
    const int cw = std::max(0, char_width(cp));
    if (w + cw > room) break;
    w += cw;
    i += used;
    cut = i;
  }
  return s.substr(0, cut) + kEllipsis;
}

}  // namespace

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
    // Remise à zéro DÉFENSIVE, et non discriminable aujourd'hui : la seule
    // façon d'avoir une liste vide est de partir d'un répertoire
    // illisible, et la sélection y vaut déjà zéro. Elle reste parce
    // qu'elle deviendra porteuse le jour où une liste non vide pourra le
    // devenir -- une suppression qui vide le dossier, par exemple.
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

std::string Files::touchable_selection() const {
  if (visible_.empty()) return {};
  const std::string& name = visible_[sel_].name;
  // `..` n'est pas un fichier de CE répertoire : le renommer renommerait
  // le parent, et le supprimer effacerait le dossier qui nous contient.
  // Personne ne demande ça en visant la première ligne.
  if (name == "..") return {};
  return name;
}

void Files::commit_rename() {
  const std::string from = touchable_selection();
  mode_ = Mode::Normal;
  if (from.empty() || edit_.empty() || edit_ == from) return;
  // Un nom qui contient une barre n'est pas un renommage, c'est un
  // déplacement -- et un déplacement à l'aveugle vers un chemin qu'on ne
  // voit pas est exactement ce qu'on ne veut pas offrir sur une touche.
  if (edit_.find('/') != std::string::npos) {
    status_ = "un nom ne peut pas contenir de barre";
    return;
  }

  const std::string src = join_path(listing_.path, from);
  const std::string dst = join_path(listing_.path, edit_);

  // `rename()` ECRASE silencieusement une cible existante. C'est la façon
  // la plus rapide de perdre un fichier, et le noyau n'offre pas de garde
  // portable : on regarde d'abord.
  struct stat st {};
  if (::lstat(dst.c_str(), &st) == 0) {
    status_ = "ce nom est deja pris";
    return;
  }
  if (::rename(src.c_str(), dst.c_str()) != 0) {
    status_ = std::string("renommage impossible : ") + std::strerror(errno);
    return;
  }

  status_.clear();
  reload();
  // La sélection SUIT le nom renommé : le perdre de vue après l'avoir
  // renommé oblige à le rechercher pour vérifier.
  for (size_t i = 0; i < visible_.size(); ++i) {
    if (visible_[i].name == edit_) {
      sel_ = i;
      break;
    }
  }
  settle();
}

void Files::commit_delete() {
  const std::string name = touchable_selection();
  mode_ = Mode::Normal;
  if (name.empty()) return;

  const std::string victim = join_path(listing_.path, name);
  const bool is_dir = visible_[sel_].kind == EntryKind::Dir;
  // Pas de suppression RÉCURSIVE : `rmdir` refuse un dossier non vide, et
  // c'est exactement ce qu'on veut. Effacer une arborescence entière sur
  // une touche est le genre de fonction qu'on regrette une seule fois.
  const int rc = is_dir ? ::rmdir(victim.c_str()) : ::unlink(victim.c_str());
  if (rc != 0) {
    status_ = std::string("suppression impossible : ") + std::strerror(errno);
    return;
  }
  status_.clear();
  reload();
}

void Files::on_key(const KeyEvent& k) {
  // Le renommage et la confirmation CAPTENT le clavier. Les laisser
  // partager les touches de la navigation ferait filtrer la liste sous les
  // doigts de celui qui tape un nom.
  if (mode_ == Mode::Renaming) {
    switch (k.key) {
      case Key::Enter:
        commit_rename();
        return;
      case Key::Escape:
        mode_ = Mode::Normal;
        edit_.clear();
        return;
      case Key::Backspace:
        if (!edit_.empty()) edit_.pop_back();
        return;
      case Key::Char:
        if (k.ch >= U' ') edit_ += encode_utf8(k.ch);
        return;
      default:
        return;
    }
  }
  if (mode_ == Mode::Confirming) {
    // SEUL un « o » ou un « y » explicite détruit. Toute autre réponse
    // annule : une confirmation qui accepte l'à-peu-près n'en est pas une.
    if (k.key == Key::Char && (k.ch == U'o' || k.ch == U'O' || k.ch == U'y' ||
                               k.ch == U'Y')) {
      commit_delete();
      return;
    }
    mode_ = Mode::Normal;
    return;
  }

  const size_t rows = static_cast<size_t>(rows_for_list());

  switch (k.key) {
    case Key::Up:
      if (sel_ > 0) --sel_;
      settle();
      return;
    case Key::Down:
      // Cette garde-ci n'est PAS porteuse -- `settle()` borne juste après,
      // et la mutation qui la retire est équivalente. Celle de la flèche
      // haut l'est, elle : `--sel_` sur zéro déborde par le bas et
      // enverrait la sélection À LA FIN de la liste. On les garde
      // symétriques pour que le lecteur n'ait pas à refaire cette
      // vérification.
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
    case Key::F2: {
      const std::string name = touchable_selection();
      if (name.empty()) return;
      // PRÉ-REMPLI : renommer « rapport-2025.txt » en « rapport-2026.txt »
      // ne doit pas demander de tout retaper.
      edit_ = name;
      mode_ = Mode::Renaming;
      return;
    }
    case Key::Delete:
      if (touchable_selection().empty()) return;
      mode_ = Mode::Confirming;
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

void Files::render(View v) {
  const int w = v.w();
  const int h = v.h();
  // Garde DÉFENSIVE, non discriminable : la `View` clippe déjà tout ce
  // qu'on lui écrit, donc peindre une fenêtre de largeur nulle ne fait
  // rien de mal. Elle reste parce qu'elle évite de parcourir la liste pour
  // rien, et parce qu'un futur calcul de géométrie pourrait, lui, diviser.
  if (w <= 0 || h <= 0) return;

  // La barre de chemin. En gras plutôt qu'en couleur : elle doit se
  // distinguer sur les seize couleurs comme sur les 16 millions.
  Style path_style;
  path_style.attrs = attr::Bold;
  v.text(0, 0, elide_left(listing_.path, w), path_style);

  const int rows = rows_for_list();
  for (int i = 0; i < rows; ++i) {
    const size_t idx = top_ + static_cast<size_t>(i);
    // `break` plutôt que `continue` : les index ne font que croître, donc
    // les deux donnent le même résultat -- la mutation est équivalente. On
    // s'arrête parce que c'est ce que le code veut dire.
    if (idx >= visible_.size()) break;
    const DirEntry& e = visible_[idx];

    Style st;
    switch (e.kind) {
      case EntryKind::Dir:
        st.fg = Color::indexed(4);
        break;
      case EntryKind::Link:
        st.fg = Color::indexed(6);
        break;
      default:
        break;
    }
    const int y = 1 + i;
    if (idx == sel_) {
      // La ligne ENTIÈRE porte l'inverse vidéo, pas seulement le nom : une
      // barre de sélection qui s'arrête au dernier caractère se lit comme
      // un mot surligné, pas comme une ligne choisie.
      st.attrs |= attr::Reverse;
      v.fill(Rect{0, y, w, 1}, st);
    }
    v.text(0, y, elide_right(e.name, w), st);
  }

  // La ligne d'état : l'erreur si elle existe, sinon le filtre en cours.
  // Sans elle, une liste réduite par un filtre passe pour un dossier
  // presque vide.
  Style status_style;
  std::string bottom;
  if (mode_ == Mode::Renaming) {
    // La saisie EST la ligne d'état : une invite qu'on ne voit pas est une
    // application qui a l'air bloquée.
    status_style.attrs = attr::Reverse;
    bottom = "nouveau nom: " + edit_;
  } else if (mode_ == Mode::Confirming) {
    status_style.attrs = attr::Reverse;
    status_style.fg = Color::indexed(1);
    bottom = "supprimer " + (visible_.empty() ? std::string() : visible_[sel_].name) +
             " ? (o/n)";
  } else if (!status_.empty()) {
    status_style.fg = Color::indexed(1);
    bottom = status_;
  } else if (!filter_.empty()) {
    status_style.attrs = attr::Bold;
    bottom = "filtre: " + filter_;
  }
  if (!bottom.empty()) v.text(0, h - 1, elide_right(bottom, w), status_style);
}

}  // namespace sshos
