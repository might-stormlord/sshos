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
  // I4 : position jusqu'où buf_ a déjà été scanné sans y trouver kPasteEnd,
  // pour ne pas rescanner tout le collage à chaque octet livré. Remise à 0
  // chaque fois que buf_ est tronqué en tête (terminateur trouvé, ou
  // fragment évacué par le plafond C2) : les positions qu'elle désignait ne
  // veulent plus rien dire une fois le préfixe supprimé.
  size_t paste_scanned_ = 0;
};

}  // namespace sshos
