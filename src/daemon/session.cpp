#include "daemon/session.hpp"

#include "wm/tile.hpp"

#include <memory>
#include <string>
#include <variant>

#include "app/catalog.hpp"
#include "daemon/host.hpp"
#include "render/width.hpp"
#include "wm/decor.hpp"

namespace sshos {
namespace {

constexpr int kMinCols = 40;
constexpr int kMinRows = 12;

// A1 : deux formulations du même avertissement. Le message complet ne
// s'affiche que lorsque le terminal est trop petit — et se retrouvait donc
// tronqué précisément quand il servait le plus, sur les largeurs les plus
// étroites (View::text clippe à la largeur de la surface, sans notion de
// mot entier). En dessous de la longueur du message complet, la forme
// courte prend le relais ; elle est garantie de tenir jusqu'à 12 colonnes,
// la plus petite surface qu'un test de cette suite construit.
constexpr char kFullWarning[] = "terminal trop petit - 40x12 minimum";
constexpr char kShortWarning[] = "trop petit";
constexpr int kFullWarningLen = sizeof(kFullWarning) - 1;

// Ce que le bureau ouvre tout seul quand la pile est vide. La première
// fenêtre tombe donc sur la première marche de la cascade, {2, 1, 44, 14} :
// plusieurs tests bout-en-bout cliquent à une coordonnée fixe en comptant
// sur le fait qu'elle atterrit dans sa zone cliente.
constexpr char kDefaultApp[] = "bloc";

// Le relâchement peut se perdre : terminal qui filtre, multiplexeur qui
// avale un octet, client tué en plein geste. Deux secondes sans nouvelle
// et le glissement est abandonné. Il n'y a pas de minuterie dédiée où
// s'accrocher -- le démon n'en a qu'une, l'horloge de frame à 33 ms
// (daemon.cpp, kFrameIntervalMs), et elle ne bat que lorsque quelque chose
// est sale -- donc le garde-fou est temporel, relu à chaque entrée et à
// chaque composition.
constexpr std::chrono::milliseconds kDragWatchdog{2000};

// Un accord resté en l'air aussi longtemps n'est pas un accord : c'est une
// hésitation. Assez court pour répondre à qui cherche, assez long pour que
// « Ctrl+A puis w » tapé d'un trait ne fasse jamais clignoter l'aide.
constexpr std::chrono::milliseconds kHelpDelay{500};

// Ce qui sépare deux gestes d'une même série. Généreux exprès : on pousse
// une fenêtre, on regarde, on pousse encore. Le danger habituel d'une
// fenêtre longue -- une frappe ordinaire prise pour une commande -- n'existe
// pas ici, puisque la série ne retient QUE les gestes qui s'enchaînent et
// rend tout le reste à l'application (input/shortcuts.cpp).
constexpr std::chrono::milliseconds kRepeatWindow{1500};

// Ce qui sépare deux clics d'un double-clic. Généreux : à travers SSH, deux
// appuis partis à 200 ms d'intervalle peuvent arriver bien plus espacés.
constexpr std::chrono::milliseconds kDoubleClick{600};


}  // namespace

Session::Session(Platform& plat, FdRegistrar& fds, int, int)
    : plat_(&plat), fds_(&fds) {
  theme_ = Theme::defaults().for_profile(out_);
}

std::string Session::take_out_of_band() {
  std::string out;
  out.swap(out_of_band_);
  return out;
}

bool Session::take_repaint() {
  const bool r = repaint_;
  repaint_ = false;
  return r;
}

bool Session::take_detach() {
  const bool d = detach_;
  detach_ = false;
  return d;
}

int Session::help_delay_ms() const {
  if (!leader_.armed() || help_.is_open()) return -1;
  const auto due = leader_stamp_ + kHelpDelay;
  const auto now = plat_->steady_now();
  if (now >= due) return 0;
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count());
}

// La touche leader est un caractère qu'on tape avec Ctrl ; « Ctrl+A » et
// « ^A » en sont les deux écritures d'usage. Une minuscule se montre en
// capitale, comme sur le capuchon de la touche.
std::string Session::leader_label() const {
  const char32_t c = leader_.leader();
  const char up = (c >= U'a' && c <= U'z')
                      ? static_cast<char>(c - U'a' + U'A')
                      : static_cast<char>(c);
  return std::string("Ctrl+") + up;
}

std::string Session::panel_hint() const {
  const char32_t c = leader_.leader();
  const char up = (c >= U'a' && c <= U'z')
                      ? static_cast<char>(c - U'a' + U'A')
                      : static_cast<char>(c);
  return std::string("^") + up + " = aide";
}

