#include "daemon/host.hpp"

#include <algorithm>

namespace sshos {

void HostImpl::set_title(std::string title) { win_->title = std::move(title); }

void HostImpl::request_close() { win_->close_requested = true; }

void HostImpl::invalidate() {
  // Le canal application -> session -> démon. Il ne servait à rien tant que
  // tout changement d'état venait d'une frappe, que le démon marque déjà
  // comme salissante ; l'horloge du panneau est le premier composant à
  // réclamer un repeint sans qu'on ait touché à une touche, et ce drapeau
  // est ce que le démon relève une fois par tour (Session::take_dirty).
  *dirty_ = true;
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

void HostImpl::watch_child(pid_t pid) {
  children_->push_back(ChildWatch{pid, win_->id});
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
