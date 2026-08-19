#include "apps/terminal.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <string>

#include "input/encode.hpp"
#include "pty/env.hpp"
#include "common/utf8.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"
#include "vt/reply.hpp"

namespace sshos {
namespace {

// LA BARRE D'ONGLETS EST TOUJOURS LÀ, même avec un seul onglet. Elle coûte
// une ligne de grille, et c'est assumé : elle porte le `+`, qui est la
// SEULE voie à la souris vers un second onglet. Une barre qui n'
// apparaîtrait qu'au deuxième onglet demanderait de connaître le raccourci
// clavier qui donne ce deuxième onglet.
constexpr int kBarRows = 1;

// Un nom d'onglet plus long que ça n'apporte plus rien : la barre l'élide
// de toute façon, et le retenir sans borne ferait grossir la session à
// chaque frappe d'un renommage qu'on n'a pas validé.
constexpr size_t kMaxName = 64;

// Un chemin est plus long qu'un nom d'onglet. PATH_MAX vaut 4096 sous
// Linux, mais un chemin qu'on tape à la main n'en approche jamais : ce qui
// compte est qu'une touche restée enfoncée ne fasse pas grossir la session
// sans limite.
constexpr size_t kMaxPath = 512;

// La croix de fermeture. Un « x » ASCII se lit comme une LETTRE au milieu
// des noms d'onglets -- la sonde a montré « 1:root@dockernx x », où deux
// des trois « x » de la ligne appartenaient au nom de la machine. Le
// projet écrit déjà de l'Unicode depuis les applications (l'élision de
// Fichiers pose un U+2026 sans se demander ce que le client sait lire).
constexpr char kCross[] = "\u00d7";

// LA ROUE, EN ASCII PUR. Une vraie roue dentée (U+2699) est un caractère
// d'emoji : la moitié des terminaux la rendent sur deux colonnes, l'autre
// moitié sur une, et la barre se décalerait d'une cellule selon la machine
// -- un bouton qu'on ne peut plus cliquer là où on le voit. Trois ASCII se
// dessinent partout pareil, et les crochets disent « ceci se clique ».
constexpr char kGear[] = "[*]";
constexpr int kGearCells = 3;

// Le premier paramètre d'une séquence, avec son défaut, et jamais moins de
// 1 : `CSI 0 A` doit monter d'une ligne, pas de zéro.
int count_of(const Params& p, size_t index = 0) {
  return std::max(1, param_or(p, index, 1));
}

// L'encodage SGR 1006 d'un événement souris. Coordonnées LOCALES et
// 1-indexées : l'invité croit occuper tout un terminal, et lui donner les
// coordonnées de l'écran ferait cliquer `htop` à côté.
std::string encode_mouse_sgr(const MouseEvent& m, int x, int y) {
  int cb = m.button;
  switch (m.action) {
    case MouseAction::WheelUp:
      cb = 64;
      break;
    case MouseAction::WheelDown:
      cb = 65;
      break;
    case MouseAction::Motion:
      cb = m.button + 32;
      break;
    default:
      break;
  }
  if ((m.mods & mod::Shift) != 0) cb += 4;
  if ((m.mods & mod::Alt) != 0) cb += 8;
  if ((m.mods & mod::Ctrl) != 0) cb += 16;
  // Le `m` final est ce qui distingue un relâchement d'un enfoncement :
  // c'est TOUT l'intérêt de l'encodage 1006, l'ancien X10 ne sachant pas
  // dire quel bouton on relâche.
  const char final_byte = m.action == MouseAction::Release ? 'm' : 'M';
  return "\033[<" + std::to_string(cb) + ";" + std::to_string(x + 1) + ";" +
         std::to_string(y + 1) + final_byte;
}

// CE QU'UN COLLAGE A LE DROIT DE CONTENIR.
//
// Un texte colle vient du DEHORS -- d'une page web, d'un courriel, de
// n'importe ou -- et il ne doit pas pouvoir piloter le terminal. On garde
// la tabulation et les fins de ligne, qui sont du texte ; on retire tout le
// reste des octets de controle.
//
// L'ECHAPPEMENT EST LE DANGER PRINCIPAL, et il y en a deux : un « \033]0; »
// renommerait la fenetre a l'insu de tous, et surtout un « \033[201~ »
// fabrique REFERMERAIT l'encadrement du collage -- tout ce qui suit
// redeviendrait de la frappe, donc des commandes EXECUTEES. C'est la faille
// classique du collage encadre, et la seule parade est de ne jamais laisser
// passer l'octet d'echappement.
//
// Les retours a la ligne, eux, passent : coller trois lignes dans un shell
// les execute, c'est exactement ce que l'utilisateur demande en le faisant.
std::string clean_paste(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == 0x7f) continue;              // DEL
    if (u >= 0x20 || c == '\t' || c == '\n' || c == '\r') out.push_back(c);
  }
  return out;
}

// La meme chose pour une saisie du bureau -- nom d'onglet, dossier de
// depart : elle tient sur UNE ligne, et une fin de ligne collee dedans
// n'aurait aucun sens.
std::string clean_paste_one_line(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : clean_paste(text)) {
    if (c == '\n' || c == '\r' || c == '\t') continue;
    out.push_back(c);
  }
  return out;
}

}  // namespace

Terminal::Terminal() {
  // UN ONGLET DES LA NAISSANCE : le reste du code n'a alors jamais a se
  // demander s'il y en a un, et `active()` n'a pas de cas degenere.
  tabs_.push_back(std::make_unique<Tab>(*this));
  active().screen.set_scrollback(&active().history);
}