// Ouvre l'application demandée, ou rappelle celle qui l'est déjà : cliquer
// une épinglée deux fois de suite ne doit pas empiler deux instances.
void Session::focus_or_open(const std::string& app_id) {
  for (const auto& up : wm_.stack()) {
    if (up->app_id != app_id) continue;
    if (up->mode == WinMode::Minimized) {
      wm_.set_mode(up->id, WinMode::Normal, last_work_);
    }
    wm_.focus(up->id);
    dirty_ = true;
    return;
  }
  open_from_catalog(app_id);
  dirty_ = true;
}

void Session::run_menu(const std::string& id) {
  menu_.close();
  dirty_ = true;

  const auto starts = [&id](const char* p) {
    const std::string pre = p;
    return id.size() > pre.size() && id.compare(0, pre.size(), pre) == 0;
  };

  if (starts("app:")) {
    focus_or_open(id.substr(4));
    return;
  }
  if (starts("panel:")) {
    const std::string where = id.substr(6);
    if (where == "top") panel_.set_edge(PanelEdge::Top);
    if (where == "bottom") panel_.set_edge(PanelEdge::Bottom);
    if (where == "left") panel_.set_edge(PanelEdge::Left);
    if (where == "right") panel_.set_edge(PanelEdge::Right);
    // Le panneau change d'épaisseur avec son bord : tout ce que le client
    // croyait savoir de l'écran est faux.
    repaint_ = true;
    return;
  }
  if (starts("cmd:")) {
    // La session ne sait pas ce que la commande veut dire, et c'est le
    // but : elle la tend à l'application focalisée, qui la comprend ou
    // l'ignore.
    if (Window* w = wm_.find(wm_.focused())) w->app->on_command(id.substr(4));
    return;
  }
  if (id == "wm:tile") {
    tile_windows();
    return;
  }
  if (id == "session:detach") {
    // Exactement ce que fait Ctrl+Q : le client s'en va, la session
    // continue de vivre dans le démon avec toutes ses fenêtres.
    detach_ = true;
    return;
  }
  if (id == "session:quit") {
    // ON DEMANDE. C'est le seul geste du bureau qui détruise le travail de
    // toutes les fenêtres à la fois, et il était jusqu'ici à un clic de
    // distance -- au milieu du menu, juste sous « Quitter », qui lui ne
    // détruit rien.
    modal_quits_session_ = true;
    modal_.ask("Fermer la session ? Toutes les fenetres seront perdues.", 0);
    dirty_ = true;
  }
}

void Session::menu_key(const KeyEvent& k) {
  switch (k.key) {
    case Key::Escape:
      menu_.close();
      break;
    case Key::Enter:
      if (const MenuItem* it = menu_.selected()) {
        run_menu(it->id);
        return;
      }
      menu_.close();
      break;
    case Key::Up:
      menu_.move(-1);
      break;
    case Key::Down:
      menu_.move(1);
      break;
    case Key::Backspace:
      menu_.backspace();
      break;
    case Key::Char:
      if ((k.mods & mod::Ctrl) != 0) {
        menu_.close();
      } else {
        menu_.type(k.ch);
      }
      break;
    default:
      break;
  }
  dirty_ = true;
}

void Session::do_action(Action a) {
  dirty_ = true;

  switch (a) {
    case Action::OpenMenu:
      menu_.open();
      return;
    case Action::ToggleMouse:
      mouse_on_ = !mouse_on_;
      // Pas de message de protocole pour ça : les séquences DEC voyagent
      // dans le flux de trames, que le client recopie verbatim.
      out_of_band_ += mouse_on_ ? "\033[?1002h\033[?1006h"
                                : "\033[?1002l\033[?1006l";
      return;
    case Action::ForceRepaint:
      repaint_ = true;
      return;
    case Action::Detach:
      detach_ = true;
      return;
    case Action::ShowHelp:
      help_.open();
      return;
    case Action::NextWindow:
      wm_.focus_next();
      return;
    case Action::PrevWindow:
      wm_.focus_prev();
      return;
    default:
      break;
  }

  Window* w = wm_.find(wm_.focused());
  if (w == nullptr) return;

  switch (a) {
    case Action::LiteralLeader:
      w->app->on_key(KeyEvent{Key::Char, leader_.leader(), mod::Ctrl});
      return;
    case Action::Close:
      request_close(*w);
      return;
    case Action::Minimize:
      wm_.set_mode(w->id, WinMode::Minimized, last_work_);
      return;
    case Action::MaximizeToggle:
      wm_.set_mode(w->id,
                   w->mode == WinMode::Maximized ? WinMode::Normal
                                                 : WinMode::Maximized,
                   last_work_);
      return;
    case Action::FullscreenToggle:
      wm_.set_mode(w->id,
                   w->mode == WinMode::Fullscreen ? WinMode::Normal
                                                  : WinMode::Fullscreen,
                   last_work_);
      // Le panneau s'escamote ou revient : le client ne peut pas le deviner
      // d'un delta.
      repaint_ = true;
      return;
    default:
      break;
  }

  // Les huit derniers travaillent sur la géométrie voulue -- jamais sur la
  // projection -- et sont bornés à la zone de travail, sans quoi une
  // fenêtre poussée trop loin partirait hors de l'écran et demanderait
  // autant de frappes pour revenir.
  //
  // Pas d'aimantation ici, contrairement au relâchement d'un glissement :
  // la tolérance vaut une cellule et le pas clavier aussi, donc snap()
  // annulerait chaque pas fait DEPUIS un bord -- la fenêtre y resterait
  // collée pour toujours (défaut trouvé à la sonde, pas par un test).
  // Aimanter sert à rattraper un geste approximatif ; une frappe est déjà
  // exacte, et buter contre le bord par clamp_to donne le même résultat.
  const Size fmin = frame_min(*w->app);
  Rect r = w->user_rect;
  switch (a) {
    case Action::MoveLeft: --r.x; break;
    case Action::MoveRight: ++r.x; break;
    case Action::MoveUp: --r.y; break;
    case Action::MoveDown: ++r.y; break;
    case Action::GrowWidth: ++r.w; break;
    case Action::ShrinkWidth: r.w = std::max(fmin.w, r.w - 1); break;
    case Action::GrowHeight: ++r.h; break;
    case Action::ShrinkHeight: r.h = std::max(fmin.h, r.h - 1); break;
    default: return;
  }
  w->user_rect = clamp_to(r, last_work_, fmin);
}

