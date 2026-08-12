#pragma once

#include <optional>
#include <vector>

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
  // Quitter le CLIENT sans toucher à la session : le démon garde tout,
  // le rattachement suivant retrouve le bureau tel quel. C'est la
  // fonctionnalité phare du projet, et il lui faut un geste.
  Detach,
  ToggleMouse,
  ForceRepaint,
  // La table elle-même, affichée. Le §16 de la spec donne « la touche
  // leader est peu découvrable » comme risque, et cette aide comme parade.
  ShowHelp,
  // La touche leader tapée deux fois : à transmettre littéralement à
  // l'application, sans quoi elle deviendrait intapable sous le bureau.
  LiteralLeader,
};

// Une ligne de l'aide. `actions` n'est pas décoratif : c'est lui qui permet
// au test de couverture d'exiger que tout ce qui est atteignable au clavier
// soit documenté. Une ligne sans action documenterait du vide, et une
// action sans ligne serait une fonction que personne ne peut trouver --
// exactement le défaut que cette aide existe pour corriger.
struct HelpRow {
  const char* keys;
  const char* what;
  std::vector<Action> actions;
};

const std::vector<HelpRow>& binding_help();

// Toutes les actions que la table associe à une touche. Le pendant du
// précédent : le test croise les deux listes.
std::vector<Action> bound_actions();

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
