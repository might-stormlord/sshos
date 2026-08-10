#include <string>
#include <variant>

#include "common/proto.hpp"
#include "harness.hpp"

using namespace sshos;

static Msg roundtrip(const Msg& m) {
  Decoder d;
  d.feed(encode(m));
  auto out = d.next();
  CHECK(out.has_value());
  return out.value();
}

TEST(proto_roundtrips_hello) {
  Hello h;
  h.build_id = 42;
  h.cols = 200;
  h.rows = 50;
  h.term = "xterm-256color";
  h.colorterm = "truecolor";
  h.utf8 = true;
  h.env.emplace_back("SSH_AUTH_SOCK", "/tmp/agent.1");
  const auto got = std::get<Hello>(roundtrip(h));
  CHECK_EQ(got.build_id, 42u);
  CHECK_EQ(static_cast<int>(got.cols), 200);
  CHECK_EQ(got.term, std::string("xterm-256color"));
  CHECK(got.utf8);
  CHECK_EQ(got.env.size(), static_cast<size_t>(1));
  CHECK_EQ(got.env[0].second, std::string("/tmp/agent.1"));
}

TEST(proto_roundtrips_simple_messages) {
  {
    const auto got = std::get<Incompatible>(roundtrip(Incompatible{"vieux demon"}));
    CHECK_EQ(got.reason, std::string("vieux demon"));
  }
  {
    const auto got = std::get<Input>(roundtrip(Input{"\033[A"}));
    CHECK_EQ(got.bytes, std::string("\033[A"));
  }
  {
    const auto got = std::get<Resize>(roundtrip(Resize{80, 24}));
    CHECK_EQ(static_cast<int>(got.rows), 24);
  }
  {
    const auto got = std::get<FrameMsg>(roundtrip(FrameMsg{"\033[1;1Hx"}));
    CHECK_EQ(got.ansi, std::string("\033[1;1Hx"));
  }
  CHECK(std::holds_alternative<Welcome>(roundtrip(Welcome{})));
}

// Le décodeur est nourri de morceaux arbitraires venant de read().
TEST(proto_decoder_survives_byte_by_byte_feeding) {
  const std::string wire = encode(Msg{Input{"hello"}}) + encode(Msg{Resize{80, 24}});
  Decoder d;
  int produced = 0;
  for (char c : wire) {
    d.feed(std::string_view(&c, 1));
    while (auto m = d.next()) ++produced;
  }
  CHECK_EQ(produced, 2);
}

TEST(proto_decoder_yields_nothing_on_partial_message) {
  const std::string wire = encode(Msg{Input{"hello"}});
  Decoder d;
  d.feed(std::string_view(wire).substr(0, wire.size() - 1));
  CHECK(!d.next().has_value());
  d.feed(std::string_view(wire).substr(wire.size() - 1));
  CHECK(d.next().has_value());
}
