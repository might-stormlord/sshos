#include "daemon/session.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <variant>

#include "apps/bloc.hpp"
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

// Position et taille de la première fenêtre, déterministes : plusieurs
// tests bout-en-bout cliquent à une coordonnée fixe en comptant sur le fait
// qu'elle tombe dans la zone cliente. La cascade et le placement libre
// arrivent à la tâche 6.
constexpr Rect kFirstWindowRect{2, 1, 44, 14};

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

Session::Session(Platform& plat, int, int) : plat_(&plat) {
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

Rect Session::work_area(int cols, int rows) const {
  // Panneau ancré en bas, épaisseur 1. Les quatre bords arrivent à la
  // tâche 9.
  return Rect{0, 0, cols, rows - 1};
}

void Session::ensure_window(const Rect& work) {
  if (win_) return;
  auto w = std::make_unique<Window>();
  w->id = next_id_++;
  w->user_rect = kFirstWindowRect;
  w->app = std::make_unique<Bloc>();
  w->display_rect = clamp_to(w->user_rect, work, frame_min(*w->app));
  // L'hôte est créé APRÈS la fenêtre et AVANT attach() : c'est attach() qui
  // fait poser son titre à l'application.
  w->host = std::make_unique<HostImpl>(*w);
  w->app->attach(*w->host);
  win_ = std::move(w);
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
    if (win_ && win_->id == mv->win) win_->user_rect = mv->origin;
  }
  drag_ = Idle{};
}

void Session::watchdog() {
  if (std::holds_alternative<Idle>(drag_)) return;
  if (plat_->steady_now() - drag_stamp_ >= kDragWatchdog) cancel_drag();
}

WinHitResult Session::hit_window_at(int x, int y) const {
  if (!win_) return WinHitResult{};
  return hit_window(*win_, x, y);
}

void Session::on_mouse(const MouseEvent& m) {
  if (!win_) return;
  Window& w = *win_;

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

  if (auto* mv = std::get_if<Moving>(&drag_)) {
    drag_stamp_ = plat_->steady_now();
    Rect r = w.user_rect;
    r.x = m.x - mv->grab_dx;
    r.y = m.y - mv->grab_dy;
    w.user_rect = r;
    if (m.action == MouseAction::Release) finish_drag();
    return;
  }

  if (auto* rz = std::get_if<Resizing>(&drag_)) {
    drag_stamp_ = plat_->steady_now();
    rz->outline.w = m.x - rz->outline.x + 1;
    rz->outline.h = m.y - rz->outline.y + 1;
    if (m.action == MouseAction::Release) {
      // C'est ICI, et seulement ici, que la fenêtre change de taille : d'où
      // un unique on_resize() par geste, au lieu d'un par mouvement.
      w.user_rect = rz->outline;
      finish_drag();
    }
    return;
  }

  if (m.action != MouseAction::Press) return;

  const WinHitResult h = hit_window(w, m.x, m.y);
  drag_stamp_ = plat_->steady_now();
  switch (h.what) {
    case WinHit::TitleBar:
      drag_ = Moving{w.id, m.x - w.display_rect.x, m.y - w.display_rect.y,
                     w.user_rect};
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
    if (win_) win_->app->on_key(*k);
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

  const Rect work = work_area(out.w(), out.h());
  ensure_window(work);

  Window& w = *win_;
  w.display_rect = clamp_to(w.user_rect, work, frame_min(*w.app));
  const Rect cr = client_rect(w.display_rect);
  const Size cs{cr.w, cr.h};
  if (!(cs == w.sent_size)) {
    w.app->on_resize(cs);
    w.sent_size = cs;
  }

  draw_decor(v, w, true, theme_, border());
  w.app->render(v.sub(cr));

  // Le contour élastique du redimensionnement, par-dessus la fenêtre : rien
  // d'autre ne bouge pendant le geste, c'est lui seul qui donne à voir la
  // taille visée.
  if (const auto* rz = std::get_if<Resizing>(&drag_)) {
    Style ol;
    ol.fg = theme_.accent;
    ol.bg = theme_.desktop_bg;
    const Rect o = clamp_to(rz->outline, work, frame_min(*w.app));
    v.box(o, border(), ol);
  }

  // Le panneau passe en dernier. clamp_to() garantit déjà qu'aucune fenêtre
  // ne l'atteint ; le dessiner par-dessus coûte une ligne et supprime toute
  // une classe de régressions futures.
  draw_panel(v, out.w(), out.h());
}

}  // namespace sshos
