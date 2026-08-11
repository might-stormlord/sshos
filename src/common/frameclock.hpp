#pragma once

#include <chrono>
#include <optional>

namespace sshos {

// Drapeau « sale » plus plafond de cadence. Le démon ne compose pas une
// frame par octet reçu : il draine tout ce qui est lisible, marque sale,
// et compose au plus une fois par intervalle.
class FrameClock {
 public:
  using Clock = std::chrono::steady_clock;

  explicit FrameClock(std::chrono::milliseconds min_interval)
      : min_interval_(min_interval) {}

  void mark_dirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }

  // -1 : rien à faire. 0 : composer maintenant. n > 0 : armer le timer.
  int delay_ms(Clock::time_point now) const {
    if (!dirty_) return -1;
    if (!last_.has_value()) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_.value());
    if (elapsed >= min_interval_) return 0;
    return static_cast<int>((min_interval_ - elapsed).count());
  }

  void note_render(Clock::time_point now) {
    dirty_ = false;
    last_ = now;
  }

  // Remet l'horloge à son état initial : plus rien de sale, plus de dernier
  // rendu connu. À appeler quand le client qui aurait pu justifier ces
  // marques disparaît (déconnexion) : sans ça, un dirty_ posé avant tout
  // Hello (donc jamais consommé par note_render(), faute de differ à
  // rendre) survivrait à la déconnexion et referait réclamer un rendu
  // immédiat par delay_ms() alors qu'il n'y a plus personne à qui l'envoyer.
  void reset() {
    dirty_ = false;
    last_.reset();
  }

 private:
  std::chrono::milliseconds min_interval_;
  std::optional<Clock::time_point> last_;
  bool dirty_ = false;
};

}  // namespace sshos
