#include "common/fd.hpp"

#include <fcntl.h>

#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

void set_nonblock(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags == -1) throw_errno("fcntl F_GETFL");
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) throw_errno("fcntl F_SETFL");
}

void set_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags == -1) throw_errno("fcntl F_GETFD");
  if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) throw_errno("fcntl F_SETFD");
}

}  // namespace sshos
