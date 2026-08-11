#include "daemon/host.hpp"

#include <algorithm>

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
  // Génération neuve à CHAQUE watch(), jamais une par fenêtre : une
  // application qui referme et rouvre son tuyau récupère le même numéro de
  // descripteur du noyau et doit malgré tout obtenir une clé neuve.
  const uint64_t key = make_key(win_->id, (*gen_)++);
  watched_.emplace_back(key, fd);
  fds_->watch(key, fd, events);
  return key;
}

void HostImpl::unwatch(uint64_t token) {
  const auto it = std::find_if(
      watched_.begin(), watched_.end(),
      [token](const auto& p) { return p.first == token; });
  // Un jeton inconnu est ignoré sans bruit : une application qui se
  // débranche deux fois -- une fois sur EPOLLHUP, une fois dans son
  // destructeur -- fait quelque chose de parfaitement raisonnable.
  if (it == watched_.end()) return;
  fds_->unwatch(it->second);
  watched_.erase(it);
}

bool HostImpl::owns(uint64_t key) const {
  return std::any_of(watched_.begin(), watched_.end(),
                     [key](const auto& p) { return p.first == key; });
}

IoStatus HostImpl::deliver(uint64_t key, uint32_t events) {
  if (!owns(key)) return IoStatus::Ok;
  return win_->app->on_io(key, events);
}

void HostImpl::unwatch_all() {
  for (const auto& p : watched_) fds_->unwatch(p.second);
  watched_.clear();
}

}  // namespace sshos
