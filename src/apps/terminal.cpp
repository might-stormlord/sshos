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

Terminal::Terminal() { screen_.set_scrollback(&history_); }

Terminal::Terminal(std::vector<std::string> argv) : argv_(std::move(argv)) {
  screen_.set_scrollback(&history_);
}

Terminal::~Terminal() {
  // SIGHUP au GROUPE, puis SIGKILL : un shell a des petits-enfants, et ne
  // prévenir que lui laisserait la compilation tourner. `Pty` fait les deux
  // dans son propre destructeur ; ceci ne fait que retirer la surveillance
  // avant que le descripteur ne parte, comme partout ailleurs.
  if (host_ != nullptr && watching_) host_->unwatch(token_);
}

void Terminal::attach(Host& host) {
  host_ = &host;

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

  spawn_error_ = pty_.spawn(spec);
  if (!spawn_error_.empty()) return;

  token_ = host.watch(pty_.master(), EPOLLIN);
  watching_ = true;
  // La récolte est globale au démon : l'application ne fait que dire à qui
  // appartient ce pid.
  host.watch_child(pty_.pid());
  host.set_title("Terminal");
}

void Terminal::to_guest(std::string_view bytes) {
  if (bytes.empty()) return;
  if (pty_.master() < 0) {
    // Pas de PTY : en test, ou après la fermeture du maître. On retient au
    // lieu de perdre -- c'est ce qui rend l'encodage vérifiable sans
    // lancer de shell.
    pending_.append(bytes);
    return;
  }
  pty_.write(bytes.data(), bytes.size());
}

std::string Terminal::take_written_for_tests() {
  std::string out;
  out.swap(pending_);
  return out;
}

void Terminal::feed_for_tests(std::string_view bytes) { parser_.feed(bytes); }

IoStatus Terminal::on_io(uint64_t token, uint32_t events) {
  if (token != token_) return IoStatus::Ok;
  (void)events;

  // On DRAINE avant de conclure quoi que ce soit. Les noyaux récents
  // livrent d'abord ce qui restait en tampon, puis rendent EIO : fermer sur
  // le premier réveil jetterait le dernier mot de l'invité.
  for (;;) {
    char buf[8192];
    const ssize_t n = pty_.read(buf, sizeof buf);
    if (n > 0) {
      parser_.feed(std::string_view(buf, static_cast<size_t>(n)));
      continue;
    }
    if (n == 0) {
      // Fin de fichier : l'esclave n'est plus ouvert nulle part.
      pty_.note_eof();
      if (host_ != nullptr) host_->invalidate();
      return IoStatus::Closed;
    }
    break;  // rien de plus à lire pour l'instant
  }
  if (host_ != nullptr) host_->invalidate();
  return IoStatus::Ok;
}

void Terminal::on_child_exit(int status) {
  (void)status;
  pty_.try_reap();
  if (host_ != nullptr) host_->invalidate();
}

void Terminal::on_resize(Size s) {
  if (s.w <= 0 || s.h <= 0) return;
  size_ = s;
  screen_.resize(s.w, s.h);
  // La taille faisant autorité est celle du PTY : c'est le noyau qui
  // envoie `SIGWINCH` au groupe au premier plan, pas nous.
  pty_.resize(static_cast<unsigned short>(s.w), static_cast<unsigned short>(s.h));
}

