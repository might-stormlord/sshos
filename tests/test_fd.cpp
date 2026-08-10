#include <fcntl.h>
#include <unistd.h>

#include <utility>
#include <system_error>

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

TEST(fd_move_assignment_transfers_ownership) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.get();
  Fd b(::open("/dev/null", O_RDONLY));
  b = std::move(a);
  CHECK(!a.valid());
  CHECK_EQ(b.get(), raw);
  CHECK(fd_is_open(raw));
}

TEST(fd_self_assignment_is_safe) {
  Fd a(::open("/dev/null", O_RDONLY));
  const int raw = a.get();
  // auto-assignment ne devrait pas fermer le descripteur
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
  a = std::move(a);
#pragma GCC diagnostic pop
  CHECK(a.valid());
  CHECK_EQ(a.get(), raw);
  CHECK(fd_is_open(raw));
}

TEST(set_nonblock_and_set_cloexec_throw_on_invalid_fd) {
  // set_nonblock(-1) devrait lever std::system_error
  bool set_nonblock_threw = false;
  try {
    sshos::set_nonblock(-1);
  } catch (const std::system_error& e) {
    set_nonblock_threw = true;
    CHECK_EQ(e.code().value(), EBADF);
  }
  CHECK(set_nonblock_threw);

  // set_cloexec(-1) devrait lever std::system_error
  bool set_cloexec_threw = false;
  try {
    sshos::set_cloexec(-1);
  } catch (const std::system_error& e) {
    set_cloexec_threw = true;
    CHECK_EQ(e.code().value(), EBADF);
  }
  CHECK(set_cloexec_threw);
}
