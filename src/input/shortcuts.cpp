#include "input/shortcuts.hpp"

namespace sshos {
namespace {

struct Binding {
  char32_t ch;
  Action action;
};

// Les minuscules déplacent, les majuscules redimensionnent : hjkl dans les
// deux cas, ce qui fait une seule chose à retenir au lieu de deux.
constexpr Binding kBindings[] = {
    {U'h', Action::MoveLeft},       {U'l', Action::MoveRight},
    {U'k', Action::MoveUp},         {U'j', Action::MoveDown},
    {U'L', Action::GrowWidth},      {U'H', Action::ShrinkWidth},
    {U'J', Action::GrowHeight},     {U'K', Action::ShrinkHeight},
    {U'n', Action::NextWindow},     {U'p', Action::PrevWindow},
    {U'w', Action::Close},          {U'm', Action::Minimize},
    {U'x', Action::MaximizeToggle}, {U'f', Action::FullscreenToggle},
    {U' ', Action::OpenMenu},       {U'o', Action::ToggleMouse},
    {U'd', Action::Detach},
    {U'r', Action::ForceRepaint},
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
  if (k.key != Key::Char) return LeaderResult{true, std::nullopt};

  for (const auto& b : kBindings) {
    if (b.ch == k.ch) return LeaderResult{true, b.action};
  }
  // Consommée sans rien faire : l'utilisateur a commencé un accord, il ne
  // s'attend pas à voir la lettre apparaître dans son document.
  return LeaderResult{true, std::nullopt};
}

}  // namespace sshos
