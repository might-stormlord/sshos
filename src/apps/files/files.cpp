#include "apps/files/files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
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
// Une taille lisible d'un coup d'œil. Les puissances de 1024, parce que
// c'est ce que tout le reste du système affiche, et une seule décimale --
// « 1,4 Go » se lit, « 1503238553 » se compte.
std::string human_size(uint64_t bytes) {
  static const char* kUnits[] = {"o", "Ko", "Mo", "Go", "To"};
  double v = static_cast<double>(bytes);
  size_t u = 0;
  while (v >= 1024.0 && u + 1 < sizeof kUnits / sizeof kUnits[0]) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  // Pas de décimale aux octets : « 7,0 o » est du bruit, pas de la
  // précision.
  std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[u]);
  return buf;
}

// La date, telle qu'on la lit dans une colonne de dix cellules. Le format
// est ISO à l'envers -- JJ/MM/AAAA -- parce que c'est celui que lit
// l'utilisateur, et sans heure : elle coûterait cinq cellules de plus sur
// une fenêtre qui n'en a pas tant, et le jour suffit à retrouver un
// fichier neuf fois sur dix.
//
// `localtime` lit le fuseau du DÉMON, pas celui du client. C'est la même
// limite que l'horloge du panneau, notée au §7.2 du dossier de reprise, et
// elle se corrigera au même endroit : quand le message d'accueil portera le
// fuseau du client.
std::string human_date(uint64_t mtime) {
  if (mtime == 0) return {};
  const time_t t = static_cast<time_t>(mtime);
  struct tm tmv {};
  if (::localtime_r(&t, &tmv) == nullptr) return {};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d/%02d/%04d", tmv.tm_mday, tmv.tm_mon + 1,
                tmv.tm_year + 1900);
  return buf;
}

// Un texte calé à DROITE dans `width` cellules. Les chiffres se comparent
// à l'œil quand leurs unités sont alignées, jamais quand leurs premières
// décimales le sont.
std::string right_align(const std::string& s, int width) {
  const int pad = width - display_width(s);
  if (pad <= 0) return s;
  return std::string(static_cast<size_t>(pad), ' ') + s;
}

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
  // La barre de chemin et l'en-tête des colonnes en haut, la ligne d'état
  // en bas. Une fenêtre trop petite pour les trois montre au moins une
  // ligne de liste : rendre zéro ferait disparaître le contenu au lieu de
  // le serrer.
  return std::max(1, size_.h - 3);
}

Files::Columns Files::columns(int w) const {
  Columns c;
  // « 1023.9 Ko » tient en neuf cellules, « JJ/MM/AAAA » en dix.
  constexpr int kSizeW = 9;
  constexpr int kDateW = 10;
  // En dessous, un nom n'est plus lisible : c'est le seuil au-dessous
  // duquel une colonne chiffrée cède la place plutôt que de le manger.
  constexpr int kNameMin = 12;

  c.name_w = w;
  if (w >= kNameMin + 1 + kSizeW + 1 + kDateW) {
    c.date_w = kDateW;
    c.date_x = w - kDateW;
    c.size_w = kSizeW;
    c.size_x = c.date_x - 1 - kSizeW;
    c.name_w = c.size_x - 1;
  } else if (w >= kNameMin + 1 + kSizeW) {
    c.size_w = kSizeW;
    c.size_x = w - kSizeW;
    c.name_w = c.size_x - 1;
  }
  return c;
}

void Files::sort_on(SortBy by) {
  // Recliquer la même colonne INVERSE ; en choisir une autre repart dans
  // le sens croissant -- personne n'attend qu'un tri par date hérite du
  // sens qu'on avait donné aux tailles.
  if (sort_by_ == by) {
    sort_desc_ = !sort_desc_;
  } else {
    sort_by_ = by;
    sort_desc_ = false;
  }
  refilter();
}

