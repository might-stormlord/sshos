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
  Action action;
};

constexpr KeyBinding kKeyBindings[] = {
    {Key::Left, false, Action::MoveLeft},
    {Key::Right, false, Action::MoveRight},
    {Key::Up, false, Action::MoveUp},
    {Key::Down, false, Action::MoveDown},
    {Key::Left, true, Action::ShrinkWidth},
    {Key::Right, true, Action::GrowWidth},
    {Key::Up, true, Action::ShrinkHeight},
    {Key::Down, true, Action::GrowHeight},
    {Key::Tab, false, Action::NextWindow},
    {Key::Tab, true, Action::PrevWindow},
};

}  // namespace

LeaderResult LeaderDispatch::feed(const KeyEvent& k) {
  const bool is_leader =
      k.key == Key::Char && k.ch == leader_ && (k.mods & mod::Ctrl) != 0;

  if (!armed_) {
    if (is_leader) {
      armed_ = true;
      return LeaderResult{true, std::nullopt};
    }
    return LeaderResult{false, std::nullopt};
  }

  // Un accord ne dure qu'une touche, quelle qu'elle soit : c'est ce qui
  // empêche le bureau de rester armé indéfiniment après une faute de frappe.
  armed_ = false;
  if (is_leader) return LeaderResult{true, Action::LiteralLeader};

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
    for (const auto& b : kKeyBindings) {
      if (b.key == key && b.shift == shift) return LeaderResult{true, b.action};
    }
    return LeaderResult{true, std::nullopt};
  }

  for (const auto& b : kBindings) {
    if (b.ch == k.ch) return LeaderResult{true, b.action};
  }
  // Consommée sans rien faire : l'utilisateur a commencé un accord, il ne
  // s'attend pas à voir la lettre apparaître dans son document.
  return LeaderResult{true, std::nullopt};
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
