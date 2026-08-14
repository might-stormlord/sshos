#include "apps/files/files.hpp"

#include <fcntl.h>
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
  pane().listing.path = std::move(start);
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

int Files::pane_width() const {
  // La cloison prend une colonne, et le reste se partage. Le panneau de
  // gauche prend la part impaire : deux panneaux qui se touchent sans
  // colonne perdue valent mieux qu'une symétrie parfaite.
  if (!split_) return std::max(1, size_.w);
  return std::max(1, (size_.w - 1) / 2);
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
  if (pane().sort_by == by) {
    pane().sort_desc = !pane().sort_desc;
  } else {
    pane().sort_by = by;
    pane().sort_desc = false;
  }
  refilter();
}

void Files::reload() {
  // Les noms marqués sont ceux d'AVANT. Les garder ferait porter la
  // prochaine action sur leurs homonymes ici, ce qui est le pire résultat
  // possible.
  pane().marked.clear();
  pane().listing = read_dir(pane().listing.path, show_hidden_);
  pane().status = pane().listing.error;
  refilter();
}

void Files::refilter() {
  // LA LIGNE CHOISIE SURVIT AU TRI. Elle change de rang, pas d'identité :
  // la retrouver ailleurs dans la liste est le minimum qu'on attende d'un
  // clic sur un en-tête.
  const std::string kept =
      pane().sel < pane().visible.size() ? pane().visible[pane().sel].name : std::string();

  pane().visible = filter_entries(pane().listing.entries, pane().filter);
  sort_entries(pane().visible, pane().sort_by, pane().sort_desc);

  if (!kept.empty()) {
    for (size_t i = 0; i < pane().visible.size(); ++i) {
      if (pane().visible[i].name == kept) {
        pane().sel = i;
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
  if (pane().visible.empty()) {
    // Remise à zéro DÉFENSIVE, et non discriminable aujourd'hui : la seule
    // façon d'avoir une liste vide est de partir d'un répertoire
    // illisible, et la sélection y vaut déjà zéro. Elle reste parce
    // qu'elle deviendra porteuse le jour où une liste non vide pourra le
    // devenir -- une suppression qui vide le dossier, par exemple.
    pane().sel = 0;
    pane().top = 0;
    return;
  }
  if (pane().sel >= pane().visible.size()) pane().sel = pane().visible.size() - 1;

  const size_t rows = static_cast<size_t>(rows_for_list());
  if (pane().sel < pane().top) pane().top = pane().sel;
  if (pane().sel >= pane().top + rows) pane().top = pane().sel - rows + 1;
  // Et jamais de page vide en bas : quand la liste rétrécit sous le
  // défilement, celui-ci doit remonter.
  if (pane().top + rows > pane().visible.size()) {
    pane().top = pane().visible.size() > rows ? pane().visible.size() - rows : 0;
  }
}

bool Files::load(const std::string& path, const std::string& came_from) {
  const DirListing probe = read_dir(path, show_hidden_);
  if (!probe.error.empty()) {
    // On RESTE où l'on est. Descendre dans un répertoire illisible pour y
    // afficher une liste vide donnerait l'impression d'un dossier vide.
    pane().status = probe.error;
    return false;
  }
  pane().listing = probe;
  pane().status.clear();
  pane().filter.clear();
  pane().marked.clear();
  pane().sel = 0;
  pane().top = 0;
  // Vidée AVANT le refiltrage : celui-ci garde la ligne choisie par son
  // NOM, et un nom de l'ancien répertoire n'a rien à faire ici.
  //
  // ÉQUIVALENTE aujourd'hui, et par un enchaînement fragile : `pane().sel` vient
  // d'être remis à zéro, donc le nom retenu est celui de la première ligne
  // de l'ancienne liste -- c'est-à-dire « .. », qui existe aussi dans la
  // nouvelle et s'y trouve déjà en tête. La garde reste parce que ce
  // raisonnement tient à l'ordre de trois lignes : déplacer `pane().sel = 0`
  // après le refiltrage suffirait à faire atterrir la sélection sur un
  // homonyme, et rien ne le dirait.
  pane().visible.clear();
  refilter();

  // Le dossier d'où l'on sort devient la sélection : remonter puis
  // redescendre doit ramener au même endroit sans le chercher des yeux.
  if (came_from.size() > path.size() && came_from.rfind(path, 0) == 0) {
    std::string rest = came_from.substr(path.size());
    while (!rest.empty() && rest.front() == '/') rest.erase(0, 1);
    const size_t slash = rest.find('/');
    const std::string child = slash == std::string::npos ? rest
                                                         : rest.substr(0, slash);
    for (size_t i = 0; i < pane().visible.size(); ++i) {
      if (pane().visible[i].name == child) {
        pane().sel = i;
        break;
      }
    }
    settle();
  }
  return true;
}

void Files::go_to(const std::string& path) {
  if (path == pane().listing.path) return;
  const std::string from = pane().listing.path;
  if (!load(path, from)) return;
  pane().back.push_back(from);
  pane().forward.clear();
}

void Files::go_back() {
  if (pane().back.empty()) return;
  const std::string from = pane().listing.path;
  const std::string target = pane().back.back();
  if (!load(target, from)) return;
  pane().back.pop_back();
  pane().forward.push_back(from);
}

void Files::go_forward() {
  if (pane().forward.empty()) return;
  const std::string from = pane().listing.path;
  const std::string target = pane().forward.back();
  if (!load(target, from)) return;
  pane().forward.pop_back();
  pane().back.push_back(from);
}

std::vector<Files::Segment> Files::path_segments(const Pane& pn, int w) const {
  // Le chemin découpé à chaque barre : « /a/b » donne « / », « a », « b »,
  // et chacun porte le chemin ABSOLU qu'il désigne.
  std::vector<Segment> out;
  const std::string& p = pn.listing.path;
  size_t i = 0;
  std::string built;
  while (i < p.size()) {
    while (i < p.size() && p[i] == '/') ++i;
    if (i >= p.size()) break;
    const size_t start = i;
    while (i < p.size() && p[i] != '/') ++i;
    built += "/" + p.substr(start, i - start);
    out.push_back(Segment{0, static_cast<int>(i - start), built});
  }
  // La racine se clique aussi : c'est la barre la plus à gauche.
  out.insert(out.begin(), Segment{0, 1, "/"});

  // CE QUI NE TIENT PAS TOMBE PAR LA GAUCHE. La fin d'un chemin porte
  // l'information, jamais son début : `/home/user/dev/…` ne dit rien
  // que `…/dev/ssh_os` ne dise mieux.
  int total = 0;
  for (const Segment& s : out) total += s.w + (s.path == "/" ? 0 : 1);
  size_t first = 0;
  while (first + 1 < out.size() && total > w) {
    total -= out[first].w + (out[first].path == "/" ? 0 : 1);
    ++first;
  }
  std::vector<Segment> kept(out.begin() + static_cast<std::ptrdiff_t>(first),
                            out.end());
  int x = first > 0 ? 1 : 0;  // la marque d'élision prend une cellule
  for (Segment& s : kept) {
    s.x = x;
    // La racine EST sa barre : lui compter une séparation de plus laisserait
    // un blanc entre « / » et le premier nom.
    x += s.w + (s.path == "/" ? 0 : 1);
  }
  return kept;
}

void Files::go_up() {
  const std::string up = parent_path(pane().listing.path);
  if (up == pane().listing.path) {
    // La racine n'a pas de parent. Ne rien faire est la seule réponse
    // honnête -- et surtout pas effacer le filtre au passage.
    return;
  }
  // Le répertoire qu'on quitte devient la sélection dans son parent :
  // remonter puis redescendre doit ramener au même endroit, sans avoir à
  // rechercher des yeux d'où l'on vient.
  std::string leaving = pane().listing.path;
  while (leaving.size() > 1 && leaving.back() == '/') leaving.pop_back();
  const size_t cut = leaving.rfind('/');
  const std::string name =
      cut == std::string::npos ? std::string() : leaving.substr(cut + 1);

  (void)name;  // `load()` repose le curseur à partir du chemin quitté
  go_to(up);
}

void Files::activate() {
  if (pane().visible.empty()) return;
  const DirEntry& e = pane().visible[pane().sel];
  if (e.kind != EntryKind::Dir) {
    // Un fichier s'ouvrirait dans l'éditeur, qui est le jalon 6. D'ici là,
    // le dire vaut mieux que ne rien faire : une touche sans effet et sans
    // explication passe pour une panne.
    pane().status = "l'editeur arrive au jalon 6";
    return;
  }
  if (e.name == "..") {
    go_up();
    return;
  }

  go_to(join_path(pane().listing.path, e.name));
}

std::string Files::touchable_selection() const {
  if (pane().visible.empty()) return {};
  const std::string& name = pane().visible[pane().sel].name;
  // `..` n'est pas un fichier de CE répertoire : le renommer renommerait
  // le parent, et le supprimer effacerait le dossier qui nous contient.
  // Personne ne demande ça en visant la première ligne.
  if (name == "..") return {};
  return name;
}

std::string Files::markable_at(size_t i) const {
  if (i >= pane().visible.size()) return {};
  // `..` n'est pas un fichier, c'est la sortie : le laisser entrer dans une
  // sélection ferait porter une copie ou une suppression sur le parent.
  if (pane().visible[i].name == "..") return {};
  return pane().visible[i].name;
}

bool Files::toggle_mark(size_t i) {
  const std::string name = markable_at(i);
  if (name.empty()) return false;
  const auto at = pane().marked.find(name);
  if (at == pane().marked.end()) {
    pane().marked.insert(name);
  } else {
    pane().marked.erase(at);
  }
  return true;
}

void Files::mark_range(size_t a, size_t b) {
  const size_t lo = std::min(a, b);
  const size_t hi = std::max(a, b);
  for (size_t i = lo; i <= hi && i < pane().visible.size(); ++i) {
    const std::string name = markable_at(i);
    if (!name.empty()) pane().marked.insert(name);
  }
}

std::vector<std::string> Files::targets() const {
  // LES MARQUÉS S'IL Y EN A, sinon la seule ligne sous la sélection. C'est
  // la règle de tous les gestionnaires, et elle évite d'avoir à marquer un
  // fichier pour agir sur lui.
  if (!pane().marked.empty()) {
    return std::vector<std::string>(pane().marked.begin(), pane().marked.end());
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
    pane().status = "un nom ne peut pas contenir de barre";
    return;
  }

  const std::string src = join_path(pane().listing.path, from);
  const std::string dst = join_path(pane().listing.path, edit_);

  // `rename()` ECRASE silencieusement une cible existante. C'est la façon
  // la plus rapide de perdre un fichier, et le noyau n'offre pas de garde
  // portable : on regarde d'abord.
  struct stat st {};
  if (::lstat(dst.c_str(), &st) == 0) {
    pane().status = "ce nom est deja pris";
    return;
  }
  if (::rename(src.c_str(), dst.c_str()) != 0) {
    pane().status = std::string("renommage impossible : ") + std::strerror(errno);
    return;
  }

  pane().status.clear();
  reload();
  // La sélection SUIT le nom renommé : le perdre de vue après l'avoir
  // renommé oblige à le rechercher pour vérifier.
  for (size_t i = 0; i < pane().visible.size(); ++i) {
    if (pane().visible[i].name == edit_) {
      pane().sel = i;
      break;
    }
  }
  settle();
}

void Files::commit_create() {
  const std::string name = edit_;
  mode_ = Mode::Normal;
  edit_.clear();
  // Un nom vide ne crée rien : une saisie ouverte par erreur ne doit pas
  // laisser un « nouveau dossier » derrière elle.
  if (name.empty()) return;
  // UN NOM AVEC UNE BARRE EST REFUSÉ. « ../ailleurs » créerait hors du
  // répertoire affiché, ce que rien à l'écran n'aurait laissé prévoir.
  if (name.find('/') != std::string::npos || name == "." || name == "..") {
    pane().status = "nom invalide : " + name;
    return;
  }

  const std::string target = join_path(pane().listing.path, name);
  // `O_EXCL` et `mkdir` refusent tous deux un nom déjà pris, et c'est le
  // noyau qui tranche : vérifier soi-même laisserait une fenêtre entre le
  // test et la création.
  int rc = 0;
  if (creating_dir_) {
    rc = ::mkdir(target.c_str(), 0755);
  } else {
    const int fd = ::open(target.c_str(),
                          O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    rc = fd < 0 ? -1 : 0;
    if (fd >= 0) ::close(fd);
  }
  if (rc != 0) {
    pane().status = std::string("creation impossible : ") + std::strerror(errno);
    return;
  }

  reload();
  // CE QU'ON VIENT DE CRÉER EST SOUS LE CURSEUR : le chercher des yeux
  // juste après l'avoir nommé est le genre de détail qui fait qu'on
  // n'utilise pas la fonction.
  for (size_t i = 0; i < pane().visible.size(); ++i) {
    if (pane().visible[i].name == name) {
      pane().sel = i;
      break;
    }
  }
  settle();
}

void Files::commit_delete() {
  const std::vector<std::string> victims = targets();
  mode_ = Mode::Normal;
  if (victims.empty()) return;

  pane().status.clear();
  int failed = 0;
  std::string first_error;
  for (const std::string& name : victims) {
    const std::string victim = join_path(pane().listing.path, name);
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
  // RELIRE D'ABORD, DIRE ENSUITE : `reload()` repose `pane().status` sur l'erreur
  // de lecture du répertoire -- vide quand tout va bien -- et effacerait
  // donc le message qu'on vient d'écrire.
  reload();
  // ON CONTINUE APRÈS UN ÉCHEC, et on dit combien : s'arrêter au premier
  // laisserait une sélection à moitié traitée dont l'utilisateur ne
  // saurait pas où elle en est.
  if (failed > 0) {
    pane().status = failed == 1 && victims.size() == 1
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
  if (mode_ == Mode::Renaming || mode_ == Mode::Creating) {
    const bool creating = mode_ == Mode::Creating;
    switch (k.key) {
      case Key::Enter:
        if (creating) {
          commit_create();
        } else {
          commit_rename();
        }
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
    if (pane().marked.empty()) {
      mark_range(0, pane().visible.empty() ? 0 : pane().visible.size() - 1);
    } else {
      pane().marked.clear();
    }
    return;
  }
  // `Espace` MARQUE ET DESCEND : on parcourt la liste en marquant au
  // passage, sans relever les doigts pour bouger. Il descend MÊME sur `..`,
  // qui ne se marque pas -- rester bloqué là donnerait l'impression que la
  // touche ne fait rien.
  if (k.key == Key::Char && k.ch == U' ' && pane().filter.empty()) {
    toggle_mark(pane().sel);
    if (pane().sel + 1 < pane().visible.size()) ++pane().sel;
    settle();
    return;
  }

  // `Alt+flèches` REMONTE ET REDESCEND L'HISTORIQUE. Sans elles, ressortir
  // d'une descente de trois niveaux demande trois retours arrière et de se
  // souvenir d'où l'on venait.
  if ((k.mods & mod::Alt) != 0) {
    if (k.key == Key::Left) {
      go_back();
      return;
    }
    if (k.key == Key::Right) {
      go_forward();
      return;
    }
  }

  switch (k.key) {
    case Key::Up:
      // `Maj+flèche` ÉTEND depuis la position courante : c'est le geste
      // qu'on essaie en premier quand on vient d'un vrai bureau.
      if ((k.mods & mod::Shift) != 0) mark_range(pane().sel, pane().sel);
      if (pane().sel > 0) --pane().sel;
      if ((k.mods & mod::Shift) != 0) mark_range(pane().sel, pane().sel);
      settle();
      return;
    case Key::Down:
      // Cette garde-ci n'est PAS porteuse -- `settle()` borne juste après,
      // et la mutation qui la retire est équivalente. Celle de la flèche
      // haut l'est, elle : `--pane().sel` sur zéro déborde par le bas et
      // enverrait la sélection À LA FIN de la liste. On les garde
      // symétriques pour que le lecteur n'ait pas à refaire cette
      // vérification.
      if ((k.mods & mod::Shift) != 0) mark_range(pane().sel, pane().sel);
      if (pane().sel + 1 < pane().visible.size()) ++pane().sel;
      if ((k.mods & mod::Shift) != 0) mark_range(pane().sel, pane().sel);
      settle();
      return;
    case Key::PgUp:
      pane().sel = pane().sel > rows ? pane().sel - rows : 0;
      settle();
      return;
    case Key::PgDn:
      pane().sel = std::min(pane().sel + rows, pane().visible.empty() ? 0 : pane().visible.size() - 1);
      settle();
      return;
    case Key::Home:
      pane().sel = 0;
      settle();
      return;
    case Key::End:
      pane().sel = pane().visible.empty() ? 0 : pane().visible.size() - 1;
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
    case Key::F3:
      // `F3` SCINDE, et rescinde referme. C'est LA fonction de Dolphin :
      // deux répertoires côte à côte, et le geste de copie qui va de l'un
      // à l'autre devient évident.
      if (split_) {
        // Refermer garde CE QU'ON REGARDE : retomber sur le répertoire de
        // l'autre panneau perdrait le travail de navigation qu'on venait
        // d'y faire.
        if (active_ == 1) panes_[0] = panes_[1];
        active_ = 0;
        split_ = false;
      } else {
        // Le second s'ouvre OÙ L'ON EST, pas à la racine : on scinde pour
        // comparer ou pour copier, et repartir de « / » ferait refaire
        // tout le chemin.
        panes_[1] = panes_[0];
        panes_[1].marked.clear();
        split_ = true;
      }
      return;
    case Key::Tab:
      // Sans scission, changer de panneau enverrait les frappes suivantes
      // dans un panneau invisible.
      if (split_) active_ = 1 - active_;
      return;
    case Key::F7:
      // `F7` un dossier, `Maj+F7` un fichier vide : le même geste, et
      // `Maj` en change la sorte.
      creating_dir_ = (k.mods & mod::Shift) == 0;
      edit_.clear();
      mode_ = Mode::Creating;
      return;
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
      if (!pane().filter.empty()) {
        pane().filter.clear();
        refilter();
        return;
      }
      pane().marked.clear();
      return;
    case Key::Backspace:
      // Le retour arrière EFFACE LE FILTRE tant qu'il en reste, et remonte
      // seulement ensuite. Remonter avec un filtre à moitié tapé ferait
      // perdre le répertoire pour une faute de frappe.
      if (!pane().filter.empty()) {
        pane().filter.pop_back();
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

  if (k.ch == U'.' && pane().filter.empty()) {
    // `.` bascule les cachés -- mais seulement hors filtre, sinon on ne
    // pourrait jamais chercher un nom qui en contient un.
    show_hidden_ = !show_hidden_;
    reload();
    return;
  }
  if (k.ch >= U' ') {
    pane().filter += encode_utf8(k.ch);
    refilter();
  }
}

void Files::on_mouse(const MouseEvent& m) {
  // CLIQUER DANS UN PANNEAU LUI DONNE LA MAIN, avant tout le reste. Sans
  // cela il faudrait viser à la souris puis appuyer sur `Tab` pour que la
  // frappe suivante y aille.
  MouseEvent e = m;
  if (split_) {
    const int left = pane_width();
    if (m.x > left) {
      active_ = 1;
      // Les coordonnées passent en LOCALES au panneau : tout ce qui suit
      // parle de sa vue, pas de la fenêtre.
      e.x = m.x - left - 1;
    } else if (m.x < left) {
      active_ = 0;
    } else {
      // La cloison n'appartient à personne.
      return;
    }
  }

  const size_t rows = static_cast<size_t>(rows_for_list());
  if (e.action == MouseAction::WheelUp) {
    pane().sel = pane().sel > 3 ? pane().sel - 3 : 0;
    settle();
    return;
  }
  if (e.action == MouseAction::WheelDown) {
    pane().sel = std::min(pane().sel + 3, pane().visible.empty() ? 0 : pane().visible.size() - 1);
    settle();
    return;
  }
  if (e.action != MouseAction::Press) return;

  // LA LIGNE 0 EST LE FIL D'ARIANE : cliquer un segment y monte.
  if (e.y == 0) {
    for (const Segment& sg : path_segments(pane(), pane_width())) {
      if (e.x < sg.x || e.x >= sg.x + sg.w) continue;
      // Le dernier segment est là où l'on est déjà : recharger pour rien
      // perdrait la sélection en cours.
      go_to(sg.path);
      return;
    }
    return;
  }

  // LA LIGNE 1 EST L'EN-TÊTE, et cliquer une colonne trie dessus. C'est le
  // geste de tous les gestionnaires, et il n'a aucun équivalent au clavier
  // qui se devine.
  if (e.y == 1) {
    const Columns c = columns(pane_width());
    if (c.date_w > 0 && e.x >= c.date_x) {
      sort_on(SortBy::Time);
    } else if (c.size_w > 0 && e.x >= c.size_x) {
      sort_on(SortBy::Size);
    } else {
      sort_on(SortBy::Name);
    }
    return;
  }

  // La barre de chemin, puis l'en-tête : la liste commence en 2.
  const int row = e.y - 2;
  if (row < 0 || static_cast<size_t>(row) >= rows) return;
  const size_t hit = pane().top + static_cast<size_t>(row);
  if (hit >= pane().visible.size()) return;

  // `Ctrl+clic` ajoute ou retire UNE entrée sans toucher au reste et sans
  // l'ouvrir : c'est ce qui distingue le clic qui choisit du clic qui agit.
  if ((e.mods & mod::Ctrl) != 0) {
    toggle_mark(hit);
    pane().sel = hit;
    settle();
    return;
  }
  // `Maj+clic` prend TOUT ce qui va de la position courante au clic.
  if ((e.mods & mod::Shift) != 0) {
    mark_range(pane().sel, hit);
    pane().sel = hit;
    settle();
    return;
  }

  // Cliquer SÉLECTIONNE ; recliquer la ligne déjà choisie l'OUVRE. Pas de
  // double-clic : l'application n'a pas à savoir compter les clics, et
  // « cliquer deux fois » se découvre tout seul.
  if (hit == pane().sel) {
    activate();
    return;
  }
  pane().sel = hit;
  settle();
}

bool Files::wants_cursor(Pos& out) const {
  if (pane().visible.empty()) return false;
  out = Pos{0, 2 + static_cast<int>(pane().sel - pane().top)};
  return true;
}

void Files::render(View v) {
  if (!split_) {
    draw_pane(v, panes_[0], true);
    return;
  }
  const int left = pane_width();
  draw_pane(v.sub(Rect{0, 0, left, v.h()}), panes_[0], active_ == 0);

  // La cloison, en creux : elle sépare sans attirer l'œil, et c'est la
  // seule chose à l'écran qui dise qu'il y a deux vues et non une liste
  // à deux colonnes.
  Style wall;
  wall.attrs = attr::Dim;
  for (int y = 0; y < v.h(); ++y) v.put(left, y, U'\u2502', wall);

  const int right = v.w() - left - 1;
  if (right > 0) {
    draw_pane(v.sub(Rect{left + 1, 0, right, v.h()}), panes_[1], active_ == 1);
  }
}

void Files::draw_pane(View v, const Pane& pn, bool focused) {
  const int w = v.w();
  const int h = v.h();
  // Garde DÉFENSIVE, non discriminable : la `View` clippe déjà tout ce
  // qu'on lui écrit, donc peindre une fenêtre de largeur nulle ne fait
  // rien de mal. Elle reste parce qu'elle évite de parcourir la liste pour
  // rien, et parce qu'un futur calcul de géométrie pourrait, lui, diviser.
  if (w <= 0 || h <= 0) return;

  // LE FIL D'ARIANE. En gras plutôt qu'en couleur : il doit se distinguer
  // sur les seize couleurs comme sur les 16 millions. Chaque segment se
  // clique -- un chemin qui ne sert qu'à lire oblige à remonter d'un cran à
  // la fois.
  //
  // Le panneau qui N'A PAS la main l'écrit en creux : une fenêtre scindée
  // doit dire où la frappe suivante ira, et la barre de sélection ne suffit
  // pas -- les deux panneaux en ont une.
  Style path_style;
  path_style.attrs = focused ? attr::Bold : attr::Dim;
  const std::vector<Segment> segs = path_segments(pn, w);
  if (!segs.empty() && segs.front().x > 0) {
    Style faint;
    faint.attrs = attr::Dim;
    v.text(0, 0, "\u2026", faint);
  }
  for (size_t i = 0; i < segs.size(); ++i) {
    const Segment& sg = segs[i];
    const std::string label =
        sg.path == "/" ? std::string("/") : sg.path.substr(sg.path.rfind('/') + 1);
    v.text(sg.x, 0, label, path_style);
    if (i + 1 < segs.size() && sg.path != "/") {
      Style sep;
      sep.attrs = attr::Dim;
      v.text(sg.x + sg.w, 0, "/", sep);
    }
  }

  // L'EN-TÊTE : il nomme les colonnes, et il est la seule chose qui dise
  // sur quoi la liste est triée. La flèche marque celle qui porte le tri.
  const Columns c = columns(w);
  Style head;
  head.attrs = attr::Underline;
  const std::string arrow = pn.sort_desc ? "v" : "^";
  v.text(0, 1, elide_right(pn.sort_by == SortBy::Name ? "Nom " + arrow : "Nom",
                           c.name_w),
         head);
  if (c.size_w > 0) {
    v.text(c.size_x, 1,
           right_align(pn.sort_by == SortBy::Size ? "Taille " + arrow : "Taille",
                       c.size_w),
           head);
  }
  if (c.date_w > 0) {
    v.text(c.date_x, 1,
           pn.sort_by == SortBy::Time ? "Date " + arrow : "Date", head);
  }

  const int rows = rows_for_list();
  for (int i = 0; i < rows; ++i) {
    const size_t idx = pn.top + static_cast<size_t>(i);
    // `break` plutôt que `continue` : les index ne font que croître, donc
    // les deux donnent le même résultat -- la mutation est équivalente. On
    // s'arrête parce que c'est ce que le code veut dire.
    if (idx >= pn.visible.size()) break;
    const DirEntry& e = pn.visible[idx];

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
    const bool chosen = pn.marked.count(e.name) != 0;
    if (idx == pn.sel) {
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
  } else if (mode_ == Mode::Creating) {
    // L'INVITE DIT CE QU'ON CRÉE : « nouveau nom » pendant qu'on nomme un
    // dossier laisserait croire à un renommage.
    status_style.attrs = attr::Reverse;
    bottom = std::string(creating_dir_ ? "nouveau dossier: " : "nouveau fichier: ") +
             edit_;
  } else if (mode_ == Mode::Confirming) {
    status_style.attrs = attr::Reverse;
    status_style.fg = Color::indexed(1);
    // COMBIEN, PAS SEULEMENT « quoi ». « supprimer ? » sur une sélection de
    // trente fichiers ne dit pas ce qu'on s'apprête à perdre.
    bottom = pn.marked.empty()
                 ? "supprimer " +
                       (pn.visible.empty() ? std::string() : pn.visible[pn.sel].name) +
                       " ? (o/n)"
                 : "supprimer " + std::to_string(pn.marked.size()) +
                       " elements ? (o/n)";
  } else if (!pn.status.empty()) {
    status_style.fg = Color::indexed(1);
    bottom = pn.status;
  } else if (!pn.filter.empty()) {
    status_style.attrs = attr::Bold;
    bottom = "filtre: " + pn.filter;
  } else if (!pn.marked.empty()) {
    // COMBIEN, ET COMBIEN ÇA PÈSE. Une sélection qu'on ne voit pas est une
    // sélection dont on ne se souvient plus au moment d'appuyer sur Suppr.
    uint64_t bytes = 0;
    for (const DirEntry& e : pn.listing.entries) {
      if (pn.marked.count(e.name) != 0) bytes += e.size;
    }
    status_style.attrs = attr::Bold;
    status_style.fg = Color::indexed(3);
    bottom = std::to_string(pn.marked.size()) + " selectionnes, " +
             human_size(bytes);
  }
  if (!bottom.empty()) v.text(0, h - 1, elide_right(bottom, w), status_style);
}

}  // namespace sshos
