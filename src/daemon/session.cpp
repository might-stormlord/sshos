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

void Session::on_input(const InputEvent& e) {
  if (const auto* k = std::get_if<KeyEvent>(&e)) {
    if (k->key == Key::Char && k->ch == U'q' && (k->mods & mod::Ctrl) != 0) {
      quit_ = true;
      return;
    }
    if (win_) win_->app->on_key(*k);
    return;
  }
  if (const auto* m = std::get_if<MouseEvent>(&e)) {
    if (!win_) return;
    // Routage volontairement minimal : la tâche 5 le remplace par le vrai
    // hit-test. Il suffit à garder verts les trois tests du round EPOLLHUP
    // à chaque commit intermédiaire.
    const Rect cr = client_rect(win_->display_rect);
    if (!cr.contains(m->x, m->y)) return;
    MouseEvent local = *m;
    local.x = m->x - cr.x;
    local.y = m->y - cr.y;
    win_->app->on_mouse(local);
  }
}

void Session::render(Surface& out) {
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

  // Le panneau passe en dernier. clamp_to() garantit déjà qu'aucune fenêtre
  // ne l'atteint ; le dessiner par-dessus coûte une ligne et supprime toute
  // une classe de régressions futures.
  draw_panel(v, out.w(), out.h());
}

}  // namespace sshos
