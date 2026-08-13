#include "apps/editor/buffer.hpp"

#include <algorithm>

namespace sshos {
namespace {

// Rendue par `line()` hors bornes. Statique : rendre une référence sur un
// temporaire serait un pointeur pendant, et l'appelant est un dessin.
const std::string& nowhere() {
  static const std::string empty;
  return empty;
}

}  // namespace

TextBuffer::TextBuffer() : lines_{""} {}

void TextBuffer::load(const std::string& text) {
  lines_.clear();
  // L'ABSENCE de saut de ligne final est retenue : en rajouter un qui n'y
  // était pas fait grossir le fichier d'un octet à chaque enregistrement,
  // et rend un diff bruyant.
  trailing_newline_ = text.empty() || text.back() == '\n';

  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size()) lines_.push_back(text.substr(start));
      break;
    }
    lines_.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  // TOUJOURS au moins une ligne : un fichier vide vaut une ligne vide.
  if (lines_.empty()) lines_.push_back("");
  // Charger n'est pas modifier : un tampon fraîchement ouvert ne doit pas
  // poser la question à la fermeture.
  modified_ = false;
}

std::string TextBuffer::text() const {
  std::string out;
  for (size_t i = 0; i < lines_.size(); ++i) {
    out += lines_[i];
    if (i + 1 < lines_.size()) out.push_back('\n');
  }
  if (trailing_newline_) out.push_back('\n');
  return out;
}

const std::string& TextBuffer::line(size_t i) const {
  if (i >= lines_.size()) return nowhere();
  return lines_[i];
}

TextPos TextBuffer::clamp(TextPos p) const {
  if (lines_.empty()) return TextPos{0, 0};
  if (p.line >= lines_.size()) p.line = lines_.size() - 1;
  if (p.col > lines_[p.line].size()) p.col = lines_[p.line].size();
  return p;
}

TextPos TextBuffer::insert(TextPos at, const std::string& s) {
  at = clamp(at);
  if (s.empty()) return at;
  lines_[at.line].insert(at.col, s);
  modified_ = true;
  return TextPos{at.line, at.col + s.size()};
}

TextPos TextBuffer::split_line(TextPos at) {
  at = clamp(at);
  const std::string tail = lines_[at.line].substr(at.col);
  lines_[at.line].erase(at.col);
  lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at.line) + 1, tail);
  modified_ = true;
  return TextPos{at.line + 1, 0};
}

TextPos TextBuffer::erase_before(TextPos at) {
  at = clamp(at);
  if (at.col > 0) {
    lines_[at.line].erase(at.col - 1, 1);
    modified_ = true;
    return TextPos{at.line, at.col - 1};
  }
  // Tout au début : il n'y a rien à effacer, et surtout rien à signaler
  // comme une modification.
  if (at.line == 0) return at;
  // Sinon on FUSIONNE avec la ligne du dessus, et le curseur se pose à la
  // jointure -- c'est ce que fait tout éditeur.
  const size_t join = lines_[at.line - 1].size();
  lines_[at.line - 1] += lines_[at.line];
  lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(at.line));
  modified_ = true;
  return TextPos{at.line - 1, join};
}

TextPos TextBuffer::erase_at(TextPos at) {
  at = clamp(at);
  if (at.col < lines_[at.line].size()) {
    lines_[at.line].erase(at.col, 1);
    modified_ = true;
    return at;
  }
  // En fin de ligne, on tire la suivante à soi.
  if (at.line + 1 >= lines_.size()) return at;
  lines_[at.line] += lines_[at.line + 1];
  lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(at.line) + 1);
  modified_ = true;
  return at;
}

bool TextBuffer::find(const std::string& needle, TextPos from,
                      TextPos& out) const {
  if (needle.empty() || lines_.empty()) return false;
  from = clamp(from);

  // Deux passes : d'ici à la fin, puis du début jusqu'ici. La recherche
  // BOUCLE -- sans cela, la deuxième occurrence d'un mot devient
  // introuvable dès qu'on l'a dépassée.
  for (size_t pass = 0; pass < 2; ++pass) {
    const size_t first = pass == 0 ? from.line : 0;
    // La borne de la seconde passe est ÉQUIVALENTE à `lines_.size()` :
    // tout ce qui est après `from.line` a déjà été vu par la première, et
    // le premier résultat serait le même. Elle reste parce qu'elle dit ce
    // que la passe est censée couvrir -- le début, jusqu'au point de
    // départ.
    const size_t last = pass == 0 ? lines_.size() : from.line + 1;
    for (size_t i = first; i < last && i < lines_.size(); ++i) {
      const size_t start = (pass == 0 && i == from.line) ? from.col : 0;
      if (start > lines_[i].size()) continue;
      const size_t at = lines_[i].find(needle, start);
      if (at != std::string::npos) {
        out = TextPos{i, at};
        return true;
      }
    }
  }
  return false;
}

}  // namespace sshos
