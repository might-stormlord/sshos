#pragma once

#include <cstdint>
#include <string_view>

#include "app/app.hpp"

namespace sshos {

// Application factice AVEC descripteur. Elle ouvre un tuyau sur elle-même,
// en surveille l'extrémité de lecture et compte ce qui arrive. Aucune
// utilité pour l'utilisateur : son rôle est d'exercer le chemin
// watch/on_io/unwatch de bout en bout, celui qu'aucune application sans fd
// ne peut atteindre.
class Battement : public App {
 public:
  ~Battement() override;

  void attach(Host& host) override;
  void render(View v) override;
  IoStatus on_io(uint64_t token, uint32_t events) override;
  void on_command(std::string_view cmd) override;
  Size min_size() const override { return {16, 2}; }

  // Écrit un octet dans le tuyau. Appelée par les tests et par le menu.
  void beat();

  // Ferme l'extrémité d'écriture : le prochain réveil portera un EPOLLHUP.
  void cut_source();

  int beats() const { return beats_; }
  bool source_alive() const { return read_fd_ >= 0; }

 private:
  void close_pipe();

  Host* host_ = nullptr;
  uint64_t token_ = 0;
  bool watching_ = false;
  int read_fd_ = -1;
  int write_fd_ = -1;
  int beats_ = 0;
};

}  // namespace sshos