void Terminal::on_key(const KeyEvent& k) {
  if (pty_.exited()) {
    // Un terminal mort ne prend plus de frappes : `Entrée` le ferme, tout
    // le reste est ignoré. La fenêtre RESTE ouverte jusque-là, pour qu'on
    // puisse lire la dernière erreur.
    if (k.key == Key::Enter && host_ != nullptr) host_->request_close();
    return;
  }

  // `Maj+PgPréc` / `PgSuiv` consultent l'historique. Ils ne vont jamais à
  // l'invité : c'est notre historique, pas le sien.
  if ((k.mods & mod::Shift) != 0 && !screen_.alt_screen()) {
    if (k.key == Key::PgUp) {
      history_.scroll_back(static_cast<size_t>(std::max(1, size_.h / 2)));
      if (host_ != nullptr) host_->invalidate();
      return;
    }
    if (k.key == Key::PgDn) {
      history_.scroll_forward(static_cast<size_t>(std::max(1, size_.h / 2)));
      if (host_ != nullptr) host_->invalidate();
      return;
    }
  }

  const std::string bytes = encode_key(k, modes_.cursor_keys_application);
  if (bytes.empty()) return;
  // Écrire, c'est revenir au présent : personne ne tape en aveugle dans
  // une page d'historique.
  history_.scroll_to_bottom();
  to_guest(bytes);
}

