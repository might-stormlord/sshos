#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "input/events.hpp"

namespace sshos {

class InputParser {
 public:
  void feed(std::string_view bytes) {
    buf_.append(bytes);
    pump();
  }

  std::optional<InputEvent> next() {
    if (ready_.empty()) return std::nullopt;
    InputEvent e = std::move(ready_.front());
    ready_.pop_front();
    return e;
  }

  // Appelé quand le délai d'ambiguïté a expiré sans octet supplémentaire.
  void timeout();

  bool esc_pending() const { return esc_pending_; }

 private:
  void pump();
  // Rend le nombre d'octets consommés, ou 0 si la séquence est incomplète.
  size_t step();

  std::string buf_;
  std::deque<InputEvent> ready_;
  bool in_paste_ = false;
  bool esc_pending_ = false;
};

}  // namespace sshos
