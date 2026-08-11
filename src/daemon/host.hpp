#pragma once

#include <cstdint>
#include <string>

#include "app/app.hpp"
#include "wm/window.hpp"

namespace sshos {

// Le seul objet qui traverse la frontière entre une application et le
// bureau. Il tient une référence sur SA fenêtre, dont l'adresse est stable
// parce que les fenêtres vivent derrière des unique_ptr, jamais dans un
// vector d'objets déplaçables.
class HostImpl : public Host {
 public:
  explicit HostImpl(Window& win) : win_(&win) {}

  void set_title(std::string title) override;
  void request_close() override;
  void invalidate() override;

  // watch/unwatch sont câblés à la tâche 8, avec le FdRegistrar et les clés
  // générationnelles. Ne rien faire est le comportement CORRECT tant
  // qu'aucune application du catalogue ne surveille de descripteur : Bloc
  // n'en a pas, et Battement -- qui en a un -- arrive avec le registrar.
  uint64_t watch(int fd, uint32_t events) override;
  void unwatch(uint64_t token) override;

 private:
  Window* win_;
};

}  // namespace sshos
