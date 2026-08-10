#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "common/fd.hpp"
#include "harness.hpp"

using sshos::Fd;

static bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(fd_closes_on_destruction) {
  int raw = ::open("/dev/null", O_RDONLY);
  CHECK(raw >= 0);
  {
    Fd f(raw);
    CHECK(f.valid());
    CHECK_EQ(f.get(), raw);
  }
  CHECK(!fd_is_open(raw));
}

TEST(fd_move_transfers_ownership) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.get();
  Fd b(std::move(a));
  CHECK(!a.valid());
  CHECK_EQ(b.get(), raw);
  CHECK(fd_is_open(raw));
}

TEST(fd_release_gives_up_ownership) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.release();
  CHECK(!a.valid());
  CHECK(fd_is_open(raw));
  ::close(raw);
}

TEST(fd_cloexec_and_nonblock_are_set) {
  Fd a(::open("/dev/null", O_RDONLY));
  sshos::set_cloexec(a.get());
  sshos::set_nonblock(a.get());
  CHECK((::fcntl(a.get(), F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((::fcntl(a.get(), F_GETFL) & O_NONBLOCK) != 0);
}
