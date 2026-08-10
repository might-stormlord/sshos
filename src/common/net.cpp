#include "common/net.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>

namespace sshos {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Rend la longueur d'adresse à passer à bind/connect. Pour une adresse
// abstraite elle s'arrête au dernier octet du nom : pas de terminateur.
socklen_t fill(sockaddr_un& addr, std::string_view name) {
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (name.size() + 1 > sizeof addr.sun_path) {
    throw std::runtime_error("nom de socket trop long");
  }
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

Fd make_socket() {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw_errno("socket");
  return Fd(fd);
}

}  // namespace

std::string read_boot_id() {
  std::ifstream in("/proc/sys/kernel/random/boot_id");
  std::string id;
  in >> id;
  if (id.empty()) id = "nobootid";
  return id;
}

std::string socket_name(uid_t uid, std::string_view boot_id) {
  return "sshos/" + std::to_string(uid) + "/" + std::string(boot_id);
}

Fd bind_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    if (errno == EADDRINUSE) throw AddressInUse();
    throw_errno("bind");
  }
  if (::listen(fd.get(), 4) != 0) throw_errno("listen");
  return fd;
}

Fd connect_abstract(std::string_view name) {
  Fd fd = make_socket();
  sockaddr_un addr{};
  const socklen_t len = fill(addr, name);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0) {
    throw_errno("connect");
  }
  return fd;
}

Fd accept_peer(int listen_fd, uid_t expected_uid) {
  const int raw = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (raw < 0) return Fd();
  Fd fd(raw);

  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return Fd();
  }
  if (cred.uid != expected_uid) return Fd();
  return fd;
}

}  // namespace sshos
