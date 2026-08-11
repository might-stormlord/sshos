#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "input/events.hpp"
#include "render/cell.hpp"
#include "render/surface.hpp"

namespace sshos {

// Réponse d'une application à « puis-je te fermer ? ». Le refus porte sa
// propre question : c'est l'application qui sait ce qu'elle risque de
// perdre, pas le gestionnaire de fenêtres.
struct CloseCheck {
  bool allowed = true;
  std::string question;

  static CloseCheck allow() { return {}; }
  static CloseCheck ask(std::string q) { return {false, std::move(q)}; }
};

// Ce qu'une application répond après avoir traité un événement sur l'un de
// ses descripteurs. `Closed` veut dire « cette source est morte » : l'hôte
// retire la surveillance et l'application décide ensuite si elle veut
// vivre sans.
enum class IoStatus { Ok, Closed };

// Ce qu'une application a le droit de demander au bureau. Elle ne reçoit
// jamais l'epoll ni son propre numéro de fenêtre : watch() rend un jeton
// opaque, que on_io() lui rendra tel quel.
class Host {
 public:
  virtual ~Host() = default;
  virtual void set_title(std::string title) = 0;
  virtual void request_close() = 0;
  virtual void invalidate() = 0;
  virtual uint64_t watch(int fd, uint32_t events) = 0;
  virtual void unwatch(uint64_t token) = 0;
};

// Le contrat applicatif du projet, valable jusqu'au jalon 6. Tout est
// virtuel avec un défaut utilisable : une application qui ne sait que
// dessiner n'a qu'une méthode à écrire.
class App {
 public:
  virtual ~App() = default;

  // Appelée une fois, à l'ouverture de la fenêtre. La référence reste
  // valable tant que l'application vit.
  virtual void attach(Host& host) { (void)host; }

  // La View est déjà clippée sur la zone cliente : écrire en dehors est
  // ignoré, pas une erreur. C'est ce qui rend structurellement impossible
  // qu'une application peigne par-dessus la barre des tâches.
  virtual void render(View v) = 0;

  virtual void on_key(const KeyEvent& k) { (void)k; }

  // Coordonnées LOCALES à la zone cliente, jamais celles de l'écran.
  virtual void on_mouse(const MouseEvent& m) { (void)m; }

  virtual void on_resize(Size s) { (void)s; }

  virtual IoStatus on_io(uint64_t token, uint32_t events) {
    (void)token;
    (void)events;
    return IoStatus::Ok;
  }

  // `out` en coordonnées locales. Rend false si l'application ne veut pas
  // de curseur visible.
  virtual bool wants_cursor(Pos& out) const {
    (void)out;
    return false;
  }

  // Porte sur la ZONE CLIENTE, pas sur le cadre. Le gestionnaire de
  // fenêtres y ajoute les décorations (voir frame_min, tâche 3).
  virtual Size min_size() const { return {10, 3}; }

  virtual CloseCheck can_close() const { return CloseCheck::allow(); }
};

}  // namespace sshos