void Terminal::on_mouse(const MouseEvent& m) {
  if (pty_.exited()) {
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
  if (wheel && !screen_.alt_screen() && modes_.tracking() == MouseTracking::None) {
    const size_t step = 3;
    if (m.action == MouseAction::WheelUp) {
      history_.scroll_back(step);
    } else {
      history_.scroll_forward(step);
    }
    if (host_ != nullptr) host_->invalidate();
    return;
  }

  if (modes_.tracking() == MouseTracking::None) return;
  if (modes_.tracking() == MouseTracking::Click &&
      m.action == MouseAction::Motion) {
    // 1000 ne rapporte PAS le mouvement. Le rapporter quand même noierait
    // le lien SSH d'un paquet par cellule parcourue.
    return;
  }
  to_guest(encode_mouse_sgr(m, m.x, m.y));
}

bool Terminal::wants_cursor(Pos& out) const {
  if (!modes_.cursor_visible || pty_.exited()) return false;
  // Remonté dans l'historique, le curseur n'est pas à l'écran : le montrer
  // ailleurs qu'où il est serait pire que ne pas le montrer.
  if (history_.offset() != 0) return false;
  const CursorPos c = screen_.cursor();
  out = Pos{c.x, c.y};
  return true;
}

CloseCheck Terminal::can_close() const {
  if (!pty_.exited() && pty_.pid() > 0) {
    return CloseCheck::ask("Un processus tourne encore. Fermer quand meme ?");
  }
  return CloseCheck::allow();
}

void Terminal::render(View v) {
  const int vw = v.w();
  const int vh = v.h();
  if (!spawn_error_.empty()) {
    Style st;
    st.fg = Color::indexed(1);
    v.text(0, 0, spawn_error_, st);
    return;
  }

  // Ce qui est visible est la concaténation « historique, puis écran »,
  // lue `offset()` lignes plus haut que sa fin.
  const size_t back = history_.offset();
  const size_t have = history_.size();
  for (int y = 0; y < vh; ++y) {
    const size_t from_top = static_cast<size_t>(y);
    if (from_top < back) {
      // Une ligne d'historique. Elle est ROGNÉE : ce qui manque à droite
      // est du vide, pas une erreur.
      const size_t idx = have - back + from_top;
      const ScrollbackLine& line = history_.at(idx);
      for (size_t x = 0; x < line.size() && static_cast<int>(x) < vw; ++x) {
        if (line[x].width == 0) continue;
        v.put(static_cast<int>(x), y, line[x].ch, line[x].style);
      }
      continue;
    }
    const int gy = y - static_cast<int>(back);
    if (gy >= screen_.rows()) break;
    for (int x = 0; x < vw && x < screen_.cols(); ++x) {
      const ScreenCell& c = screen_.at(x, gy);
      if (c.width == 0) continue;  // seconde moitié d'une pleine chasse
      v.put(x, y, c.ch, c.style);
    }
  }

  if (pty_.exited()) {
    // La fenêtre RESTE ouverte : on doit pouvoir lire la dernière erreur.
    Style st;
    st.attrs = attr::Reverse;
    const std::string msg =
        "[processus termine (code " + std::to_string(pty_.exit_code()) +
        ") - Entree ou clic pour fermer]";
    v.text(0, vh - 1, msg, st);
  }
}

// ---------------------------------------------------------------------------
// Le puits du parseur.
// ---------------------------------------------------------------------------

void Terminal::print(char32_t c) { screen_.print(c); }

void Terminal::execute(uint8_t byte) {
  switch (byte) {
    case '\n':
    case 0x0b:
    case 0x0c:
      screen_.line_feed();
      break;
    case '\r':
      screen_.carriage_return();
      break;
    case '\b':
      screen_.backspace();
      break;
    case '\t':
      screen_.tab();
      break;
    default:
      // La cloche et le reste : rien à faire d'une grille.
      break;
  }
}

void Terminal::sync_modes() {
  screen_.set_autowrap(modes_.autowrap);
  if (modes_.alt_screen && !screen_.alt_screen()) {
    screen_.enter_alt_screen();
  } else if (!modes_.alt_screen && screen_.alt_screen()) {
    screen_.leave_alt_screen();
  }
}

void Terminal::csi(const Params& params, std::string_view intermediates,
                   uint8_t final_byte) {
  // Les questions d'abord : une réponse part sur le maître, jamais vers le
  // client, et elle ne doit pas se faire doubler par un effet de bord.
  const CursorPos cur = screen_.cursor();
  const std::string answer =
      reply_for_csi(params, intermediates, final_byte, cur.x, cur.y, modes_);
  if (!answer.empty()) {
    to_guest(answer);
    return;
  }

  if (intermediates == "?") {
    if (final_byte == 'h' || final_byte == 'l') {
      apply_dec_private(params, final_byte == 'h', modes_);
      sync_modes();
    }
    return;
  }
  if (!intermediates.empty()) return;

  switch (final_byte) {
    case 'A':
      screen_.move_up(count_of(params));
      break;
    case 'B':
      screen_.move_down(count_of(params));
      break;
    case 'C':
      screen_.move_right(count_of(params));
      break;
    case 'D':
      screen_.move_left(count_of(params));
      break;
    case 'G':
      screen_.set_column(count_of(params) - 1);
      break;
    case 'd':
      screen_.set_row(count_of(params) - 1);
      break;
    case 'H':
    case 'f':
      // Le fil compte à partir de 1, la grille à partir de 0. La
      // conversion se fait ICI, en un seul endroit.
      screen_.move_to(count_of(params, 1) - 1, count_of(params, 0) - 1);
      break;
    case 'J':
      screen_.erase_display(param_or(params, 0, 0));
      break;
    case 'K':
      screen_.erase_line(param_or(params, 0, 0));
      break;
    case 'X':
      screen_.erase_chars(count_of(params));
      break;
    case '@':
      screen_.insert_chars(count_of(params));
      break;
    case 'P':
      screen_.delete_chars(count_of(params));
      break;
    case 'L':
      screen_.insert_lines(count_of(params));
      break;
    case 'M':
      screen_.delete_lines(count_of(params));
      break;
    case 'r':
      if (params.empty()) {
        screen_.reset_scroll_region();
      } else {
        screen_.set_scroll_region(count_of(params, 0) - 1,
                                  param_or(params, 1, screen_.rows()) - 1);
      }
      break;
    case 'm': {
      Style pen = screen_.pen();
      apply_sgr(params, pen);
      screen_.set_pen(pen);
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
    screen_.set_charset(charset_from_final(final_byte));
    return;
  }
  if (!intermediates.empty()) return;

  switch (final_byte) {
    case '7':
      screen_.save_cursor();
      break;
    case '8':
      screen_.restore_cursor();
      break;
    case 'D':
      screen_.index();
      break;
    case 'M':
      screen_.reverse_index();
      break;
    case 'E':
      screen_.next_line();
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
