#include "apps/battement.hpp"

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
