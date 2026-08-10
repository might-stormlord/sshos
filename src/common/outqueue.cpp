#include "common/outqueue.hpp"

#include <sys/socket.h>

#include <cerrno>

namespace sshos {

void OutQueue::push(std::string_view bytes) {
  buf_.append(bytes);
  if (size() > ceiling_) {
    buf_.clear();
    off_ = 0;
    overflowed_ = true;
  }
}

bool OutQueue::flush(int fd) {
  while (off_ < buf_.size()) {
    const ssize_t n = ::send(fd, buf_.data() + off_, buf_.size() - off_,
                             MSG_NOSIGNAL);
    if (n > 0) {
      off_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  compact();
  return true;
}

bool OutQueue::take_overflow() {
  const bool v = overflowed_;
  overflowed_ = false;
  return v;
}

void OutQueue::compact() {
  if (off_ == 0) return;
  if (off_ == buf_.size()) {
    buf_.clear();
    off_ = 0;
    return;
  }
  if (off_ > (1 << 16)) {
    buf_.erase(0, off_);
    off_ = 0;
  }
}

}  // namespace sshos