bool Session::take_dirty() {
  // L'horloge est relue ICI plutôt que dans render() seule : render() n'est
  // appelée que lorsque la frame est déjà sale, elle ne peut donc pas être
  // ce qui découvre qu'une minute a tourné.
  if (clock_.update(*plat_)) dirty_ = true;
  // L'accord est resté en l'air assez longtemps : la main hésite, on lui
  // montre la table.
  if (help_delay_ms() == 0) {
    help_.open();
    dirty_ = true;
  }
  const bool d = dirty_;
  dirty_ = false;
  return d;
}

void Session::set_output(const OutputProfile& p) {
  out_ = p;
  theme_ = Theme::defaults().for_profile(p);
}

void Session::resize(int, int) {}

Border Session::border() const {
  return out_.utf8 ? Border::Unicode : Border::Ascii;
}

WindowId Session::open_from_catalog(std::string_view id) {
  const CatalogEntry* e = catalog_find(id);
  if (e == nullptr) return 0;
  Window* w = wm_.open(e->make(), last_work_);
  if (w == nullptr) return 0;
  w->app_id = e->id;
  // L'hôte est créé APRÈS que le gestionnaire a donné à la fenêtre son
  // adresse définitive, et AVANT attach() : c'est attach() qui fait poser
  // son titre à l'application.
  w->host = std::make_unique<HostImpl>(*w, *fds_, fd_gen_, dirty_, children_);
  w->app->attach(*w->host);
  return w->id;
}

void Session::request_close(Window& w) {
  // Annuler le glissement AVANT tout le reste : sans cet ordre, la machine
  // à états garderait l'identifiant d'une fenêtre détruite et le prochain
  // mouvement de souris irait chercher un fantôme.
  //
  // Aucun chemin actuel n'y arrive avec un geste en cours -- au clavier, la
  // règle « toute frappe annule le glissement » (on_input) est passée
  // avant ; à la souris, un clic sur [×] suppose un bouton relâché, donc
  // aucun geste engagé. Ça reste vrai tant que la seule voie restante,
  // Host::request_close() consommée en tête de composition, ne peut pas
  // survenir pendant un geste : la composition n'a jamais lieu au milieu du
  // traitement d'un évènement de souris.
  cancel_drag();
  const CloseCheck c = w.app->can_close();
  if (c.allowed) {
    close_window(w);
    return;
  }
  modal_.ask(c.question, w.id);
  dirty_ = true;
}

void Session::answer_modal(bool confirmed) {
  if (confirmed) {
    if (modal_quits_session_) {
      quit_ = true;
    } else if (Window* t = wm_.find(modal_.target())) {
      close_window(*t);
    }
  }
  modal_quits_session_ = false;
  modal_.dismiss();
  dirty_ = true;
}

// Le rangement ne touche QUE les fenêtres visibles : une fenêtre réduite
// n'occupe aucune place, et lui en donner une la ferait réapparaître sans
// qu'on l'ait demandé.
void Session::tile_windows() {
  std::vector<WindowId> visible;
  for (const auto& w : wm_.stack()) {
    if (w != nullptr && w->mode != WinMode::Minimized) visible.push_back(w->id);
  }
  if (visible.empty()) return;

  const std::vector<Rect> cells =
      tile_rects(last_work_, static_cast<int>(visible.size()));
  for (size_t i = 0; i < visible.size() && i < cells.size(); ++i) {
    // Une fenêtre maximisée ou plein écran redevient normale : la ranger
    // sans cela lui donnerait une géométrie que son mode ignore.
    wm_.set_mode(visible[i], WinMode::Normal, last_work_);
    wm_.set_rect(visible[i], cells[i], last_work_);
  }
  dirty_ = true;
}

