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
void set_cloexec(int fd);

}  // namespace sshos
