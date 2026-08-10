#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace sshos {

// Couture d'injection. Tout ce qui touche au monde extérieur passe par
// ici : sans ça l'horloge du panneau rend le harnais non déterministe.
// `spawn()` rejoindra cette interface au jalon 3, avec le Terminal.
struct Platform {
  virtual ~Platform() = default;
  virtual std::chrono::system_clock::time_point now() const = 0;
  virtual std::string read_file(std::string_view path) const = 0;
};

struct RealPlatform : Platform {
  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::now();
  }
  std::string read_file(std::string_view path) const override;
};

}  // namespace sshos