void Session::close_window(Window& w) {
  // L'ordre est le même que partout ailleurs : les entrées epoll partent
  // avant les descripteurs. Le destructeur de l'application le fait déjà
  // pour ses propres surveillances (Window déclare `host` avant `app`,
  // donc l'hôte lui survit) ; ceci couvre celles qu'elle aurait oubliées.
  if (w.host != nullptr) static_cast<HostImpl*>(w.host.get())->unwatch_all();
  // Les enfants de cette fenêtre ne sont plus attendus par personne. Les
  // laisser dans la table ferait livrer leur mort à une fenêtre disparue,
  // ou pire, à celle qui reprendrait son numéro.
  children_.erase(std::remove_if(children_.begin(), children_.end(),
                                 [id = w.id](const ChildWatch& c) {
                                   return c.win == id;
                                 }),
                  children_.end());
  wm_.close(w.id);
}

int Session::refresh_delay_ms() const {
  int best = -1;
  for (const auto& w : wm_.stack()) {
    if (w == nullptr || w->app == nullptr) continue;
    // La fenetre MINIMISEE ne compte pas : c'est toute la regle.
    if (w->mode == WinMode::Minimized) continue;
    const int want = w->app->refresh_ms();
    if (want < 0) continue;
    if (best < 0 || want < best) best = want;
  }
  return best;
}

Window* Session::window_for_tests(WindowId id) { return wm_.find(id); }

void Session::close_window_for_tests(WindowId id) {
  Window* w = wm_.find(id);
  if (w != nullptr) close_window(*w);
}

void Session::on_child_exit(pid_t pid, int status) {
  const auto it = std::find_if(children_.begin(), children_.end(),
                               [pid](const ChildWatch& c) { return c.pid == pid; });
  if (it == children_.end()) return;
  const WindowId win = it->win;
  // L'entrée part AVANT de prévenir l'application : celle-ci a le droit de
  // relancer un enfant depuis son `on_child_exit()`, et l'itérateur ne
  // survivrait pas au `push_back` que ça provoquerait.
  children_.erase(it);

  Window* w = wm_.find(win);
  if (w == nullptr || w->app == nullptr) return;
  w->app->on_child_exit(status);
  // Ce que l'application vient d'apprendre change ce qu'elle affiche.
  dirty_ = true;
}

void Session::on_fd_event(uint64_t key, uint32_t events) {
  Window* w = wm_.find(key_window(key));
  if (w == nullptr || w->host == nullptr) return;  // fenêtre déjà fermée
  auto* host = static_cast<HostImpl*>(w->host.get());
  if (host->deliver(key, events) == IoStatus::Closed) {
    // L'application a perdu sa source. Elle reste vivante : c'est à elle de
    // décider si elle veut continuer, et Battement le fait.
  }
}

void Session::ensure_window(const Rect& work) {
  (void)work;  // déjà relevée dans last_work_ par render()
  // UNE SEULE FOIS. S'attacher sur un écran vide sans savoir quoi faire est
  // le pire premier contact possible, donc on amorce ; mais rouvrir à
  // chaque trame rend la dernière fenêtre INFERMABLE, ce qui est pire
  // encore -- le [×] semble ne rien faire. La suite appartient à
  // l'utilisateur, bureau vide compris.
  if (seeded_) return;
  seeded_ = true;
  open_from_catalog(kDefaultApp);
}


void Session::cancel_drag() {
  // Défaire ce que le geste a déjà écrit. Un déplacement suit le curseur en
  // direct -- user_rect a donc bougé dès le premier mouvement -- alors qu'un
  // redimensionnement ne touche à rien avant le relâchement : seul le
  // premier a quelque chose à restaurer.
  if (const auto* mv = std::get_if<Moving>(&drag_)) {
    if (Window* w = wm_.find(mv->win)) w->user_rect = mv->origin;
  }
  drag_ = Idle{};
}

void Session::watchdog() {
  if (std::holds_alternative<Idle>(drag_)) return;
  if (plat_->steady_now() - drag_stamp_ >= kDragWatchdog) cancel_drag();
}

WinHitResult Session::hit_window_at(int x, int y) const {
  // De l'avant vers l'arrière : c'est la fenêtre du dessus qui répond.
  const auto& st = wm_.stack();
  for (auto it = st.rbegin(); it != st.rend(); ++it) {
    const Window& w = **it;
    if (w.mode == WinMode::Minimized) continue;
    if (w.display_rect.contains(x, y)) return hit_window(w, x, y);
  }
  return WinHitResult{};
}