Terminal::Terminal(std::vector<std::string> argv) : argv_(std::move(argv)) {
  tabs_.push_back(std::make_unique<Tab>(*this));
  active().screen.set_scrollback(&active().history);
}

Terminal::Tab& Terminal::target() {
  return feeding_ != nullptr ? *feeding_ : active();
}

const std::string& Terminal::tab_name(size_t i) const {
  const Tab& t = *tabs_[i];
  // Le nom CHOISI gagne toujours. Un `bash` repose son titre à chaque
  // invite : sans cette priorité, un onglet renommé reprendrait son nom
  // d'origine à la première commande. Vide veut dire « aucun des deux »,
  // et c'est à l'appelant de décider ce qu'il montre alors -- la barre y
  // met le numéro seul, le cadre de la fenêtre n'y met rien.
  return t.custom_title.empty() ? t.guest_title : t.custom_title;
}

std::string Terminal::tab_label_for_tests(size_t i) const {
  if (i >= tabs_.size()) return {};
  const std::string& name = tab_name(i);
  return name.empty() ? std::to_string(i + 1) : name;
}

std::vector<Terminal::Slot> Terminal::bar_slots() const {
  std::vector<Slot> out;
  const int w = std::max(1, size_.w);

  // PENDANT LA SAISIE DU CHEMIN, la barre EST le champ. Un chemin ne tient
  // pas dans une case d'onglet, et les onglets ne servent à rien tant qu'on
  // écrit : les cacher donne toute la largeur au seul geste en cours.
  if (mode_ == Mode::EditingPath) {
    out.push_back(Slot{0, w, 0, SlotKind::Settings, std::string(kGear) + " " + edit_});
    return out;
  }

  const int n = static_cast<int>(tabs_.size());
  // La dernière colonne porte le `+`, les trois d'avant la roue, et celle
  // qui reste les sépare du dernier onglet : le reste est aux onglets.
  const int avail = std::max(1, w - (kGearCells + 3));
  // UN SEUL ONGLET N'A PAS DE CROIX. La fenêtre a déjà son [×], et une
  // croix d'onglet qui fermerait la fenêtre entière serait un piège.
  const bool crosses = n > 1;
  const int per = std::max(3, avail / n);
  // « espace nom [croix] espace » : la croix ne coûte que si elle existe.
  const int name_max = std::max(1, per - (crosses ? 3 : 2));

  int x = 0;
  for (int i = 0; i < n; ++i) {
    // PENDANT LE RENOMMAGE, l'onglet montre ce qu'on tape, et sa case
    // s'élargit avec. La calculer sur l'ancien nom couperait le nouveau au
    // troisième caractère, et le curseur n'aurait nulle part où aller.
    const bool editing =
        mode_ == Mode::Renaming && static_cast<size_t>(i) == active_;
    // LE NUMÉRO TOUJOURS, le nom ensuite. Sans le numéro, un onglet unique
    // ne montre que le titre du shell -- que la barre de titre de la
    // fenêtre, deux lignes plus haut, affiche déjà : la barre passait pour
    // une redite au lieu de dire où l'on est.
    const std::string num = std::to_string(i + 1);
    const std::string& raw = editing ? edit_ : tab_name(static_cast<size_t>(i));
    const std::string body =
        raw.empty() ? num
                    : num + ":" +
                          elide_to_cells(raw, std::max(1, name_max -
                                                              text_cells(num) - 1),
                                         "\u2026");
    const std::string text = " " + body + " ";
    const int cells = text_cells(text) + (crosses ? 1 : 0);
    // Ce qui ne tient pas n'est PAS dessiné à moitié : un onglet coupé par
    // le bord se cliquerait là où il n'est plus.
    if (x + cells > avail) break;
    out.push_back(Slot{x, text_cells(text), static_cast<size_t>(i),
                       SlotKind::Select, text});
    x += text_cells(text);
    if (crosses) {
      out.push_back(
          Slot{x, 1, static_cast<size_t>(i), SlotKind::Close, kCross});
      x += 1;
    }
  }
  // LA ROUE JUSTE AVANT LE `+`, jamais à sa place : le `+` reste à
  // l'extrême droite, là où la main le cherche depuis toujours.
  if (w > kGearCells + 1) {
    out.push_back(Slot{w - 1 - kGearCells, kGearCells, 0, SlotKind::Settings, kGear});
  }
  out.push_back(Slot{w - 1, 1, 0, SlotKind::New, "+"});
  return out;
}

int Terminal::settings_column_for_tests() const {
  for (const Slot& s : bar_slots()) {
    if (s.kind == SlotKind::Settings) return s.x;
  }
  return -1;
}

Terminal::~Terminal() {
  // `Pty::shutdown()`, appelée par son destructeur, emporte le groupe de
  // chaque onglet : SIGHUP, fermeture du maître, SIGKILL. Ceci ne fait que
  // retirer la surveillance avant que le descripteur ne parte, comme
  // partout ailleurs.
  for (const auto& t : tabs_) {
    if (host_ != nullptr && t->watching) host_->unwatch(t->token);
  }
}

void Terminal::attach(Host& host) {
  host_ = &host;
  open_tab_into(active());
  relayout();
  retitle();
}

