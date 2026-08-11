#include "daemon/session.hpp"

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <variant>

#include "app/catalog.hpp"
#include "daemon/host.hpp"
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

std::string clock_text(const Platform& plat) {
  const std::time_t t = std::chrono::system_clock::to_time_t(plat.now());
  // Heure LOCALE, pas UTC : un panneau qui affiche l'heure doit suivre le
  // fuseau réel de la machine, heure d'été comprise -- jamais un décalage
  // fixe codé en dur (Toronto est à UTC-5 en hiver/EST, UTC-4 en été/EDT ;
  // seule la base de fuseaux tzdata, interrogée par ::localtime_r, connaît
  // la bonne bascule). Un offset constant serait juste faux six mois par
  // an.
  //
  // ::tzset() est nécessaire ici, pas cosmétique : POSIX ne garantit pas que
  // ::localtime_r() l'appelle elle-même, et la glibc ne relit TZ qu'à la
  // première utilisation (vérifié empiriquement -- voir rapport de tâche) :
  // un changement ultérieur de TZ resterait sinon silencieusement ignoré.
  // Coût nominal négligeable : glibc ne re-parse le fichier de zone que si
  // TZ a effectivement changé depuis le dernier appel.
  //
  // Le démon est détaché (double fork + setsid, voir daemonize.cpp) : il ne
  // conserve aucun terminal contrôleur, mais hérite bien de l'environnement
  // du processus qui l'a lancé, TZ compris -- c'est cette valeur, figée au
  // moment du lancement, que ::tzset() lit ici.
  ::tzset();
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

}  // namespace

Session::Session(Platform& plat, FdRegistrar& fds, int, int)
    : plat_(&plat), fds_(&fds) {
  theme_ = Theme::defaults().for_profile(out_);
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
  // L'hôte est créé APRÈS que le gestionnaire a donné à la fenêtre son
  // adresse définitive, et AVANT attach() : c'est attach() qui fait poser
  // son titre à l'application.
  w->host = std::make_unique<HostImpl>(*w, *fds_, fd_gen_);
  w->app->attach(*w->host);
  return w->id;
}

void Session::close_window(Window& w) {
  // L'ordre est le même que partout ailleurs : les entrées epoll partent
  // avant les descripteurs. Le destructeur de l'application le fait déjà
  // pour ses propres surveillances (Window déclare `host` avant `app`,
  // donc l'hôte lui survit) ; ceci couvre celles qu'elle aurait oubliées.
  if (w.host != nullptr) static_cast<HostImpl*>(w.host.get())->unwatch_all();
  wm_.close(w.id);
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
  if (!wm_.stack().empty()) return;
  open_from_catalog(kDefaultApp);
}

void Session::draw_panel(View& v, int cols, int rows) {
  Style p;
  p.bg = theme_.panel_bg;
  p.fg = theme_.panel_fg;
  const int py = rows - 1;
  v.fill(Rect{0, py, cols, 1}, p);

  // Le bouton de menu porte la marque du projet. En UTF-8 il gagne son
  // glyphe ; sans UTF-8 le mot seul reste lisible, là où un point
  // d'interrogation ne dirait rien. Il devient cliquable à la tâche 10.
  v.text(1, py, out_.utf8 ? "☰ ssh_os" : "ssh_os", p);

  const std::string t = clock_text(*plat_);
  v.text(cols - static_cast<int>(t.size()) - 1, py, t, p);
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

  if (m.action != MouseAction::Press) return;

  Window* wp = wm_.hit(m.x, m.y);
  if (wp == nullptr) return;
  Window& w = *wp;

  // Un clic n'importe où sur une fenêtre non focalisée lui donne le focus
  // AVANT tout le reste : appuyer sur [×] d'une fenêtre d'arrière-plan doit
  // fermer celle-là, pas celle qui avait la main.
  if (wm_.focused() != w.id) wm_.focus(w.id);

  const WinHitResult h = hit_window(w, m.x, m.y);
  drag_stamp_ = plat_->steady_now();
  switch (h.what) {
    case WinHit::TitleBar:
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
      // Le dialogue modal arrive à la tâche 10. D'ici là, une application qui
      // refuse de partir reste ouverte -- sans rien dire, mais sans mentir
      // non plus.
      if (w.app->can_close().allowed) close_window(w);
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
    if (k->key == Key::Char && k->ch == U'q' && (k->mods & mod::Ctrl) != 0) {
      quit_ = true;
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

  const Rect work = work_area(out.w(), out.h(), edge_, thickness_);
  last_work_ = work;
  ensure_window(work);

  // Toute la géométrie se décide ICI, en un seul endroit et sans toucher à
  // un seul user_rect : c'est ce qui rend le redimensionnement du terminal
  // réversible.
  relayout(wm_, work, out.w(), out.h());

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
  const Window* front = wm_.find(focused);
  if (front == nullptr || front->mode != WinMode::Fullscreen) {
    draw_panel(v, out.w(), out.h());
  }
}

}  // namespace sshos
