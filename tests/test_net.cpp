#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "common/net.hpp"
#include "harness.hpp"

using sshos::Fd;

static std::string unique_name() {
  return "sshos-test/" + std::to_string(::getpid());
}

TEST(net_socket_name_is_stable) {
  CHECK_EQ(sshos::socket_name(1000, "abc"), std::string("sshos/1000/abc"));
}

TEST(net_bind_acts_as_a_mutex) {
  const std::string name = unique_name();
  Fd first = sshos::bind_abstract(name);
  CHECK(first.valid());

  bool threw = false;
  try {
    Fd second = sshos::bind_abstract(name);
  } catch (const sshos::AddressInUse&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_connect_reaches_the_listener_and_peer_uid_matches) {
  const std::string name = unique_name() + "-conn";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  CHECK(client.valid());

  Fd served = sshos::accept_peer(listener.get(), ::getuid());
  CHECK(served.valid());
}

TEST(net_accept_rejects_a_foreign_uid) {
  const std::string name = unique_name() + "-uid";
  Fd listener = sshos::bind_abstract(name);
  Fd client = sshos::connect_abstract(name);
  // Un uid qui n'est certainement pas le nôtre.
  Fd served = sshos::accept_peer(listener.get(), ::getuid() + 4242);
  CHECK(!served.valid());
}

TEST(net_connect_fails_when_nobody_listens) {
  bool threw = false;
  try {
    Fd f = sshos::connect_abstract(unique_name() + "-absent");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(net_boot_id_is_not_empty) {
  CHECK(!sshos::read_boot_id().empty());
}