bool Terminal::open_tab() {
  tabs_.push_back(std::make_unique<Tab>(*this));
  Tab& t = *tabs_.back();
  t.screen.set_scrollback(&t.history);
  if (host_ != nullptr && !open_tab_into(t)) {
    // Le shell n'a pas démarré : on reste sur celui qui marche plutôt que
    // de poser un onglet mort au premier plan.
    tabs_.pop_back();
    return false;
  }
  // `relayout()` est la SEULE source de la géométrie : les `spec.rows` du
  // spawn ci-dessus ne sont qu'une valeur de départ, et c'est elle qui les
  // corrige -- la mutation qui les fausse est donc équivalente.
  relayout();
  // Le nouvel onglet passe DEVANT : on ne l'ouvre pas pour continuer à
  // regarder l'ancien.
  active_ = tabs_.size() - 1;
  retitle();
  if (host_ != nullptr) host_->invalidate();
  return true;
}

void Terminal::select_tab(size_t i) {
  if (i >= tabs_.size()) return;
  active_ = i;
  retitle();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::cycle_tab(int d) {
  const int n = static_cast<int>(tabs_.size());
  const int at = static_cast<int>(active_);
  select_tab(static_cast<size_t>(((at + d) % n + n) % n));
}

void Terminal::close_tab(size_t i) {
  if (i >= tabs_.size()) return;
  if (tabs_.size() == 1) {
    // LE DERNIER ONGLET FERME LA FENÊTRE, et il passe par le [×] habituel :
    // un `make` en cours doit faire poser la question, ici comme ailleurs.
    // L'onglet reste debout tant que la réponse n'est pas venue.
    if (host_ != nullptr) host_->request_close();
    return;
  }
  std::unique_ptr<Tab> dying = std::move(tabs_[i]);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
  if (active_ >= tabs_.size()) active_ = tabs_.size() - 1;

  if (host_ != nullptr && dying->watching) host_->unwatch(dying->token);
  dying->watching = false;
  // La MÊME fermeture que celle d'une fenêtre : elle vit dans `Pty` pour
  // qu'un onglet fermé et une fenêtre fermée ne puissent pas diverger.
  dying->pty.shutdown();
  // Récoltable tout de suite ? Sinon il attend en antichambre le SIGCHLD
  // qui vient.
  if (dying->pty.pid() > 0 && !dying->pty.try_reap()) {
    closing_.push_back(std::move(dying));
  }
  retitle();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::retitle() {
  if (host_ == nullptr) return;
  // L'APPLICATION D'ABORD, L'ONGLET ENTRE PARENTHÈSES. Le seul nom de
  // l'onglet ne disait plus de quelle application il s'agissait : une
  // fenêtre nommée « build » ressemblait à n'importe quelle autre, et le
  // titre du shell -- que la barre d'onglets affiche déjà -- occupait tout
  // le cadre.
  const std::string& name = tab_name(active_);
  // Sans nom, « Terminal » tout court : ni parenthèse vide, ni numéro
  // d'onglet pour ne rien dire de plus que la barre.
  host_->set_title(name.empty() ? "Terminal" : "Terminal (" + name + ")");
}

void Terminal::begin_rename() {
  mode_ = Mode::Renaming;
  // On repart du nom CHOISI, pas du titre de l'invité : le vider est ce qui
  // rend l'onglet à son titre automatique, et pré-remplir avec ce dernier
  // obligerait à effacer trente caractères pour y revenir.
  edit_ = active().custom_title;
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::commit_rename() {
  if (mode_ != Mode::Renaming) return;
  // L'ONGLET RENOMMÉ EST TOUJOURS L'ACTIF, et l'invariant tient parce que
  // rien ne peut changer d'onglet pendant une saisie sans passer d'abord
  // par ici : le clavier va tout entier au renommage, et `on_mouse` valide
  // avant de traiter le clic.
  active().custom_title = edit_;
  mode_ = Mode::Normal;
  edit_.clear();
  retitle();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::begin_path_edit() {
  mode_ = Mode::EditingPath;
  // ON REPART DE CE QU'IL A TAPE, pas du chemin développé : lui remontrer
  // « /home/moi/dev » quand il avait écrit « ~/dev » donnerait l'impression
  // que le bureau a changé son choix dans son dos.
  edit_ = host_ != nullptr ? host_->configured_start_dir() : std::string();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::commit_path_edit() {
  if (mode_ != Mode::EditingPath) return;
  if (host_ != nullptr) host_->set_start_dir(edit_);
  mode_ = Mode::Normal;
  edit_.clear();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::path_key(const KeyEvent& k) {
  switch (k.key) {
    case Key::Enter:
      commit_path_edit();
      break;
    case Key::Escape:
      // RIEN N'EST POSE. Une saisie qu'on abandonne ne doit rien laisser
      // derrière elle, surtout pas un réglage écrit sur disque.
      mode_ = Mode::Normal;
      edit_.clear();
      break;
    case Key::Backspace: {
      // Un CARACTÈRE, pas un octet : un chemin peut porter de l'accentué,
      // et couper une séquence UTF-8 en deux laisserait un demi-caractère.
      while (!edit_.empty() &&
             (static_cast<unsigned char>(edit_.back()) & 0xc0) == 0x80) {
        edit_.pop_back();
      }
      if (!edit_.empty()) edit_.pop_back();
      break;
    }
    case Key::Char:
      // Un chemin est plus long qu'un nom d'onglet, mais pas sans fin : la
      // borne existe pour qu'une touche restée enfoncée ne fasse pas
      // grossir la session sans limite.
      if (edit_.size() < kMaxPath) edit_ += encode_utf8(k.ch);
      break;
    default:
      // Tout le reste est AVALÉ, même règle que le renommage : une flèche
      // qui partirait à l'invité déplacerait son curseur à l'aveugle.
      break;
  }
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::rename_key(const KeyEvent& k) {
  switch (k.key) {
    case Key::Enter:
      commit_rename();
      break;
    case Key::Escape:
      mode_ = Mode::Normal;
      edit_.clear();
      break;
    case Key::Backspace: {
      // On retire un CARACTÈRE, pas un octet : couper une séquence UTF-8 en
      // deux laisserait un demi-caractère dans le nom.
      while (!edit_.empty() &&
             (static_cast<unsigned char>(edit_.back()) & 0xc0) == 0x80) {
        edit_.pop_back();
      }
      if (!edit_.empty()) edit_.pop_back();
      break;
    }
    case Key::Char:
      if (edit_.size() < kMaxName) edit_ += encode_utf8(k.ch);
      break;
    default:
      // Tout le reste est AVALÉ : le renommage est un mode, et une flèche
      // qui partirait à l'invité pendant qu'on tape un nom déplacerait son
      // curseur à l'aveugle.
      break;
  }
  if (host_ != nullptr) host_->invalidate();
}

bool Terminal::open_tab_into(Tab& t) {
  PtySpawn spec;
  if (argv_.empty()) {
    // Le shell vient de `getpwuid()`, PAS de `$SHELL` : l'environnement du
    // démon est un fossile de la première session SSH.
    spec.path = login_shell();
    spec.argv = {spec.path, "-l"};
  } else {
    spec.path = argv_.front();
    spec.argv = argv_;
  }
  spec.env = child_env(daemon_env(), EnvDelta{});
  // OU IL COMMENCE. Le bureau seul le sait : c'est un réglage de
  // l'utilisateur, écrit sur disque, et une application n'a pas à savoir où
  // ce fichier vit. Vide -- un hôte de test -- veut dire « là où on est ».
  if (host_ != nullptr) spec.cwd = host_->start_dir();
  spec.cols = static_cast<unsigned short>(std::max(1, size_.w));
  spec.rows = static_cast<unsigned short>(std::max(1, size_.h - kBarRows));

  t.spawn_error = t.pty.spawn(spec);
  if (!t.spawn_error.empty()) return false;

  if (host_ != nullptr) {
    t.token = host_->watch(t.pty.master(), EPOLLIN);
    t.watching = true;
    // La récolte est globale au démon : l'application ne fait que dire à
    // qui appartient ce pid.
    host_->watch_child(t.pty.pid());
  }
  return true;
}

void Terminal::to_guest(std::string_view bytes) {
  if (bytes.empty()) return;
  // `target()`, PAS `active()`. Une réponse à `CSI 6 n` ou à `CSI c` part
  // pendant qu'on parse, et elle appartient à l'onglet qui a POSÉ la
  // question : l'envoyer sur le maître de l'onglet regardé ferait apparaître
  // « ;1R » au milieu de l'invite d'à côté. Hors lecture -- une frappe, un
  // clic -- `target()` rend l'onglet actif, qui est bien le destinataire.
  Tab& t = target();
  if (t.pty.master() < 0) {
    // Pas de PTY : en test, ou après la fermeture du maître. On retient au
    // lieu de perdre -- c'est ce qui rend l'encodage vérifiable sans
    // lancer de shell.
    t.pending.append(bytes);
    return;
  }
  t.pty.write(bytes.data(), bytes.size());
}

std::string Terminal::take_written_for_tests() {
  std::string out;
  out.swap(active().pending);
  return out;
}

void Terminal::feed_for_tests(std::string_view bytes) {
  feed_tab_for_tests(active_, bytes);
}

void Terminal::feed_tab_for_tests(size_t i, std::string_view bytes) {
  if (i >= tabs_.size()) return;
  // On reproduit ce que fait `on_io` : poser l'onglet nourri, parser, le
  // reprendre. Sans ce jalonnement, aucun cas ne pourrait distinguer
  // l'onglet d'où viennent les octets de celui qu'on regarde.
  feeding_ = tabs_[i].get();
  tabs_[i]->parser.feed(bytes);
  feeding_ = nullptr;
}

std::string Terminal::take_written_for_tests(size_t i) {
  std::string out;
  if (i < tabs_.size()) out.swap(tabs_[i]->pending);
  return out;
}

std::vector<int> Terminal::cross_columns_for_tests() const {
  std::vector<int> out;
  for (const Slot& s : bar_slots()) {
    if (s.kind == SlotKind::Close) out.push_back(s.x);
  }
  return out;
}

IoStatus Terminal::on_io(uint64_t token, uint32_t events) {
  (void)events;
  Tab* tab = nullptr;
  for (const auto& t : tabs_) {
    if (t->watching && t->token == token) {
      tab = t.get();
      break;
    }
  }
  if (tab == nullptr) return IoStatus::Ok;

  // ON DIT AU PUITS DE QUI VIENNENT LES OCTETS. Le parseur de chaque onglet
  // appelle le même `Terminal`, qui sans ceci écrirait dans la grille de
  // l'onglet regardé -- un `ls` lancé dans le second onglet repeindrait le
  // premier.
  feeding_ = tab;

  // On DRAINE avant de conclure quoi que ce soit. Les noyaux récents
  // livrent d'abord ce qui restait en tampon, puis rendent EIO : fermer sur
  // le premier réveil jetterait le dernier mot de l'invité.
  for (;;) {
    char buf[8192];
    const ssize_t n = tab->pty.read(buf, sizeof buf);
    if (n > 0) {
      tab->parser.feed(std::string_view(buf, static_cast<size_t>(n)));
      continue;
    }
    if (n == 0) {
      // Fin de fichier : l'esclave n'est plus ouvert nulle part.
      tab->pty.note_eof();
      feeding_ = nullptr;
      if (host_ != nullptr) host_->invalidate();
      return IoStatus::Closed;
    }
    break;  // rien de plus à lire pour l'instant
  }
  feeding_ = nullptr;
  if (host_ != nullptr) host_->invalidate();
  return IoStatus::Ok;
}

void Terminal::on_child_exit(int status) {
  (void)status;
  // On ne sait PAS quel onglet vient de perdre son shell : la récolte est
  // globale au démon, et `waitpid(WNOHANG)` sur un pid encore vivant ne
  // coûte qu'un appel système qui ne fait rien.
  for (const auto& t : tabs_) t->pty.try_reap();
  // Les onglets fermés attendent ici leur récolte, et partent pour de bon
  // dès qu'elle a eu lieu.
  for (auto& t : closing_) t->pty.try_reap();
  closing_.erase(std::remove_if(closing_.begin(), closing_.end(),
                                [](const std::unique_ptr<Tab>& t) {
                                  return t->pty.exited();
                                }),
                 closing_.end());
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::relayout() {
  // LA BARRE MANGE UNE LIGNE, et donc tous les onglets rétrécissent quand
  // le second s'ouvre -- pas seulement celui qu'on regarde. Un `vim` laissé
  // dans un onglet de fond qui garderait l'ancienne hauteur peindrait sa
  // dernière ligne sous la fenêtre.
  const int w = std::max(1, size_.w);
  const int h = std::max(1, size_.h - kBarRows);
  for (const auto& t : tabs_) {
    t->screen.resize(w, h);
    // La taille faisant autorité est celle du PTY : c'est le noyau qui
    // envoie `SIGWINCH` au groupe au premier plan, pas nous.
    t->pty.resize(static_cast<unsigned short>(w), static_cast<unsigned short>(h));
  }
}

void Terminal::on_resize(Size s) {
  if (s.w <= 0 || s.h <= 0) return;
  size_ = s;
  relayout();
}

void Terminal::on_key(const KeyEvent& k) {
  if (mode_ == Mode::Renaming) {
    rename_key(k);
    return;
  }
  if (mode_ == Mode::EditingPath) {
    path_key(k);
    return;
  }

  // LES GESTES D'ONGLET D'ABORD, et ils ne descendent JAMAIS à l'invité :
  // `Alt+t` transpose deux mots dans readline, et le laisser passer en plus
  // d'ouvrir un onglet ferait les deux. `Alt` plutôt que `Ctrl`, qui
  // appartient à l'invité tout entier -- et avant la garde de mort ci-
  // dessous, pour qu'un onglet dont le shell est parti reste utilisable.
  if ((k.mods & mod::Alt) != 0) {
    if (k.key == Key::Char && k.ch == U't') {
      open_tab();
      return;
    }
    if (k.key == Key::Char && k.ch == U'w') {
      close_tab(active_);
      return;
    }
    if (k.key == Key::Left) {
      cycle_tab(-1);
      return;
    }
    if (k.key == Key::Right) {
      cycle_tab(1);
      return;
    }
  }
  if (k.key == Key::F2) {
    begin_rename();
    return;
  }

  Tab& t = active();
  if (t.pty.exited()) {
    // Un terminal mort ne prend plus de frappes : `Entrée` le ferme, tout
    // le reste est ignoré. La fenêtre RESTE ouverte jusque-là, pour qu'on
    // puisse lire la dernière erreur.
    if (k.key == Key::Enter && host_ != nullptr) host_->request_close();
    return;
  }

  // `Maj+PgPréc` / `PgSuiv` consultent l'historique. Ils ne vont jamais à
  // l'invité : c'est notre historique, pas le sien.
  if ((k.mods & mod::Shift) != 0 && !t.screen.alt_screen()) {
    if (k.key == Key::PgUp) {
      t.history.scroll_back(static_cast<size_t>(std::max(1, size_.h / 2)));
      if (host_ != nullptr) host_->invalidate();
      return;
    }
    if (k.key == Key::PgDn) {
      t.history.scroll_forward(static_cast<size_t>(std::max(1, size_.h / 2)));
      if (host_ != nullptr) host_->invalidate();
      return;
    }
  }

  const std::string bytes = encode_key(k, t.modes.cursor_keys_application);
  if (bytes.empty()) return;
  // Écrire, c'est revenir au présent : personne ne tape en aveugle dans
  // une page d'historique.
  t.history.scroll_to_bottom();
  to_guest(bytes);
}

void Terminal::bar_click(int x) {
  for (const Slot& s : bar_slots()) {
    if (x < s.x || x >= s.x + s.w) continue;
    switch (s.kind) {
      case SlotKind::New:
        open_tab();
        return;
      case SlotKind::Settings:
        begin_path_edit();
        return;
      case SlotKind::Close:
        close_tab(s.tab);
        return;
      case SlotKind::Select:
        // CLIQUER L'ONGLET DÉJÀ REGARDÉ LE RENOMME. C'est la seule voie au
        // renommage qui ne demande pas de connaître `F2`, et l'utilisateur
        // pilote à la souris.
        if (s.tab == active_) {
          begin_rename();
        } else {
          select_tab(s.tab);
        }
        return;
    }
  }
}

void Terminal::on_mouse(const MouseEvent& m) {
  // UN CLIC, OÙ QU'IL SOIT, VALIDE LE RENOMMAGE EN COURS. Sans cette
  // règle, le mode restait ouvert indéfiniment : le texte en cours se
  // dessine sur l'onglet ACTIF, il suivait donc la sélection, et l'onglet
  // qu'on venait de nommer revenait à son titre d'invité. C'est ce que
  // font toutes les saisies en place -- on est passé à autre chose, le nom
  // tapé ne reste pas en suspens.
  if (m.action == MouseAction::Press) {
    commit_rename();
    commit_path_edit();
  }

  // LA BARRE EST AU BUREAU, PAS À L'INVITÉ -- et avant la garde de mort
  // ci-dessous, sans quoi un clic sur `+` dans un onglet dont le shell est
  // parti fermerait la fenêtre au lieu d'ouvrir un onglet.
  if (m.y < kBarRows) {
    if (m.action == MouseAction::Press) bar_click(m.x);
    return;
  }

  Tab& t = active();
  if (t.pty.exited()) {
    // Un CLIC ferme aussi un terminal mort. Une fonction qui n'a qu'un
    // raccourci clavier est une fonction incomplète.
    if (m.action == MouseAction::Press && host_ != nullptr) {
      host_->request_close();
    }
    return;
  }

  const bool wheel = m.action == MouseAction::WheelUp ||
                     m.action == MouseAction::WheelDown;
  // La molette fait défiler NOTRE historique tant que l'écran alterné est
  // inactif ; quand il est actif, elle part à l'invité -- c'est ce que font
  // les vrais émulateurs, et c'est ce qui fait défiler `less` sans que
  // notre historique s'en mêle.
  if (wheel && !t.screen.alt_screen() && t.modes.tracking() == MouseTracking::None) {
    const size_t step = 3;
    if (m.action == MouseAction::WheelUp) {
      t.history.scroll_back(step);
    } else {
      t.history.scroll_forward(step);
    }
    if (host_ != nullptr) host_->invalidate();
    return;
  }

  if (t.modes.tracking() == MouseTracking::None) return;
  if (t.modes.tracking() == MouseTracking::Click &&
      m.action == MouseAction::Motion) {
    // 1000 ne rapporte PAS le mouvement. Le rapporter quand même noierait
    // le lien SSH d'un paquet par cellule parcourue.
    return;
  }
  // Coordonnées LOCALES À LA GRILLE : la barre d'onglets n'existe pas pour
  // l'invité, et lui donner un `y` décalé ferait cliquer `htop` une ligne
  // trop bas.
  to_guest(encode_mouse_sgr(m, m.x, m.y - kBarRows));
}

void Terminal::on_paste(std::string_view text, bool complete) {
  // PENDANT UNE SAISIE DU BUREAU, le collage va dans le CHAMP, pas a
  // l'invite : c'est meme le geste le plus utile des deux -- coller un
  // chemin dans le champ de la roue plutot que le retaper.
  if (mode_ == Mode::Renaming || mode_ == Mode::EditingPath) {
    const size_t plafond = mode_ == Mode::Renaming ? kMaxName : kMaxPath;
    const std::string ajout = clean_paste_one_line(text);
    if (edit_.size() < plafond) {
      edit_ += ajout.substr(0, plafond - edit_.size());
    }
    if (host_ != nullptr) host_->invalidate();
    return;
  }

  Tab& t = active();
  if (t.pty.exited()) return;

  // Coller, c'est ecrire : on revient au present, comme pour une frappe.
  // Personne ne colle en aveugle dans une page d'historique.
  t.history.scroll_to_bottom();

  if (!pasting_) {
    // LE CHOIX D'ENCADRER SE FIGE ICI, a l'ouverture, et vaut pour tous les
    // morceaux : un invite qui basculerait le mode 2004 au milieu laisserait
    // sinon un « \033[200~ » sans son « \033[201~ », et le shell resterait
    // en attente pour toujours.
    paste_bracketed_ = t.modes.bracketed_paste;
    if (paste_bracketed_) to_guest("\033[200~");
  }
  pasting_ = !complete;

  to_guest(clean_paste(text));

  if (complete && paste_bracketed_) to_guest("\033[201~");
}

bool Terminal::wants_cursor(Pos& out) const {
  if (mode_ == Mode::Renaming) {
    // Pendant un renommage, le curseur est DANS LA BARRE, au bout de ce
    // qu'on tape : une saisie sans curseur a l'air d'une application figée.
    for (const Slot& s : bar_slots()) {
      if (s.kind != SlotKind::Select || s.tab != active_) continue;
      // LA DERNIÈRE CELLULE DE LA CASE : elle grandit avec ce qu'on tape,
      // et son blanc de queue est exactement la place du caret.
      out = Pos{s.x + s.w - 1, 0};
      return true;
    }
    return false;
  }
  if (mode_ == Mode::EditingPath) {
    // Au bout de ce qu'on tape : une saisie sans curseur a l'air d'une
    // application figée.
    const int x = kGearCells + 1 + text_cells(edit_);
    out = Pos{std::min(x, std::max(0, size_.w - 1)), 0};
    return true;
  }
  const Tab& t = active();
  if (!t.modes.cursor_visible || t.pty.exited()) return false;
  // Remonté dans l'historique, le curseur n'est pas à l'écran : le montrer
  // ailleurs qu'où il est serait pire que ne pas le montrer.
  if (t.history.offset() != 0) return false;
  const CursorPos c = t.screen.cursor();
  out = Pos{c.x, c.y + kBarRows};
  return true;
}

CloseCheck Terminal::can_close() const {
  // N'IMPORTE QUEL onglet vivant retient la fenêtre. Ne regarder que celui
  // qu'on voit tuerait un `make` en cours dans un onglet de fond sans
  // jamais poser la question.
  for (const auto& t : tabs_) {
    if (!t->pty.exited() && t->pty.pid() > 0) {
      return CloseCheck::ask("Un processus tourne encore. Fermer quand meme ?");
    }
  }
  return CloseCheck::allow();
}

void Terminal::draw_bar(View v) const {
  // AUCUN EFFACEMENT ICI. `draw_decor` remplit tout le cadre avant que
  // l'application ne peigne : la barre part donc d'une ligne propre, et lui
  // imposer un fond de plus la détacherait du cadre dont elle fait partie.
  for (const Slot& s : bar_slots()) {
    Style st;
    switch (s.kind) {
      case SlotKind::New:
        st.fg = Color::indexed(2);
        st.attrs = attr::Bold;
        break;
      case SlotKind::Close:
        st.fg = Color::indexed(1);
        break;
      case SlotKind::Settings:
        if (mode_ == Mode::EditingPath) {
          // La barre EST le champ : on la peint en entier, sinon la partie
          // non écrite garderait le fond du cadre et le champ aurait l'air
          // de s'arrêter au dernier caractère tapé.
          st.attrs = attr::Reverse;
          v.fill(Rect{s.x, 0, s.w, kBarRows}, st);
        }
        break;
      case SlotKind::Select:
        if (s.tab == active_) {
          // L'inverse vidéo dit LEQUEL on regarde. Sans elle, la barre dit
          // combien d'onglets existent, et rien de plus.
          st.attrs = attr::Reverse;
          if (mode_ == Mode::Renaming) st.attrs |= attr::Underline;
          v.fill(Rect{s.x, 0, s.w, kBarRows}, st);
        } else {
          st.attrs = attr::Dim;
        }
        break;
    }
    v.text(s.x, 0, s.text, st);
  }
}

void Terminal::render(View v) {
  draw_bar(v);

  const int vw = v.w();
  const int top = kBarRows;
  // La barre est déjà peinte : ce qui reste est la hauteur de la grille.
  // Retrancher `top` ne fait qu'éviter un tour de boucle -- la `View` clippe
  // déjà ce qui dépasse et la grille s'arrête d'elle-même à sa dernière
  // ligne, ce qui rend ÉQUIVALENTE la mutation qui garde `v.h()`.
  const int vh = v.h() - top;
  const Tab& t = active();
  if (!t.spawn_error.empty()) {
    Style st;
    st.fg = Color::indexed(1);
    v.text(0, top, t.spawn_error, st);
    return;
  }

  // Ce qui est visible est la concaténation « historique, puis écran »,
  // lue `offset()` lignes plus haut que sa fin.
  const size_t back = t.history.offset();
  const size_t have = t.history.size();
  for (int y = 0; y < vh; ++y) {
    const size_t from_top = static_cast<size_t>(y);
    if (from_top < back) {
      // Une ligne d'historique. Elle est ROGNÉE : ce qui manque à droite
      // est du vide, pas une erreur.
      const size_t idx = have - back + from_top;
      const ScrollbackLine& line = t.history.at(idx);
      for (size_t x = 0; x < line.size() && static_cast<int>(x) < vw; ++x) {
        if (line[x].width == 0) continue;
        v.put(static_cast<int>(x), top + y, line[x].ch, line[x].style);
      }
      continue;
    }
    const int gy = y - static_cast<int>(back);
    if (gy >= t.screen.rows()) break;
    for (int x = 0; x < vw && x < t.screen.cols(); ++x) {
      const ScreenCell& c = t.screen.at(x, gy);
      if (c.width == 0) continue;  // seconde moitié d'une pleine chasse
      v.put(x, top + y, c.ch, c.style);
    }
  }

  if (t.pty.exited()) {
    // La fenêtre RESTE ouverte : on doit pouvoir lire la dernière erreur.
    Style st;
    st.attrs = attr::Reverse;
    // UN SIGNAL N'EST PAS UN CODE DE SORTIE. « code 11 » pour un SIGSEGV se
    // lit comme un `exit 11` et envoie chercher un défaut là où il n'y en a
    // pas : le shell n'a rien rendu, il a été tué.
    const std::string how =
        t.pty.killed_by_signal()
            ? "tue par le signal " + std::to_string(t.pty.exit_code())
            : "termine (code " + std::to_string(t.pty.exit_code()) + ")";
    const std::string msg =
        "[processus " + how + " - Entree ou clic pour fermer]";
    v.text(0, v.h() - 1, msg, st);
  }
}

// ---------------------------------------------------------------------------
// Le puits du parseur.
// ---------------------------------------------------------------------------

void Terminal::print(char32_t c) { target().screen.print(c); }

void Terminal::execute(uint8_t byte) {
  switch (byte) {
    case '\n':
    case 0x0b:
    case 0x0c:
      target().screen.line_feed();
      break;
    case '\r':
      target().screen.carriage_return();
      break;
    case '\b':
      target().screen.backspace();
      break;
    case '\t':
      target().screen.tab();
      break;
    default:
      // La cloche et le reste : rien à faire d'une grille.
      break;
  }
}

void Terminal::sync_modes() {
  target().screen.set_autowrap(target().modes.autowrap);
  if (target().modes.alt_screen && !target().screen.alt_screen()) {
    target().screen.enter_alt_screen();
  } else if (!target().modes.alt_screen && target().screen.alt_screen()) {
    target().screen.leave_alt_screen();
  }
}

void Terminal::csi(const Params& params, std::string_view intermediates,
                   uint8_t final_byte) {
  // Les questions d'abord : une réponse part sur le maître, jamais vers le
  // client, et elle ne doit pas se faire doubler par un effet de bord.
  const CursorPos cur = target().screen.cursor();
  const std::string answer =
      reply_for_csi(params, intermediates, final_byte, cur.x, cur.y, target().modes);
  if (!answer.empty()) {
    to_guest(answer);
    return;
  }

  if (intermediates == "?") {
    if (final_byte == 'h' || final_byte == 'l') {
      apply_dec_private(params, final_byte == 'h', target().modes);
      sync_modes();
    }
    return;
  }
  if (!intermediates.empty()) return;

  switch (final_byte) {
    case 'A':
      target().screen.move_up(count_of(params));
      break;
    case 'B':
      target().screen.move_down(count_of(params));
      break;
    case 'C':
      target().screen.move_right(count_of(params));
      break;
    case 'D':
      target().screen.move_left(count_of(params));
      break;
    case 'G':
      target().screen.set_column(count_of(params) - 1);
      break;
    case 'd':
      target().screen.set_row(count_of(params) - 1);
      break;
    case 'H':
    case 'f':
      // Le fil compte à partir de 1, la grille à partir de 0. La
      // conversion se fait ICI, en un seul endroit.
      target().screen.move_to(count_of(params, 1) - 1, count_of(params, 0) - 1);
      break;
    case 'J':
      target().screen.erase_display(param_or(params, 0, 0));
      break;
    case 'K':
      target().screen.erase_line(param_or(params, 0, 0));
      break;
    case 'X':
      target().screen.erase_chars(count_of(params));
      break;
    case '@':
      target().screen.insert_chars(count_of(params));
      break;
    case 'P':
      target().screen.delete_chars(count_of(params));
      break;
    case 'L':
      target().screen.insert_lines(count_of(params));
      break;
    case 'M':
      target().screen.delete_lines(count_of(params));
      break;
    case 'r':
      if (params.empty()) {
        target().screen.reset_scroll_region();
      } else {
        target().screen.set_scroll_region(count_of(params, 0) - 1,
                                  param_or(params, 1, target().screen.rows()) - 1);
      }
      break;
    case 'g':
      // TBC. `0` -- le défaut -- retire le taquet sous le curseur, `3` les
      // retire tous. LE RESTE NE FAIT RIEN : la norme ne définit aucune
      // autre valeur, et prendre `CSI 1 g` pour `CSI 3 g` coûterait tous
      // ses taquets à qui se trompe d'un chiffre.
      if (param_or(params, 0, 0) == 0) {
        target().screen.clear_tab();
      } else if (param_or(params, 0, 0) == 3) {
        target().screen.clear_all_tabs();
      }
      break;
    case 'm': {
      Style pen = target().screen.pen();
      apply_sgr(params, pen);
      target().screen.set_pen(pen);
      break;
    }
    default:
      // Tout le reste est ignoré -- mais a bien été CONSOMMÉ par la
      // machine à états, ce qui est le seul point qui compte.
      break;
  }
}

void Terminal::esc(std::string_view intermediates, uint8_t final_byte) {
  if (intermediates == "(") {
    target().screen.set_charset(charset_from_final(final_byte));
    return;
  }
  if (!intermediates.empty()) return;

  switch (final_byte) {
    case '7':
      target().screen.save_cursor();
      break;
    case '8':
      target().screen.restore_cursor();
      break;
    case 'D':
      target().screen.index();
      break;
    case 'M':
      target().screen.reverse_index();
      break;
    case 'E':
      target().screen.next_line();
      break;
    case 'H':
      // HTS : un taquet de tabulation là où est le curseur.
      target().screen.set_tab();
      break;
    default:
      break;
  }
}

void Terminal::osc(std::string_view data) {
  // `OSC 0` et `OSC 2` posent le titre. Le `;` sépare l'identifiant du
  // texte ; sans lui, il n'y a rien à lire.
  const size_t semi = data.find(';');
  if (semi == std::string_view::npos) return;
  const std::string_view id = data.substr(0, semi);
  if (id != "0" && id != "2") return;

  Tab& t = target();
  t.guest_title = std::string(data.substr(semi + 1));
  // SEUL L'ONGLET REGARDÉ NOMME LA FENÊTRE. Un `make` qui pose son titre
  // dans un onglet de fond n'a pas à renommer ce qu'on a sous les yeux ;
  // son nom l'attend dans la barre.
  //
  // La garde est ÉQUIVALENTE en l'état, et le reste par intention :
  // `retitle()` nomme toujours la fenêtre d'après l'onglet actif, donc
  // l'appeler pour un onglet de fond réécrirait le même titre. Elle dit ce
  // que le code veut, et elle porterait pour de bon le jour où `retitle()`
  // prendrait un onglet en argument.
  if (&t == &active()) retitle();
}

}  // namespace sshos
