#include "fake_apps.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace sshos {

void Bloc::attach(Host& host) {
  host_ = &host;
  host_->set_title("Bloc");
}

void Bloc::clamp_cursor() {
  // La zone cliente peut rétrécir sous le curseur. Une position hors zone
  // serait transmise telle quelle au client, qui placerait son curseur
  // matériel sur une cellule qui n'existe plus.
  cursor_.x = std::max(0, std::min(cursor_.x, size_.w - 1));
  cursor_.y = std::max(0, std::min(cursor_.y, size_.h - 1));
}

void Bloc::on_resize(Size s) {
  ++resizes_;
  size_ = s;
  clamp_cursor();
}

void Bloc::on_key(const KeyEvent& k) {
  switch (k.key) {
    case Key::Left:
      --cursor_.x;
      break;
    case Key::Right:
      ++cursor_.x;
      break;
    case Key::Up:
      --cursor_.y;
      break;
    case Key::Down:
      ++cursor_.y;
      break;
    case Key::Home:
      cursor_.x = 0;
      break;
    case Key::End:
      cursor_.x = size_.w - 1;
      break;
    default:
      // Toute autre touche est une « saisie » : c'est elle qui rend le
      // document modifié, pas un simple déplacement du curseur. Sans cette
      // distinction, ouvrir une fenêtre et appuyer sur une flèche
      // suffirait à déclencher un dialogue de confirmation à la fermeture.
      if (!modified_) {
        modified_ = true;
        if (host_ != nullptr) host_->set_title("Bloc *");
      }
      break;
  }
  clamp_cursor();
}

void Bloc::on_mouse(const MouseEvent& m) {
  if (m.action != MouseAction::Press) return;
  ++clicks_;
  cursor_.x = m.x;
  cursor_.y = m.y;
  clamp_cursor();
}

bool Bloc::wants_cursor(Pos& out) const {
  out = cursor_;
  return true;
}

CloseCheck Bloc::can_close() const {
  if (!modified_) return CloseCheck::allow();
  return CloseCheck::ask("Bloc a des modifications non enregistrees.");
}

void Bloc::render(View v) {
  Style st;
  st.fg = Color::indexed(7);
  st.bg = Color::indexed(0);
  v.fill(Rect{0, 0, v.w(), v.h()}, st);

  // Les trois lignes sont retenues plutôt qu'écrites directement : le
  // curseur ci-dessous a besoin de savoir quel caractère se trouve sous
  // lui. Elles sont purement ASCII, donc l'index d'octet vaut la colonne.
  const std::string lines[3] = {
      "taille: " + std::to_string(size_.w) + "x" + std::to_string(size_.h),
      "clics: " + std::to_string(clicks_),
      "resize: " + std::to_string(resizes_),
  };
  for (int i = 0; i < 3; ++i) v.text(0, i, lines[i], st);

  // Le curseur INVERSE la cellule sous lui au lieu de la remplacer, comme
  // le fait un vrai curseur de terminal. Un curseur qui remplacerait
  // effacerait le caractère qu'il recouvre, et les relevés « taille: »,
  // « clics: » et « resize: » -- sur lesquels reposent les tests de
  // session -- deviendraient illisibles dès que le curseur passe dessus.
  Style cur = st;
  cur.attrs |= attr::Reverse;
  char32_t under = U' ';
  if (cursor_.y >= 0 && cursor_.y < 3) {
    const std::string& l = lines[cursor_.y];
    if (cursor_.x >= 0 && static_cast<std::size_t>(cursor_.x) < l.size())
      under = static_cast<char32_t>(static_cast<unsigned char>(l[cursor_.x]));
  }
  v.put(cursor_.x, cursor_.y, under, cur);
}

}  // namespace sshos


#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "render/cell.hpp"

namespace sshos {

Battement::~Battement() {
  // L'ordre compte, et c'est celui-ci : retirer la surveillance AVANT de
  // fermer le descripteur. L'inverse laisserait une entrée epoll sur un
  // numéro que le noyau peut réattribuer à la milliseconde suivante, et le
  // prochain occupant recevrait nos réveils.
  //
  // Ce que cet ordre exige de la fenêtre : que l'hôte survive à
  // l'application. C'est exactement ce que garantit la déclaration de
  // `host` avant `app` dans Window -- les membres meurent à l'envers de
  // leur déclaration.
  close_pipe();
  if (write_fd_ >= 0) ::close(write_fd_);
}

void Battement::close_pipe() {
  if (watching_ && host_ != nullptr) {
    host_->unwatch(token_);
    watching_ = false;
  }
  if (read_fd_ >= 0) {
    ::close(read_fd_);
    read_fd_ = -1;
  }
}

void Battement::attach(Host& host) {
  host_ = &host;
  host.set_title("Battement");

  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) return;
  read_fd_ = fds[0];
  write_fd_ = fds[1];
  token_ = host.watch(read_fd_, EPOLLIN);
  watching_ = true;
}

void Battement::beat() {
  if (write_fd_ < 0) return;
  const char b = 1;
  const ssize_t put = ::write(write_fd_, &b, 1);
  (void)put;  // tuyau plein : le battement est perdu, sans conséquence
}

void Battement::on_command(std::string_view cmd) {
  if (cmd == "beat") beat();
  if (cmd == "cut") cut_source();
}

void Battement::cut_source() {
  if (write_fd_ < 0) return;
  ::close(write_fd_);
  write_fd_ = -1;
}

IoStatus Battement::on_io(uint64_t token, uint32_t events) {
  if (!watching_ || token != token_) return IoStatus::Ok;

  // Drainer D'ABORD, tester la fermeture ENSUITE. C'est le correctif
  // EPOLLHUP du jalon 1 transposé au niveau applicatif : le noyau coalesce
  // couramment EPOLLIN et EPOLLHUP dans un seul réveil quand la source
  // écrit puis ferme aussitôt, et honorer HUP en premier jetterait des
  // octets déjà arrivés.
  bool ended = false;
  char buf[256];
  for (;;) {
    const ssize_t got = ::read(read_fd_, buf, sizeof buf);
    if (got > 0) {
      beats_ += static_cast<int>(got);
      continue;
    }
    if (got == 0) {
      ended = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    ended = true;
    break;
  }

  // Ce que l'application vient de lire change ce qu'elle affiche : c'est à
  // elle de le dire, pas au démon de le deviner. Sans cet appel, le compteur
  // n'apparaîtrait à l'écran qu'à la frappe suivante.
  host_->invalidate();

  if (ended || (events & (EPOLLHUP | EPOLLERR)) != 0) {
    close_pipe();
    return IoStatus::Closed;
  }
  return IoStatus::Ok;
}

void Battement::render(View v) {
  Style st;
  st.fg = Color::indexed(7);
  st.bg = Color::indexed(0);
  v.fill(Rect{0, 0, v.w(), v.h()}, st);
  v.text(0, 0, "battements: " + std::to_string(beats_), st);
  v.text(0, 1, source_alive() ? "source: vivante" : "source: fermee", st);
}

}  // namespace sshos
