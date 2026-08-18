#include "common/oom.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace sshos {
namespace {

constexpr char kPath[] = "/proc/self/oom_score_adj";

// Ecrit `n` octets, sans rien allouer et en reprenant sur EINTR : cette
// fonction tourne aussi entre fork() et execve(), ou tout le reste est
// interdit.
bool write_setting(const char* text, size_t n) {
  const int fd = ::open(kPath, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  size_t done = 0;
  bool ok = true;
  while (done < n) {
    const ssize_t w = ::write(fd, text + done, n - done);
    if (w > 0) {
      done += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    ok = false;
    break;
  }
  ::close(fd);
  return ok;
}

}  // namespace

bool protect_from_oom() {
  static_assert(kOomProtected == -1000, "le litteral ci-dessous suit kOomProtected");
  constexpr char valeur[] = "-1000";
  return write_setting(valeur, sizeof valeur - 1);
}

void drop_oom_protection() {
  constexpr char valeur[] = "0";
  write_setting(valeur, sizeof valeur - 1);
}

}  // namespace sshos
