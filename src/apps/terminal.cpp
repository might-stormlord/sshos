#include "apps/terminal.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <string>

#include "input/encode.hpp"
#include "pty/env.hpp"
#include "render/surface.hpp"
#include "vt/reply.hpp"

namespace sshos {
namespace {

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

int Terminal::tab_bar_rows() const {
  // UN SEUL onglet n'a pas de barre : elle couterait une ligne de grille
  // pour ne rien dire. Elle apparait des le second.
  return tabs_.size() > 1 || mode_ == Mode::Renaming ? 1 : 0;
}

std::string Terminal::tab_label_for_tests(size_t i) const {
  if (i >= tabs_.size()) return {};
  const Tab& t = *tabs_[i];
  if (!t.custom_title.empty()) return t.custom_title;
  if (!t.guest_title.empty()) return t.guest_title;
  return std::to_string(i + 1);
}

Terminal::~Terminal() {
  // SIGHUP au GROUPE, puis SIGKILL : un shell a des petits-enfants, et ne
  // prévenir que lui laisserait la compilation tourner. `Pty` fait les deux
  // dans son propre destructeur ; ceci ne fait que retirer la surveillance
  // avant que le descripteur ne parte, comme partout ailleurs.
  for (const auto& t : tabs_) {
    if (host_ != nullptr && t->watching) host_->unwatch(t->token);
  }
}

void Terminal::attach(Host& host) {
  host_ = &host;
  open_tab_into(active());
  host.set_title("Terminal");
}

bool Terminal::open_tab() {
  tabs_.push_back(std::make_unique<Tab>(*this));
  Tab& t = *tabs_.back();
  t.screen.set_scrollback(&t.history);
  t.screen.resize(std::max(1, size_.w), std::max(1, size_.h - 1));
  if (host_ != nullptr && !open_tab_into(t)) {
    // Le shell n'a pas demarre : on reste sur celui qui marche plutot que
    // de poser un onglet mort au premier plan.
    tabs_.pop_back();
    return false;
  }
  active_ = tabs_.size() - 1;
  if (host_ != nullptr) host_->invalidate();
  return true;
}

void Terminal::select_tab(size_t i) {
  if (i >= tabs_.size()) return;
  active_ = i;
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::close_tab(size_t i) {
  if (i >= tabs_.size()) return;
  Tab& t = *tabs_[i];
  if (host_ != nullptr && t.watching) host_->unwatch(t.token);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
  if (tabs_.empty()) {
    // LE DERNIER ONGLET FERME LA FENETRE. Une fenetre de terminal sans
    // terminal dedans n'a rien a montrer.
    tabs_.push_back(std::make_unique<Tab>(*this));
    active_ = 0;
    if (host_ != nullptr) host_->request_close();
    return;
  }
  if (active_ >= tabs_.size()) active_ = tabs_.size() - 1;
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
  spec.cols = static_cast<unsigned short>(std::max(1, size_.w));
  spec.rows = static_cast<unsigned short>(std::max(1, size_.h));

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
  Tab& t = active();
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
  active().parser.feed(bytes);
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
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::relayout() {
  // LA BARRE MANGE UNE LIGNE, et donc tous les onglets rétrécissent quand
  // le second s'ouvre -- pas seulement celui qu'on regarde. Un `vim` laissé
  // dans un onglet de fond qui garderait l'ancienne hauteur peindrait sa
  // dernière ligne sous la fenêtre.
  const int w = std::max(1, size_.w);
  const int h = std::max(1, size_.h - tab_bar_rows());
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

void Terminal::on_mouse(const MouseEvent& m) {
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
  to_guest(encode_mouse_sgr(m, m.x, m.y - tab_bar_rows()));
}

bool Terminal::wants_cursor(Pos& out) const {
  const Tab& t = active();
  if (!t.modes.cursor_visible || t.pty.exited()) return false;
  // Remonté dans l'historique, le curseur n'est pas à l'écran : le montrer
  // ailleurs qu'où il est serait pire que ne pas le montrer.
  if (t.history.offset() != 0) return false;
  const CursorPos c = t.screen.cursor();
  out = Pos{c.x, c.y + tab_bar_rows()};
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

void Terminal::render(View v) {
  const int vw = v.w();
  const int top = tab_bar_rows();
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
    const std::string msg =
        "[processus termine (code " + std::to_string(t.pty.exit_code()) +
        ") - Entree ou clic pour fermer]";
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
  if (host_ != nullptr) host_->set_title(std::string(data.substr(semi + 1)));
}

}  // namespace sshos
