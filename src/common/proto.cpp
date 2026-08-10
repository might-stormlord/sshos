#include "common/proto.hpp"

#include <cstring>

namespace sshos {
namespace {

enum class Tag : uint8_t {
  Hello = 1, Welcome = 2, Incompatible = 3, Detached = 4,
  Input = 5, Resize = 6, Frame = 7,
};

void put_u8(std::string& o, uint8_t v) { o += static_cast<char>(v); }

void put_u16(std::string& o, uint16_t v) {
  put_u8(o, static_cast<uint8_t>(v >> 8));
  put_u8(o, static_cast<uint8_t>(v & 0xFF));
}

void put_u32(std::string& o, uint32_t v) {
  put_u16(o, static_cast<uint16_t>(v >> 16));
  put_u16(o, static_cast<uint16_t>(v & 0xFFFF));
}

void put_str(std::string& o, const std::string& s) {
  put_u32(o, static_cast<uint32_t>(s.size()));
  o += s;
}

struct Reader {
  std::string_view s;
  size_t i = 0;
  bool ok = true;

  uint8_t u8() {
    if (i + 1 > s.size()) { ok = false; return 0; }
    return static_cast<uint8_t>(s[i++]);
  }
  uint16_t u16() { const uint16_t hi = u8(); return static_cast<uint16_t>((hi << 8) | u8()); }
  uint32_t u32() { const uint32_t hi = u16(); return (hi << 16) | u16(); }
  std::string str() {
    const uint32_t n = u32();
    if (!ok || i + n > s.size()) { ok = false; return {}; }
    std::string out(s.substr(i, n));
    i += n;
    return out;
  }
};

std::string body_of(const Msg& m, Tag& tag) {
  std::string b;
  if (const auto* h = std::get_if<Hello>(&m)) {
    tag = Tag::Hello;
    put_u32(b, h->build_id);
    put_u16(b, h->cols);
    put_u16(b, h->rows);
    put_str(b, h->term);
    put_str(b, h->colorterm);
    put_u8(b, h->utf8 ? 1 : 0);
    put_u32(b, static_cast<uint32_t>(h->env.size()));
    for (const auto& [k, v] : h->env) { put_str(b, k); put_str(b, v); }
  } else if (std::get_if<Welcome>(&m) != nullptr) {
    tag = Tag::Welcome;
  } else if (const auto* x = std::get_if<Incompatible>(&m)) {
    tag = Tag::Incompatible;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Detached>(&m)) {
    tag = Tag::Detached;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Input>(&m)) {
    tag = Tag::Input;
    put_str(b, x->bytes);
  } else if (const auto* x = std::get_if<Resize>(&m)) {
    tag = Tag::Resize;
    put_u16(b, x->cols);
    put_u16(b, x->rows);
  } else {
    tag = Tag::Frame;
    put_str(b, std::get<FrameMsg>(m).ansi);
  }
  return b;
}

}  // namespace

std::string encode(const Msg& m) {
  Tag tag = Tag::Welcome;
  const std::string body = body_of(m, tag);
  std::string out;
  put_u8(out, static_cast<uint8_t>(tag));
  put_u32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

std::optional<Msg> Decoder::next() {
  if (buf_.size() < 5) return std::nullopt;

  Reader head{buf_, 0, true};
  const auto tag = static_cast<Tag>(head.u8());
  const uint32_t len = head.u32();
  if (buf_.size() < 5 + len) return std::nullopt;

  Reader r{std::string_view(buf_).substr(5, len), 0, true};
  std::optional<Msg> out;

  switch (tag) {
    case Tag::Hello: {
      Hello h;
      h.build_id = r.u32();
      h.cols = r.u16();
      h.rows = r.u16();
      h.term = r.str();
      h.colorterm = r.str();
      h.utf8 = r.u8() != 0;
      const uint32_t n = r.u32();
      for (uint32_t k = 0; k < n && r.ok; ++k) {
        std::string key = r.str();
        h.env.emplace_back(std::move(key), r.str());
      }
      out = Msg{std::move(h)};
      break;
    }
    case Tag::Welcome: out = Msg{Welcome{}}; break;
    case Tag::Incompatible: out = Msg{Incompatible{r.str()}}; break;
    case Tag::Detached: out = Msg{Detached{r.str()}}; break;
    case Tag::Input: out = Msg{Input{r.str()}}; break;
    case Tag::Resize: { Resize z; z.cols = r.u16(); z.rows = r.u16(); out = Msg{z}; break; }
    case Tag::Frame: out = Msg{FrameMsg{r.str()}}; break;
    default: break;  // tag inconnu : message consommé et ignoré
  }

  buf_.erase(0, 5 + len);
  if (!r.ok) return std::nullopt;
  return out;
}

}  // namespace sshos
