#include "vt/attrs.hpp"

#include <cstddef>
#include <cstdint>

namespace sshos {
namespace {

bool is_byte(int v) { return v >= 0 && v <= 255; }

// Lit la couleur décrite par `[first, last)`, où `first` désigne le MODE
// (`5` indexé, `2` triplet) et le reste ses arguments.
//
// Les deux orthographes se rejoignent ici, et c'est tout l'intérêt de
// découper l'étendue en amont : que les arguments soient venus après des
// `;` ou après des `:`, ils occupent la même tranche, et lire les derniers
// plutôt que les premiers absorbe le champ d'espace colorimétrique vide de
// `38:2::r:g:b` sans avoir à le compter.
//
// Rend false sur une description tronquée ou hors bornes -- l'appelant
// laisse alors la couleur en place. Rabattre un indice de 300 par
// troncature donnerait une couleur silencieusement fausse ; ne rien
// changer se voit tout de suite.
bool read_extended(const Params& p, size_t first, size_t last, Color& out) {
  if (first >= last) return false;
  const int mode = p[first].value;
  const size_t args = last - first - 1;

  if (mode == 5) {
    if (args < 1) return false;
    const int idx = p[last - 1].value;
    if (!is_byte(idx)) return false;
    out = Color::indexed(static_cast<uint8_t>(idx));
    return true;
  }
  if (mode == 2) {
    if (args < 3) return false;
    const int r = p[last - 3].value;
    const int g = p[last - 2].value;
    const int b = p[last - 1].value;
    if (!is_byte(r) || !is_byte(g) || !is_byte(b)) return false;
    out = Color::rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                     static_cast<uint8_t>(b));
    return true;
  }
  return false;
}

// L'étendue d'une couleur étendue écrite à plat (`38;5;n`), où le mode et
// ses arguments sont des paramètres à part entière. Rend l'index de fin,
// borné à la taille : une séquence tronquée donne une tranche trop courte,
// que read_extended refuse.
size_t flat_extent(const Params& p, size_t first) {
  if (first >= p.size()) return first;
  const int mode = p[first].value;
  size_t want = first + 1;  // mode inconnu : on consomme le mode, rien de plus
  if (mode == 5) want = first + 2;
  if (mode == 2) want = first + 4;
  return want < p.size() ? want : p.size();
}

}  // namespace

void apply_sgr(const Params& params, Style& style) {
  if (params.empty()) {
    style = Style{};
    return;
  }

  size_t i = 0;
  while (i < params.size()) {
    // Le groupe court du code jusqu'au dernier de ses sous-paramètres.
    // Tout ce qui est consommé ici est consommé ENSEMBLE, y compris pour
    // les codes qu'on n'interprète pas : c'est ce qui empêche un
    // sous-paramètre de se relire comme un code autonome.
    size_t end = i + 1;
    while (end < params.size() && params[end].sub) ++end;

    const int code = params[i].value < 0 ? 0 : params[i].value;

    if (code == 38 || code == 48 || code == 58) {
      const size_t first = i + 1;
      // Étendue en sous-paramètres si le groupe en porte, à plat sinon.
      const size_t last = (end > first) ? end : flat_extent(params, first);
      Color c;
      if (read_extended(params, first, last, c)) {
        if (code == 38) style.fg = c;
        if (code == 48) style.bg = c;
        // 58 : lu pour être avalé proprement, puis jeté -- le rendu n'a pas
        // de couleur de soulignement.
      }
      // Même quand la lecture échoue, on saute la tranche : c'est
      // justement une couleur tronquée qui laisserait son `5` se relire en
      // clignotant.
      i = (last > i) ? last : i + 1;
      continue;
    }

    switch (code) {
      case 0:
        style = Style{};
        break;
      case 1:
        style.attrs |= attr::Bold;
        break;
      case 2:
        style.attrs |= attr::Dim;
        break;
      case 3:
        style.attrs |= attr::Italic;
        break;
      case 4:
        // `4:0` éteint, `4:1` à `4:5` posent un style de trait qu'on ne
        // distingue pas. Un `4` nu pose, comme toujours.
        if (end > i + 1 && param_or(params, i + 1, 1) == 0) {
          style.attrs &= ~attr::Underline;
        } else {
          style.attrs |= attr::Underline;
        }
        break;
      case 5:
      case 6:
        // Le clignotement rapide se replie sur le lent : aucun terminal
        // moderne ne les distingue, et le rendre visible vaut mieux que de
        // l'ignorer.
        style.attrs |= attr::Blink;
        break;
      case 7:
        style.attrs |= attr::Reverse;
        break;
      case 8:
        style.attrs |= attr::Hidden;
        break;
      case 9:
        style.attrs |= attr::Strike;
        break;
      case 21:
        // Double soulignement (xterm), pas « gras éteint » (ECMA-48). On
        // promet `xterm-256color` à l'invité ; on tient la promesse.
        style.attrs |= attr::Underline;
        break;
      case 22:
        // La seule extinction qui en vise deux : rien n'éteint le gras
        // seul, ni le faible seul.
        style.attrs &= static_cast<uint16_t>(~(attr::Bold | attr::Dim));
        break;
      case 23:
        style.attrs &= static_cast<uint16_t>(~attr::Italic);
        break;
      case 24:
        style.attrs &= static_cast<uint16_t>(~attr::Underline);
        break;
      case 25:
        style.attrs &= static_cast<uint16_t>(~attr::Blink);
        break;
      case 27:
        style.attrs &= static_cast<uint16_t>(~attr::Reverse);
        break;
      case 28:
        style.attrs &= static_cast<uint16_t>(~attr::Hidden);
        break;
      case 29:
        style.attrs &= static_cast<uint16_t>(~attr::Strike);
        break;
      case 39:
        style.fg = Color::def();
        break;
      case 49:
        style.bg = Color::def();
        break;
      default:
        if (code >= 30 && code <= 37) {
          style.fg = Color::indexed(static_cast<uint8_t>(code - 30));
        } else if (code >= 40 && code <= 47) {
          style.bg = Color::indexed(static_cast<uint8_t>(code - 40));
        } else if (code >= 90 && code <= 97) {
          // Les vives sont les indices 8 à 15 de la palette, pas « la même
          // en gras » : les confondre rend un thème sombre illisible.
          style.fg = Color::indexed(static_cast<uint8_t>(code - 90 + 8));
        } else if (code >= 100 && code <= 107) {
          style.bg = Color::indexed(static_cast<uint8_t>(code - 100 + 8));
        }
        // Tout le reste est ignoré, mais son groupe a bien été consommé.
        break;
    }
    i = end;
  }
}

}  // namespace sshos