void Files::reload() {
  // Les noms marqués sont ceux d'AVANT. Les garder ferait porter la
  // prochaine action sur leurs homonymes ici, ce qui est le pire résultat
  // possible.
  marked_.clear();
  listing_ = read_dir(listing_.path, show_hidden_);
  status_ = listing_.error;
  refilter();
}

void Files::refilter() {
  // LA LIGNE CHOISIE SURVIT AU TRI. Elle change de rang, pas d'identité :
  // la retrouver ailleurs dans la liste est le minimum qu'on attende d'un
  // clic sur un en-tête.
  const std::string kept =
      sel_ < visible_.size() ? visible_[sel_].name : std::string();

  visible_ = filter_entries(listing_.entries, filter_);
  sort_entries(visible_, sort_by_, sort_desc_);

  if (!kept.empty()) {
    for (size_t i = 0; i < visible_.size(); ++i) {
      if (visible_[i].name == kept) {
        sel_ = i;
        break;
      }
    }
  }
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

std::string Files::markable_at(size_t i) const {
  if (i >= visible_.size()) return {};
  // `..` n'est pas un fichier, c'est la sortie : le laisser entrer dans une
  // sélection ferait porter une copie ou une suppression sur le parent.
  if (visible_[i].name == "..") return {};
  return visible_[i].name;
}

bool Files::toggle_mark(size_t i) {
  const std::string name = markable_at(i);
  if (name.empty()) return false;
  const auto at = marked_.find(name);
  if (at == marked_.end()) {
    marked_.insert(name);
  } else {
    marked_.erase(at);
  }
  return true;
}

void Files::mark_range(size_t a, size_t b) {
  const size_t lo = std::min(a, b);
  const size_t hi = std::max(a, b);
  for (size_t i = lo; i <= hi && i < visible_.size(); ++i) {
    const std::string name = markable_at(i);
    if (!name.empty()) marked_.insert(name);
  }
}

std::vector<std::string> Files::targets() const {
  // LES MARQUÉS S'IL Y EN A, sinon la seule ligne sous la sélection. C'est
  // la règle de tous les gestionnaires, et elle évite d'avoir à marquer un
  // fichier pour agir sur lui.
  if (!marked_.empty()) {
    return std::vector<std::string>(marked_.begin(), marked_.end());
  }
  const std::string one = touchable_selection();
  if (one.empty()) return {};
  return {one};
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
  const std::vector<std::string> victims = targets();
  mode_ = Mode::Normal;
  if (victims.empty()) return;

  status_.clear();
  int failed = 0;
  std::string first_error;
  for (const std::string& name : victims) {
    const std::string victim = join_path(listing_.path, name);
    // Pas de suppression RÉCURSIVE : `rmdir` refuse un dossier non vide, et
    // c'est exactement ce qu'on veut. Effacer une arborescence entière sur
    // une touche est le genre de fonction qu'on regrette une seule fois.
    // `unlink` d'abord et `rmdir` en repli plutôt que de relire le type :
    // la liste peut dater, le disque non.
    int rc = ::unlink(victim.c_str());
    if (rc != 0 && (errno == EISDIR || errno == EPERM)) {
      rc = ::rmdir(victim.c_str());
    }
    if (rc != 0) {
      ++failed;
      if (first_error.empty()) first_error = std::strerror(errno);
    }
  }
  // RELIRE D'ABORD, DIRE ENSUITE : `reload()` repose `status_` sur l'erreur
  // de lecture du répertoire -- vide quand tout va bien -- et effacerait
  // donc le message qu'on vient d'écrire.
  reload();
  // ON CONTINUE APRÈS UN ÉCHEC, et on dit combien : s'arrêter au premier
  // laisserait une sélection à moitié traitée dont l'utilisateur ne
  // saurait pas où elle en est.
  if (failed > 0) {
    status_ = failed == 1 && victims.size() == 1
                  ? "suppression impossible : " + first_error
                  : std::to_string(failed) + " sur " +
                        std::to_string(victims.size()) +
                        " n'ont pas pu etre supprimes : " + first_error;
  }
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

  // `Ctrl+A` BASCULE : tout, puis rien. Un terminal ne sait pas distinguer
  // `Ctrl+Maj+A` de `Ctrl+A` -- la combinaison de Dolphin est intapable ici
  // -- et deux raccourcis pour un aller-retour valent moins qu'un seul qui
  // fait les deux.
  if (k.key == Key::Char && (k.ch == U'a' || k.ch == U'A') &&
      (k.mods & mod::Ctrl) != 0) {
    if (marked_.empty()) {
      mark_range(0, visible_.empty() ? 0 : visible_.size() - 1);
    } else {
      marked_.clear();
    }
    return;
  }
  // `Espace` MARQUE ET DESCEND : on parcourt la liste en marquant au
  // passage, sans relever les doigts pour bouger. Il descend MÊME sur `..`,
  // qui ne se marque pas -- rester bloqué là donnerait l'impression que la
  // touche ne fait rien.
  if (k.key == Key::Char && k.ch == U' ' && filter_.empty()) {
    toggle_mark(sel_);
    if (sel_ + 1 < visible_.size()) ++sel_;
    settle();
    return;
  }

  switch (k.key) {
    case Key::Up:
      // `Maj+flèche` ÉTEND depuis la position courante : c'est le geste
      // qu'on essaie en premier quand on vient d'un vrai bureau.
      if ((k.mods & mod::Shift) != 0) mark_range(sel_, sel_);
      if (sel_ > 0) --sel_;
      if ((k.mods & mod::Shift) != 0) mark_range(sel_, sel_);
      settle();
      return;
    case Key::Down:
      // Cette garde-ci n'est PAS porteuse -- `settle()` borne juste après,
      // et la mutation qui la retire est équivalente. Celle de la flèche
      // haut l'est, elle : `--sel_` sur zéro déborde par le bas et
      // enverrait la sélection À LA FIN de la liste. On les garde
      // symétriques pour que le lecteur n'ait pas à refaire cette
      // vérification.
      if ((k.mods & mod::Shift) != 0) mark_range(sel_, sel_);
      if (sel_ + 1 < visible_.size()) ++sel_;
      if ((k.mods & mod::Shift) != 0) mark_range(sel_, sel_);
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
      // `targets()`, pas la seule ligne : avec une sélection, le curseur
      // peut très bien être resté sur `..`, qui ne se supprime pas.
      if (targets().empty()) return;
      mode_ = Mode::Confirming;
      return;
    case Key::Escape:
      // L'échappement RÉTABLIT, du plus récent au plus ancien : le filtre
      // d'abord, la sélection ensuite. Il ne ferme jamais la fenêtre -- on
      // perdrait le répertoire courant pour une frappe de trop.
      if (!filter_.empty()) {
        filter_.clear();
        refilter();
        return;
      }
      marked_.clear();
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

  // LA LIGNE 1 EST L'EN-TÊTE, et cliquer une colonne trie dessus. C'est le
  // geste de tous les gestionnaires, et il n'a aucun équivalent au clavier
  // qui se devine.
  if (m.y == 1) {
    const Columns c = columns(size_.w);
    if (c.date_w > 0 && m.x >= c.date_x) {
      sort_on(SortBy::Time);
    } else if (c.size_w > 0 && m.x >= c.size_x) {
      sort_on(SortBy::Size);
    } else {
      sort_on(SortBy::Name);
    }
    return;
  }

  // La barre de chemin, puis l'en-tête : la liste commence en 2.
  const int row = m.y - 2;
  if (row < 0 || static_cast<size_t>(row) >= rows) return;
  const size_t hit = top_ + static_cast<size_t>(row);
  if (hit >= visible_.size()) return;

  // `Ctrl+clic` ajoute ou retire UNE entrée sans toucher au reste et sans
  // l'ouvrir : c'est ce qui distingue le clic qui choisit du clic qui agit.
  if ((m.mods & mod::Ctrl) != 0) {
    toggle_mark(hit);
    sel_ = hit;
    settle();
    return;
  }
  // `Maj+clic` prend TOUT ce qui va de la position courante au clic.
  if ((m.mods & mod::Shift) != 0) {
    mark_range(sel_, hit);
    sel_ = hit;
    settle();
    return;
  }

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
  out = Pos{0, 2 + static_cast<int>(sel_ - top_)};
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

  // L'EN-TÊTE : il nomme les colonnes, et il est la seule chose qui dise
  // sur quoi la liste est triée. La flèche marque celle qui porte le tri.
  const Columns c = columns(w);
  Style head;
  head.attrs = attr::Underline;
  const std::string arrow = sort_desc_ ? "v" : "^";
  v.text(0, 1, elide_right(sort_by_ == SortBy::Name ? "Nom " + arrow : "Nom",
                           c.name_w),
         head);
  if (c.size_w > 0) {
    v.text(c.size_x, 1,
           right_align(sort_by_ == SortBy::Size ? "Taille " + arrow : "Taille",
                       c.size_w),
           head);
  }
  if (c.date_w > 0) {
    v.text(c.date_x, 1,
           sort_by_ == SortBy::Time ? "Date " + arrow : "Date", head);
  }

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
    const int y = 2 + i;
    // LA MARQUE PRÉCÈDE LE NOM, et elle est en couleur : un compteur en bas
    // dit COMBIEN sont choisis, jamais LESQUELS, et une sélection qu'on ne
    // peut pas relire ne se corrige pas.
    const bool chosen = marked_.count(e.name) != 0;
    if (idx == sel_) {
      // La ligne ENTIÈRE porte l'inverse vidéo, pas seulement le nom : une
      // barre de sélection qui s'arrête au dernier caractère se lit comme
      // un mot surligné, pas comme une ligne choisie.
      st.attrs |= attr::Reverse;
      v.fill(Rect{0, y, w, 1}, st);
    }
    if (chosen) {
      Style mark = st;
      mark.fg = Color::indexed(3);
      mark.attrs |= attr::Bold;
      v.text(0, y, "*", mark);
      v.text(1, y, elide_right(e.name, c.name_w - 1), st);
    } else {
      v.text(0, y, elide_right(e.name, c.name_w), st);
    }
    // Un répertoire n'a pas de taille qui veuille dire quelque chose : celle
    // de son inode ne dit rien de ce qu'il contient, et l'afficher ferait
    // croire le contraire.
    if (c.size_w > 0 && e.kind != EntryKind::Dir && e.name != "..") {
      v.text(c.size_x, y, right_align(human_size(e.size), c.size_w), st);
    }
    if (c.date_w > 0 && e.name != "..") {
      v.text(c.date_x, y, human_date(e.mtime), st);
    }
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
    // COMBIEN, PAS SEULEMENT « quoi ». « supprimer ? » sur une sélection de
    // trente fichiers ne dit pas ce qu'on s'apprête à perdre.
    bottom = marked_.empty()
                 ? "supprimer " +
                       (visible_.empty() ? std::string() : visible_[sel_].name) +
                       " ? (o/n)"
                 : "supprimer " + std::to_string(marked_.size()) +
                       " elements ? (o/n)";
  } else if (!status_.empty()) {
    status_style.fg = Color::indexed(1);
    bottom = status_;
  } else if (!filter_.empty()) {
    status_style.attrs = attr::Bold;
    bottom = "filtre: " + filter_;
  } else if (!marked_.empty()) {
    // COMBIEN, ET COMBIEN ÇA PÈSE. Une sélection qu'on ne voit pas est une
    // sélection dont on ne se souvient plus au moment d'appuyer sur Suppr.
    uint64_t bytes = 0;
    for (const DirEntry& e : listing_.entries) {
      if (marked_.count(e.name) != 0) bytes += e.size;
    }
    status_style.attrs = attr::Bold;
    status_style.fg = Color::indexed(3);
    bottom = std::to_string(marked_.size()) + " selectionnes, " +
             human_size(bytes);
  }
  if (!bottom.empty()) v.text(0, h - 1, elide_right(bottom, w), status_style);
}

}  // namespace sshos