void Session::on_mouse(const MouseEvent& m) {
  // Un mouvement sans bouton pendant un glissement veut dire que le
  // relâchement s'est perdu (parser.cpp : `cb & 3`, donc 3 = aucun bouton).
  if (m.action == MouseAction::Motion && m.button == 3) {
    cancel_drag();
    return;
  }

  // Un second appui pendant un glissement l'annule. On ne sait pas ce que
  // l'utilisateur veut, et continuer à traîner la fenêtre est le pire des
  // paris possibles.
  if (m.action == MouseAction::Press && !std::holds_alternative<Idle>(drag_)) {
    cancel_drag();
    return;
  }

  // Une fenêtre peut disparaître au milieu d'un geste -- une application qui
  // se ferme toute seule sur un événement de descripteur, par exemple. Le
  // glissement n'a alors plus de sujet : on l'annule.
  if (auto* mv = std::get_if<Moving>(&drag_)) {
    Window* w = wm_.find(mv->win);
    if (w == nullptr) {
      cancel_drag();
      return;
    }
    drag_stamp_ = plat_->steady_now();
    Rect r = w->user_rect;
    r.x = m.x - mv->grab_dx;
    r.y = m.y - mv->grab_dy;
    w->user_rect = r;
    if (m.action == MouseAction::Release) {
      // L'aimantation ne s'applique qu'ICI, au déplacement. snap() conserve
      // w et h et ne bouge que x et y : c'est exactement ce qu'un
      // déplacement fait, et exactement ce qu'un redimensionnement ne doit
      // pas faire -- tirer le coin bas-droit ne doit jamais décaler le coin
      // haut-gauche, ce que ferait un x aimanté.
      //
      // Et seulement si le geste a effectivement déplacé la fenêtre. Un
      // simple clic sur la barre de titre -- prendre le focus, rien de plus
      // -- passe par ici avec un déplacement net nul, et l'aimanter
      // décalerait la fenêtre d'une cellule sans que personne l'ait
      // demandé : la première marche de la cascade tombe précisément à une
      // cellule du bord haut de la zone.
      if (!(w->user_rect == mv->origin)) {
        w->user_rect = snap(w->user_rect, last_work_, 1);
      }
      finish_drag();
    }
    return;
  }

  if (auto* rz = std::get_if<Resizing>(&drag_)) {
    Window* w = wm_.find(rz->win);
    if (w == nullptr) {
      cancel_drag();
      return;
    }
    drag_stamp_ = plat_->steady_now();
    rz->outline.w = m.x - rz->outline.x + 1;
    rz->outline.h = m.y - rz->outline.y + 1;
    if (m.action == MouseAction::Release) {
      // C'est ICI, et seulement ici, que la fenêtre change de taille : d'où
      // un unique on_resize() par geste, au lieu d'un par mouvement.
      w->user_rect = rz->outline;
      finish_drag();
    }
    return;
  }

  // INVARIANT : au-delà de cette ligne, tout est un appui. Les glissements
  // ont consommé leurs relâchements plus haut, et rien d'autre ne réagit
  // au relâchement ni au survol. Les branches qui suivent ne re-testent
  // donc PAS `m.action` -- le faire suggérerait que l'invariante ne tient
  // pas, et masquerait sa disparition si quelqu'un la supprimait.
  if (m.action != MouseAction::Press) return;

  // Et la modale passe devant le menu : rien derrière un dialogue modal
  // n'est cliquable, pas même pour le refermer d'un clic à côté.
  if (modal_.is_open()) {
    const ModalHit mh = modal_.hit(m.x, m.y);
    if (mh == ModalHit::Confirm) answer_modal(true);
    if (mh == ModalHit::Cancel) answer_modal(false);
    dirty_ = true;
    return;
  }

  // L'aide se retire au premier clic, où qu'il tombe. Elle ne prend pas le
  // clic pour autant : ce n'est pas un dialogue, rien n'y est à répondre.
  if (help_.is_open()) {
    help_.close();
    dirty_ = true;
    return;
  }

  // Le menu passe devant tout : ouvert, il prend le clic ou se referme.
  if (menu_.is_open()) {
    const MenuHitResult mh = menu_.hit(m.x, m.y);
    if (mh.what == MenuHit::Item) {
      const auto& items = menu_.visible();
      if (mh.index >= 0 && mh.index < static_cast<int>(items.size())) {
        run_menu(items[static_cast<size_t>(mh.index)].id);
      }
      return;
    }
    if (mh.what == MenuHit::None) menu_.close();
    dirty_ = true;
    return;
  }

  // Puis le panneau, qui recouvre par construction tout ce qui pourrait se
  // trouver dessous.
  const PanelHitResult ph = panel_.hit(m.x, m.y);
  if (ph.what != PanelHit::None) {
    dirty_ = true;
    switch (ph.what) {
      case PanelHit::MenuButton:
        menu_.open();
        break;
      case PanelHit::Hint:
        // Le rappel dit quelle touche ouvre l'aide ; le cliquer l'ouvre
        // aussi. Quelqu'un qui ne connaît pas encore le clavier du bureau a
        // toutes les raisons d'essayer la souris d'abord.
        help_.open();
        break;
      case PanelHit::Pinned: {
        const auto& cat = catalog();
        if (ph.index >= 0 && ph.index < static_cast<int>(cat.size())) {
          focus_or_open(cat[static_cast<size_t>(ph.index)].id);
        }
        break;
      }
      case PanelHit::Task: {
        Window* t = wm_.find(ph.win);
        if (t == nullptr) break;
        // Cliquer l'entrée de la fenêtre active la réduit ; cliquer celle
        // d'une autre la rappelle. C'est la convention de toutes les barres
        // des tâches, et elle rend le clic idempotent par paire.
        if (t->mode == WinMode::Minimized) {
          wm_.set_mode(t->id, WinMode::Normal, last_work_);
          wm_.focus(t->id);
        } else if (wm_.focused() == t->id) {
          wm_.set_mode(t->id, WinMode::Minimized, last_work_);
        } else {
          wm_.focus(t->id);
        }
        break;
      }
      default:
        break;
    }
    return;
  }

  Window* wp = wm_.hit(m.x, m.y);
  if (wp == nullptr) {
    // Clic DROIT sur le vide : le menu s'ouvre là où on a cliqué. C'est le
    // geste qu'essaie en premier qui vient d'un vrai bureau, et c'est la
    // sortie d'un écran vide sans toucher au clavier. Le clic gauche, lui,
    // ne fait rien : ouvrir un menu dessus serait insupportable.
    if (m.button == 2) {
      menu_.open_at(m.x, m.y);
      dirty_ = true;
    }
    return;
  }
  Window& w = *wp;

  // Un clic n'importe où sur une fenêtre non focalisée lui donne le focus
  // AVANT tout le reste : appuyer sur [×] d'une fenêtre d'arrière-plan doit
  // fermer celle-là, pas celle qui avait la main.
  if (wm_.focused() != w.id) wm_.focus(w.id);

  const WinHitResult h = hit_window(w, m.x, m.y);
  drag_stamp_ = plat_->steady_now();

  // Un double-clic ne se voit que d'ici : le hit-testing est sans mémoire.
  // On le relève AVANT le switch pour que la position comparée soit celle
  // de l'appui, et pas celle qu'un déplacement aurait déjà décalée.
  const auto now = plat_->steady_now();
  const bool doubled = m.x == last_click_x_ && m.y == last_click_y_ &&
                       now - last_click_ <= kDoubleClick;
  last_click_ = now;
  last_click_x_ = m.x;
  last_click_y_ = m.y;
  // Un double-clic reconnu remet le compteur à zéro : trois clics font un
  // double puis un simple, pas deux doubles qui se chevauchent.
  if (doubled) last_click_x_ = -1;

  switch (h.what) {
    case WinHit::TitleBar:
      // Double-clic sur la barre de titre : maximise, puis rétablit. Le
      // geste de tous les bureaux, et il n'existait qu'au clavier et au
      // bouton [□].
      if (doubled) {
        wm_.set_mode(w.id,
                     w.mode == WinMode::Maximized ? WinMode::Normal
                                                  : WinMode::Maximized,
                     last_work_);
        break;
      }
      // Une fenêtre maximisée ou en plein écran ne se traîne pas : elle
      // n'est pas là où user_rect le dit. La déplacer réécrirait EN SILENCE
      // la géométrie de retour -- un simple clic pour prendre le focus
      // suffisait à décaler la fenêtre au rétablissement suivant.
      if (w.mode != WinMode::Normal) break;
      drag_ = Moving{w.id, m.x - w.display_rect.x, m.y - w.display_rect.y,
                     w.user_rect};
      break;
    case WinHit::ButtonMinimize:
      wm_.set_mode(w.id, WinMode::Minimized, last_work_);
      break;
    case WinHit::ButtonMaximize:
      wm_.set_mode(w.id,
                   w.mode == WinMode::Maximized ? WinMode::Normal
                                                : WinMode::Maximized,
                   last_work_);
      break;
    case WinHit::ButtonClose:
      request_close(w);
      break;
    case WinHit::EdgeRight:
    case WinHit::EdgeBottom:
    case WinHit::CornerBR:
      drag_ = Resizing{w.id, w.display_rect};
      break;
    case WinHit::Client: {
      MouseEvent local = m;
      local.x = h.lx;
      local.y = h.ly;
      w.app->on_mouse(local);
      break;
    }
    default:
      break;
  }
}

