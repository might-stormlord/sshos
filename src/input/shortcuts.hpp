#pragma once

#include <optional>

#include "input/events.hpp"

namespace sshos {

enum class Action {
  MoveLeft,
  MoveRight,
  MoveUp,
  MoveDown,
  GrowWidth,
  ShrinkWidth,
  GrowHeight,
  ShrinkHeight,
  NextWindow,
  PrevWindow,
  Close,
  Minimize,
  MaximizeToggle,
  FullscreenToggle,
  OpenMenu,
  ToggleMouse,
  ForceRepaint,
  // La touche leader tapée deux fois : à transmettre littéralement à
  // l'application, sans quoi elle deviendrait intapable sous le bureau.
  LiteralLeader,
};

// Deux états qu'un seul optional ne sait pas dire : quand Ctrl+A ARME le
// dispatcheur, il n'y a pas d'action à exécuter et la touche ne doit
// pourtant pas être transmise à l'application.
struct LeaderResult {
  bool consumed = false;
  std::optional<Action> action;
};

// Accord à deux temps : une touche leader, puis une lettre. Le dispatcheur
// ne connaît que la table ; c'est la session qui sait exécuter.
class LeaderDispatch {
 public:
  explicit LeaderDispatch(char32_t leader = U'a') : leader_(leader) {}

  LeaderResult feed(const KeyEvent& k);
  bool armed() const { return armed_; }
  char32_t leader() const { return leader_; }

 private:
  char32_t leader_;
  bool armed_ = false;
};

}  // namespace sshos
