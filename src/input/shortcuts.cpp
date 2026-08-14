#include "input/shortcuts.hpp"

namespace sshos {
namespace {

struct Binding {
  char32_t ch;
  Action action;
};

// Les flèches et Maj+flèches de la spec §7.4 sont le chemin principal --
// c'est ce qu'un utilisateur venu d'un vrai bureau essaie en premier. Les
// minuscules déplacent et les majuscules redimensionnent, mêmes lettres
// dans les deux cas : une doublure pour les doigts qui vivent sur hjkl.
constexpr Binding kBindings[] = {
    {U'h', Action::MoveLeft},        {U'l', Action::MoveRight},
    {U'k', Action::MoveUp},          {U'j', Action::MoveDown},
    {U'H', Action::ShrinkWidth},     {U'L', Action::GrowWidth},
    {U'K', Action::ShrinkHeight},    {U'J', Action::GrowHeight},
    {U'n', Action::NextWindow},      {U'p', Action::PrevWindow},
    {U'w', Action::Close},           {U'-', Action::Minimize},
    {U'z', Action::MaximizeToggle},  {U'f', Action::FullscreenToggle},
    {U' ', Action::OpenMenu},        {U'm', Action::ToggleMouse},
    {U'r', Action::ForceRepaint},    {U'd', Action::Detach},
    {U'?', Action::ShowHelp},
};

struct KeyBinding {
  Key key;
  bool shift;
  bool ctrl;
  Action action;
};

constexpr KeyBinding kKeyBindings[] = {
    {Key::Left, false, false, Action::MoveLeft},
    {Key::Right, false, false, Action::MoveRight},
    {Key::Up, false, false, Action::MoveUp},
    {Key::Down, false, false, Action::MoveDown},
    {Key::Left, true, false, Action::ShrinkWidth},
    {Key::Right, true, false, Action::GrowWidth},
    {Key::Up, true, false, Action::ShrinkHeight},
    {Key::Down, true, false, Action::GrowHeight},
    {Key::Tab, false, false, Action::NextWindow},
    {Key::Tab, true, false, Action::PrevWindow},
};

}  // namespace

// Cherche l'action d'une touche, dans l'une ou l'autre table.
std::optional<Action> LeaderDispatch::lookup(const KeyEvent& k) const {
  if (k.key != Key::Char) {
    // `\033[Z` ne porte aucun paramètre, donc aucun bit Maj : le terminal
    // dit « Maj+Tab » par le NOM de la touche. Les terminaux qui envoient
    // plutôt `\033[1;2I` disent la même chose par le modificateur. Les deux
    // formes se rejoignent ici, sans quoi la moitié des terminaux
    // n'auraient pas de cyclage arrière.
    Key key = k.key;
    bool shift = (k.mods & mod::Shift) != 0;
    if (key == Key::BackTab) {
      key = Key::Tab;
      shift = true;
    }
    const bool ctrl = (k.mods & mod::Ctrl) != 0;
    for (const auto& b : kKeyBindings) {
      if (b.key == key && b.shift == shift && b.ctrl == ctrl) return b.action;
    }
    return std::nullopt;
  }
  for (const auto& b : kBindings) {
    if (b.ch == k.ch) return b.action;
  }
  return std::nullopt;
}

LeaderResult LeaderDispatch::feed(const KeyEvent& k) {
  const bool is_leader =
      k.key == Key::Char && k.ch == leader_ && (k.mods & mod::Ctrl) != 0;

  if (phase_ == LeaderPhase::Idle) {
    if (is_leader) {
      phase_ = LeaderPhase::Armed;
      return LeaderResult{true, std::nullopt};
    }
    return LeaderResult{false, std::nullopt};
  }

  if (is_leader) {
    // Armé, le leader répété s'émet littéralement -- sans quoi Ctrl+A
    // deviendrait intapable pour l'application en dessous. En série, il
    // ouvre un accord franc : on veut visiblement autre chose qu'un
    // déplacement de plus.
    if (phase_ == LeaderPhase::Armed) {
      phase_ = LeaderPhase::Idle;
      return LeaderResult{true, Action::LiteralLeader};
    }
    phase_ = LeaderPhase::Armed;
    return LeaderResult{true, std::nullopt};
  }

  const std::optional<Action> a = lookup(k);

  if (phase_ == LeaderPhase::Repeating) {
    // La série ne retient QUE ce qui s'enchaîne. Tout le reste rend la main
    // à l'application sans être consommé : c'est ce qui rend la fenêtre de
    // répétition inoffensive. Sans cette règle, un « w » tapé dans un
    // document une seconde après un déplacement fermerait la fenêtre.
    if (a.has_value() && is_repeatable(*a)) return LeaderResult{true, a};
    phase_ = LeaderPhase::Idle;
    return LeaderResult{false, std::nullopt};
  }

  // Armé : l'accord ne dure qu'une touche, quelle qu'elle soit -- c'est ce
  // qui empêche le bureau de rester armé indéfiniment après une faute de
  // frappe. Sauf si le geste s'enchaîne : il ouvre alors une série, que la
  // session bornera dans le temps.
  phase_ = (a.has_value() && is_repeatable(*a)) ? LeaderPhase::Repeating
                                                : LeaderPhase::Idle;
  // Une touche sans liaison est consommée sans rien faire : l'utilisateur a
  // commencé un accord, il ne s'attend pas à voir la lettre apparaître dans
  // son document.
  return LeaderResult{true, a};
}

// Les gestes qu'on répète vraiment : pousser une fenêtre, l'étirer, faire
// le tour de la pile. Une bascule n'a aucun sens deux fois de suite, et
// « fermer » enchaîné serait une machine à détruire des fenêtres.
bool is_repeatable(Action a) {
  switch (a) {
    case Action::MoveLeft:
    case Action::MoveRight:
    case Action::MoveUp:
    case Action::MoveDown:
    case Action::GrowWidth:
    case Action::ShrinkWidth:
    case Action::GrowHeight:
    case Action::ShrinkHeight:
    case Action::NextWindow:
    case Action::PrevWindow:
      return true;
    default:
      return false;
  }
}

// L'ordre est celui de la spec §7.4 : ce qui sert le plus souvent d'abord.
// Un accord ajouté aux tables ci-dessus sans ligne ici fait échouer le test
// de couverture, et c'est le but -- une fonction que l'aide ne cite pas est
// une fonction que personne ne trouvera.
const std::vector<HelpRow>& binding_help() {
  static const std::vector<HelpRow> rows = {
      {"Flèches / hjkl", "Déplacer la fenêtre",
       {Action::MoveLeft, Action::MoveRight, Action::MoveUp, Action::MoveDown}},
      {"Maj+flèches / HJKL", "Redimensionner",
       {Action::ShrinkWidth, Action::GrowWidth, Action::ShrinkHeight,
        Action::GrowHeight}},
      {"Tab, Maj+Tab / n, p", "Fenêtre suivante, précédente",
       {Action::NextWindow, Action::PrevWindow}},
      {"w", "Fermer la fenêtre", {Action::Close}},
      {"-", "Réduire", {Action::Minimize}},
      {"z", "Maximiser (bascule)", {Action::MaximizeToggle}},
      {"f", "Plein écran (bascule)", {Action::FullscreenToggle}},
      {"Espace", "Ouvrir le menu", {Action::OpenMenu}},
      {"m", "Souris : bureau ou terminal", {Action::ToggleMouse}},
      {"r", "Tout repeindre", {Action::ForceRepaint}},
      {"d", "Détacher (la session survit)", {Action::Detach}},
      {"?", "Cette aide", {Action::ShowHelp}},
      // La touche leader n'est pas nommée en dur : elle est configurable, et
      // l'en-tête de l'aide dit laquelle c'est aujourd'hui.
      {"<leader>", "L'envoyer à l'application", {Action::LiteralLeader}},
  };
  return rows;
}

std::vector<Action> bound_actions() {
  std::vector<Action> out;
  for (const auto& b : kBindings) out.push_back(b.action);
  for (const auto& b : kKeyBindings) out.push_back(b.action);
  // Produite par feed() sans figurer dans aucune table, et pourtant bel et
  // bien atteignable au clavier : elle doit être documentée comme le reste.
  out.push_back(Action::LiteralLeader);
  return out;
}

}  // namespace sshos