void Session::on_input(const InputEvent& e) {
  watchdog();

  if (const auto* k = std::get_if<KeyEvent>(&e)) {
    // Toute frappe annule un glissement en cours -- Échap comme les autres.
    // Continuer à déplacer une fenêtre pendant que l'utilisateur tape serait
    // au mieux surprenant.
    if (!std::holds_alternative<Idle>(drag_)) {
      cancel_drag();
      return;
    }
    // Ctrl+Q DÉTACHE, il ne détruit rien. C'est le geste que la main fait
    // pour « quitter », et le laisser tuer la session serait le contraire de
    // ce que ce projet promet : on revient, tout est là. Détruire la session
    // pour de bon se demande explicitement, par l'entrée « Quitter la
    // session » du menu.
    if (k->key == Key::Char && k->ch == U'q' && (k->mods & mod::Ctrl) != 0) {
      detach_ = true;
      return;
    }
    // La modale passe avant tout le reste : c'est ce que « modal » veut
    // dire. Ni le menu, ni les raccourcis, ni l'application ne voient rien
    // tant qu'on n'a pas répondu.
    if (modal_.is_open()) {
      switch (k->key) {
        case Key::Escape:
          modal_.dismiss();
          break;
        case Key::Tab:
        case Key::BackTab:
        case Key::Left:
        case Key::Right:
          modal_.focus_next();
          break;
        case Key::Enter:
          answer_modal(modal_.confirm_focused());
          break;
        default:
          break;
      }
      dirty_ = true;
      return;
    }

    // Le menu capture tout tant qu'il est ouvert : une application ne doit
    // jamais recevoir les frappes qui pilotent le bureau.
    if (menu_.is_open()) {
      menu_key(*k);
      return;
    }
    // La série a-t-elle expiré ? Relue ici, à la frappe, plutôt que sur une
    // minuterie : tant que personne ne tape, une série finie ne change rien
    // à ce qui est à l'écran.
    if (leader_.repeating() && plat_->steady_now() > repeat_until_) {
      leader_.reset();
    }

    // L'aide se retire à la PREMIÈRE touche, et cette touche garde son
    // effet : elle s'est ouverte parce que l'accord traînait, pas pour
    // installer un mode dont il faudrait ressortir.
    if (help_.is_open()) {
      help_.close();
      dirty_ = true;
    }

    const LeaderResult lr = leader_.feed(*k);
    if (lr.action.has_value()) {
      do_action(*lr.action);
      // Un geste qui s'enchaîne rouvre sa fenêtre à chaque fois : c'est
      // l'ÉCART entre deux gestes qui est borné, pas la durée totale de la
      // série. Pousser une fenêtre à l'autre bout de l'écran ne demande donc
      // qu'un seul accord.
      if (leader_.repeating()) {
        repeat_until_ = plat_->steady_now() + kRepeatWindow;
      }
      return;
    }
    if (lr.consumed) {
      // Le leader vient d'armer : c'est d'ici que part le compte à rebours.
      if (leader_.armed()) leader_stamp_ = plat_->steady_now();
      return;
    }
    if (Window* w = wm_.find(wm_.focused())) w->app->on_key(*k);
    return;
  }
  if (const auto* f = std::get_if<FocusEvent>(&e)) {
    if (!f->focused) cancel_drag();
    return;
  }
  if (const auto* m = std::get_if<MouseEvent>(&e)) on_mouse(*m);
}

