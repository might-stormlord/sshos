#include "daemon/host.hpp"

namespace sshos {

void HostImpl::set_title(std::string title) { win_->title = std::move(title); }

void HostImpl::request_close() { win_->close_requested = true; }

void HostImpl::invalidate() {
  // Tout changement d'état d'une application vient aujourd'hui d'une
  // entrée utilisateur, que le démon marque déjà comme salissante
  // (daemon.cpp : `while (auto e = client->input.next()) session.on_input(*e);`
  // est immédiatement suivi de `clock.mark_dirty();`). Le canal
  // session -> démon arrive à la tâche 9, avec l'horloge : c'est le premier
  // composant qui doit réclamer un repaint sans qu'on ait touché à une
  // touche.
}

uint64_t HostImpl::watch(int fd, uint32_t events) {
  (void)fd;
  (void)events;
  return 0;
}

void HostImpl::unwatch(uint64_t token) { (void)token; }

}  // namespace sshos
