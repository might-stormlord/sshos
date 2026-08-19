#pragma once

#include <optional>
#include <string>
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
  // L'ANCRAGE, geste des bureaux modernes : la fenetre prend la moitie de
  // l'ecran du cote de la fleche. La touche « Tux » n'existe pas dans un
  // terminal -- aucun n'en rapporte l'etat -- d'ou le leader suivi de
  // Ctrl+fleche, la seule combinaison d'fleche encore libre apres le
  // deplacement (fleche nue) et le redimensionnement (Maj+fleche).
  SnapLeft,
  SnapRight,
  SnapUp,
  SnapDown,
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

// LES GESTES SANS ACCORD. Ils ne passent pas par la touche leader, donc ce
// ne sont pas des `Action` -- ni `bound_actions()` ni les deux gardes de
// couverture ne les voient. Sans table a eux, ils n'existaient nulle part :
// les onglets du terminal ont ete livres avec quatre raccourcis que rien
// ne citait, et l'ancrage avec une parenthese coincee dans l'en-tete.
struct DirectRow {
  std::string keys;
  std::string what;
};
const std::vector<DirectRow>& direct_help();

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

// Un accord qui s'enchaîne : après lui, la touche suivante agit sans qu'on
// reprenne le leader. Déplacer une fenêtre de dix cellules demandait sinon
// dix Ctrl+A, ce qui rend le clavier inutilisable pour ce à quoi il sert le
// plus. Seuls les gestes qu'on répète naturellement le sont -- pas les
// bascules, qui n'ont aucun sens deux fois de suite.
bool is_repeatable(Action a);

// Idle : la frappe appartient à l'application.
// Armed : le leader vient d'être tapé, la touche suivante est une commande.
// Repeating : un geste enchaînable vient d'agir. Seuls les gestes
//   enchaînables restent captés ; TOUT LE RESTE repart à l'application sans
//   être consommé. C'est ce qui rend la fenêtre de répétition sans danger --
//   au pire un « j » déplace au lieu de s'écrire, jamais un « w » ne ferme.
enum class LeaderPhase { Idle, Armed, Repeating };

// Accord à deux temps : une touche leader, puis une lettre. Le dispatcheur
// ne connaît que la table ; c'est la session qui sait exécuter.
class LeaderDispatch {
 public:
  explicit LeaderDispatch(char32_t leader = U'a') : leader_(leader) {}

  LeaderResult feed(const KeyEvent& k);

  bool armed() const { return phase_ == LeaderPhase::Armed; }
  bool repeating() const { return phase_ == LeaderPhase::Repeating; }

  // La série n'a pas d'horloge à elle : c'est la session qui tient le délai
  // et qui vient la clore. Le dispatcheur reste sans notion de temps, comme
  // au premier jour -- une table qui saurait l'heure serait intestable.
  void reset() { phase_ = LeaderPhase::Idle; }

  char32_t leader() const { return leader_; }

 private:
  std::optional<Action> lookup(const KeyEvent& k) const;

  char32_t leader_;
  LeaderPhase phase_ = LeaderPhase::Idle;
};

}  // namespace sshos