void Session::draw_empty_hint(View v, const Rect& work) const {
  // Deux lignes centrées : ce qu'on voit, puis les deux gestes qui en
  // sortent. Le clic droit d'abord, parce qu'il marche PARTOUT sur le vide
  // et n'oblige pas à viser un bouton de trois cellules.
  const bool utf8 = out_.utf8;
  const std::string burger = utf8 ? "\xe2\x98\xb0" : "=";
  const std::string lines[2] = {
      "Bureau vide",
      "Clic droit ici, ou " + burger + " en bas, pour ouvrir le menu",
  };

  Style st;
  st.fg = theme_.panel_fg;
  st.bg = theme_.desktop_bg;
  const int top = work.y + (work.h - 2) / 2;
  for (int i = 0; i < 2; ++i) {
    // Le rognage est DÉFENSIF et non observable aujourd'hui : le panneau
    // est peint après l'invite et View::text clippe déjà au bord de la
    // surface, si bien qu'aucune des quatre positions de panneau ne
    // distingue une invite rognée d'une invite qui déborde. Il redeviendra
    // porteur le jour où l'invite passera dans une sous-vue, ou passera
    // après le panneau. Mutation déclarée équivalente, pas couverte.
    const std::string txt = elide_to_cells(lines[i], work.w, "");
    const int x = work.x + (work.w - text_cells(txt)) / 2;
    v.text(x < work.x ? work.x : x, top + i, txt, st);
  }
}

