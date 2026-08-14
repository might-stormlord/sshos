#pragma once

#include <unistd.h>

#include <utility>

namespace sshos {

// Propriétaire unique d'un descripteur. Non copiable : deux propriétaires
// signifieraient une double fermeture, et une double fermeture après
// réattribution du numéro ferme le descripteur de quelqu'un d'autre.
class Fd {
 public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}

  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = -1;
    }
    return *this;
  }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  ~Fd() { reset(); }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  int release() noexcept { return std::exchange(fd_, -1); }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

void set_nonblock(int fd);
// AUCUN APPELANT EN PRODUCTION, et c'est voulu : chaque descripteur du
// projet naît déjà CLOEXEC, en un seul appel système -- `O_CLOEXEC`,
// `SOCK_CLOEXEC`, `SFD_CLOEXEC`, `EPOLL_CLOEXEC`, `TFD_CLOEXEC`, et
// `accept4()` pour les connexions entrantes. C'est le motif SÛR : le poser
// après coup laisse une fenêtre pendant laquelle un `fork()` concurrent
// hériterait du descripteur. Les deux seuls sites sans CLOEXEC le sont
// exprès -- l'esclave du pseudo-terminal et le `/dev/null` du démon sont
// justement faits pour être hérités.
//
// La fonction reste, testée, pour le jour où un site ne pourra pas faire
// autrement. Ce commentaire existe pour qu'un balayage des méthodes sans
// appelant ne la reprenne pas pour un branchement oublié.
void set_cloexec(int fd);

}  // namespace sshos
