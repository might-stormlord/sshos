#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sshos {

// Incrémenté à chaque changement incompatible du protocole. Comparé au
// handshake : mieux vaut un message clair qu'un affichage corrompu.
inline constexpr uint32_t kBuildId = 1;

struct Hello {
  uint32_t build_id = kBuildId;
  uint16_t cols = 0;
  uint16_t rows = 0;
  std::string term;
  std::string colorterm;
  bool utf8 = false;
  std::vector<std::pair<std::string, std::string>> env;
};

struct Welcome {};
struct Incompatible { std::string reason; };
struct Detached { std::string reason; };
struct Input { std::string bytes; };
struct Resize { uint16_t cols = 0; uint16_t rows = 0; };
struct FrameMsg { std::string ansi; };

using Msg = std::variant<Hello, Welcome, Incompatible, Detached, Input, Resize,
                         FrameMsg>;

std::string encode(const Msg& m);

// Décodeur incrémental : les messages arrivent découpés n'importe comment.
class Decoder {
 public:
  void feed(std::string_view bytes) { buf_.append(bytes); }
  std::optional<Msg> next();

 private:
  std::string buf_;
};

}  // namespace sshos