void Session::render(Surface& out) {
  watchdog();

  View v = out.root();
  Style desk;
  desk.bg = theme_.desktop_bg;
  v.fill(Rect{0, 0, out.w(), out.h()}, desk);

  if (out.w() < kMinCols || out.h() < kMinRows) {
    // On sort AVANT de toucher à quoi que ce soit : l'état du bureau est
    // préservé intact, et réagrandir le terminal le rend tel quel.
    Style warn;
    warn.fg = theme_.panel_fg;
    if (out.w() >= kFullWarningLen) {
      v.text(0, 0, kFullWarning, warn);
    } else {
      v.text(0, 0, kShortWarning, warn);
    }
    return;
  }

  if (clock_.update(*plat_)) dirty_ = true;

  // Une application qui a demandé sa propre fermeture est servie ici, en
  // tête de composition : elle passe par le MÊME chemin qu'un clic sur
  // [×], donc par can_close() et, s'il le faut, par le dialogue. Aucune
  // application du catalogue n'appelle encore Host::request_close(), mais
  // sans cette consommation le drapeau serait en écriture seule.
  for (const auto& up : wm_.stack()) {
    if (!up->close_requested) continue;
    up->close_requested = false;
    request_close(*up);
    break;  // la pile vient de changer : le reste au prochain tour
  }

  const Rect work =
      work_area(out.w(), out.h(), panel_.edge(), panel_.thickness());
  last_work_ = work;
  ensure_window(work);

  // Toute la géométrie se décide ICI, en un seul endroit et sans toucher à
  // un seul user_rect : c'est ce qui rend le redimensionnement du terminal
  // réversible.
  relayout(wm_, work, out.w(), out.h());

  // Un bureau vide dit quoi faire, et le dit pour la SOURIS : c'est avec
  // elle qu'on arrive, et le menu est à un clic. Sans cette ligne, fermer
  // la dernière fenêtre laisse un écran nu où plus rien n'a l'air
  // cliquable -- exactement l'impasse que l'amorce perpétuelle masquait.
  if (wm_.stack().empty()) draw_empty_hint(v, work);

  // De l'arrière vers l'avant : la dernière peinte est celle du dessus.
  const WindowId focused = wm_.focused();
  for (const auto& up : wm_.stack()) {
    Window& w = *up;
    if (w.mode == WinMode::Minimized) continue;

    const Rect cr = client_rect(w.display_rect);
    const Size cs{cr.w, cr.h};
    if (!(cs == w.sent_size)) {
      w.app->on_resize(cs);
      w.sent_size = cs;
    }

    draw_decor(v, w, w.id == focused, theme_, border());
    w.app->render(v.sub(cr));
  }

  // Le contour élastique du redimensionnement, par-dessus toute la pile :
  // rien d'autre ne bouge pendant le geste, c'est lui seul qui donne à voir
  // la taille visée.
  if (const auto* rz = std::get_if<Resizing>(&drag_)) {
    if (const Window* w = wm_.find(rz->win)) {
      Style ol;
      ol.fg = theme_.accent;
      ol.bg = theme_.desktop_bg;
      const Rect o = clamp_to(rz->outline, work, frame_min(*w->app));
      v.box(o, border(), ol);
    }
  }

  // Le panneau passe en dernier. clamp_to() garantit déjà qu'aucune fenêtre
  // ne l'atteint ; le dessiner par-dessus coûte une ligne et supprime toute
  // une classe de régressions futures.
  //
  // Sauf sous une fenêtre plein écran, qui l'escamote : c'est là toute la
  // différence entre « plein écran » et « maximisé », lequel s'arrête à la
  // zone de travail précisément pour laisser le panneau visible.
  //
  // La disposition du panneau est recalculee dans tous les cas : c'est elle
  // que le hit-test consulte, et un panneau caché reste un panneau dont on
  // doit savoir où il serait.
  panel_.set_hint(panel_hint());
  panel_.layout(wm_, out.w(), out.h(), out_.utf8);
  const Window* front = wm_.find(focused);
  if (front == nullptr || front->mode != WinMode::Fullscreen) {
    panel_.draw(v, theme_, clock_.text(), clock_.date());
  }

  // Le menu passe en dernier : il recouvre le panneau qui l'a ouvert.
  menu_.layout(out.w(), out.h());
  menu_.draw(v, theme_, border());

  // Puis l'aide par-dessus le menu : elle peut s'ouvrir pendant qu'il est
  // là, et c'est elle qu'on vient de demander.
  help_.layout(out.w(), out.h());
  help_.draw(v, theme_, border(), leader_label(), out_.utf8);

  // Et la modale par-dessus tout : elle est la dernière chose posée
  // parce qu'elle est la seule à laquelle on puisse répondre.
  modal_.layout(out.w(), out.h());
  modal_.draw(v, theme_, border());
}

}  // namespace sshos
