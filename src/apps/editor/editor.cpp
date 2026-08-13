#include "apps/editor/editor.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "common/utf8.hpp"
#include "render/surface.hpp"

namespace sshos {
namespace {

std::string read_whole(const std::string& path) {
  std::string out;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  char buf[8192];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) {
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return out;
}

}  // namespace

Editor::Editor() = default;

Editor::Editor(std::string path) : path_(std::move(path)) {
  buf_.load(read_whole(path_));
}

void Editor::attach(Host& host) {
  host_ = &host;
  host.set_title(path_.empty() ? "Editeur" : path_);
}

void Editor::on_resize(Size s) {
  if (s.w <= 0 || s.h <= 0) return;
  size_ = s;
  settle();
}

int Editor::rows_for_text() const {
  // Une ligne d'état en bas. Une fenêtre trop courte garde au moins une
  // ligne de texte : rendre zéro ferait disparaître le fichier.
  return std::max(1, size_.h - 1);
}

void Editor::settle() {
  cur_ = buf_.clamp(cur_);
  const size_t rows = static_cast<size_t>(rows_for_text());
  if (cur_.line < top_) top_ = cur_.line;
  if (cur_.line >= top_ + rows) top_ = cur_.line - rows + 1;
  if (top_ + rows > buf_.line_count()) {
    top_ = buf_.line_count() > rows ? buf_.line_count() - rows : 0;
  }
}

void Editor::save() {
  if (path_.empty()) {
    status_ = "aucun fichier";
    return;
  }
  // ÉCRITURE ATOMIQUE : on écrit à côté, puis on renomme. Écrire en place
  // et mourir au milieu laisse un fichier TRONQUÉ -- et c'est le fichier
  // de l'utilisateur, pas le nôtre.
  // NON DISCRIMINABLE par un test, et gardé quand même : l'atomicité ne
  // se mesure qu'en mourant au milieu de l'écriture. La mutation qui écrit
  // en place produit exactement le même fichier -- tant que rien
  // n'interrompt. C'est précisément la fenêtre d'interruption qu'on ferme
  // ici, et le fichier en jeu est celui de l'utilisateur.
  const std::string tmp = path_ + ".sshos-tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    status_ = std::string("ecriture impossible : ") + std::strerror(errno);
    return;
  }
  const std::string text = buf_.text();
  const ssize_t n = ::write(fd, text.data(), text.size());
  ::close(fd);
  if (n < 0 || static_cast<size_t>(n) != text.size()) {
    ::unlink(tmp.c_str());
    status_ = "ecriture incomplete";
    return;
  }
  if (::rename(tmp.c_str(), path_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    status_ = std::string("remplacement impossible : ") + std::strerror(errno);
    return;
  }
  buf_.mark_saved();
  status_ = "enregistre";
}

CloseCheck Editor::can_close() const {
  // Le [×] du cadre passe par ici : sans cette question, un clic dessus
  // perdrait le travail en cours.
  if (buf_.modified()) {
    return CloseCheck::ask("Le fichier a change. Fermer sans enregistrer ?");
  }
  return CloseCheck::allow();
}

void Editor::on_key(const KeyEvent& k) {
  if (mode_ == Mode::Searching) {
    switch (k.key) {
      case Key::Enter: {
        mode_ = Mode::Normal;
        TextPos found;
        // On repart d'UNE COLONNE PLUS LOIN : sinon la même occurrence se
        // retrouve indéfiniment et la recherche n'avance jamais.
        const TextPos from{cur_.line, cur_.col + 1};
        if (buf_.find(query_, from, found)) {
          cur_ = found;
          status_.clear();
          settle();
        } else {
          status_ = "introuvable : " + query_;
        }
        return;
      }
      case Key::Escape:
        mode_ = Mode::Normal;
        query_.clear();
        return;
      case Key::Backspace:
        if (!query_.empty()) query_.pop_back();
        return;
      case Key::Char:
        if (k.ch >= U' ') query_ += encode_utf8(k.ch);
        return;
      default:
        return;
    }
  }
  if (mode_ == Mode::Confirming) {
    if (k.key == Key::Char && (k.ch == U'o' || k.ch == U'O' || k.ch == U'y' ||
                               k.ch == U'Y')) {
      mode_ = Mode::Normal;
      if (host_ != nullptr) host_->request_close();
      return;
    }
    mode_ = Mode::Normal;
    return;
  }

  const size_t rows = static_cast<size_t>(rows_for_text());
  switch (k.key) {
    case Key::Up:
      if (cur_.line > 0) --cur_.line;
      settle();
      return;
    case Key::Down:
      if (cur_.line + 1 < buf_.line_count()) ++cur_.line;
      settle();
      return;
    case Key::Left:
      if (cur_.col > 0) {
        --cur_.col;
      } else if (cur_.line > 0) {
        // Remonter en bout de ligne précédente : c'est ce que fait tout
        // éditeur, et s'arrêter net donne une flèche sans effet.
        --cur_.line;
        cur_.col = buf_.line(cur_.line).size();
      }
      settle();
      return;
    case Key::Right:
      if (cur_.col < buf_.line(cur_.line).size()) {
        ++cur_.col;
      } else if (cur_.line + 1 < buf_.line_count()) {
        ++cur_.line;
        cur_.col = 0;
      }
      settle();
      return;
    case Key::Home:
      cur_.col = 0;
      settle();
      return;
    case Key::End:
      cur_.col = buf_.line(cur_.line).size();
      settle();
      return;
    case Key::PgUp:
      cur_.line = cur_.line > rows ? cur_.line - rows : 0;
      settle();
      return;
    case Key::PgDn:
      cur_.line = std::min(cur_.line + rows, buf_.line_count() - 1);
      settle();
      return;
    case Key::Enter:
      cur_ = buf_.split_line(cur_);
      settle();
      return;
    case Key::Backspace:
      cur_ = buf_.erase_before(cur_);
      settle();
      return;
    case Key::Delete:
      cur_ = buf_.erase_at(cur_);
      settle();
      return;
    case Key::Char:
      break;
    default:
      return;
  }

  if ((k.mods & mod::Ctrl) != 0) {
    // `Ctrl+Q` n'arrivera jamais : le bureau l'intercepte pour détacher.
    if (k.ch == U's') {
      save();
    } else if (k.ch == U'x') {
      if (buf_.modified()) {
        mode_ = Mode::Confirming;
      } else if (host_ != nullptr) {
        host_->request_close();
      }
    } else if (k.ch == U'f') {
      mode_ = Mode::Searching;
      query_.clear();
    }
    return;
  }

  // Un caractère de CONTRÔLE ne s'insère pas : il donnerait un fichier
  // qu'aucun autre éditeur ne relirait proprement.
  if (k.ch < U' ') return;
  cur_ = buf_.insert(cur_, encode_utf8(k.ch));
  status_.clear();
  settle();
}

bool Editor::wants_cursor(Pos& out) const {
  // Garde DÉFENSIVE, non discriminable : `settle()` maintient toujours
  // `top_ <= cur_.line`. Elle reste parce qu'elle dit ce que la fonction
  // suppose, et qu'un futur défilement indépendant du curseur -- une
  // molette, par exemple -- la rendrait porteuse du jour au lendemain.
  if (cur_.line < top_) return false;
  const int y = static_cast<int>(cur_.line - top_);
  if (y >= rows_for_text()) return false;
  out = Pos{static_cast<int>(cur_.col), y};
  return true;
}

void Editor::render(View v) {
  const int w = v.w();
  const int h = v.h();
  if (w <= 0 || h <= 0) return;

  const int rows = rows_for_text();
  for (int i = 0; i < rows; ++i) {
    const size_t idx = top_ + static_cast<size_t>(i);
    if (idx >= buf_.line_count()) break;
    v.text(0, i, buf_.line(idx), Style{});
  }

  Style st;
  st.attrs = attr::Reverse;
  std::string bottom;
  if (mode_ == Mode::Searching) {
    bottom = "chercher: " + query_;
  } else if (mode_ == Mode::Confirming) {
    bottom = "quitter sans enregistrer ? (o/n)";
  } else {
    // Le nom ET la marque de modification : sans la seconde, on
    // enregistre par superstition.
    bottom = (buf_.modified() ? "*" : " ") +
             (path_.empty() ? std::string("(sans nom)") : path_) + "  L" +
             std::to_string(cur_.line + 1) + " C" +
             std::to_string(cur_.col + 1);
    if (!status_.empty()) bottom += "  " + status_;
  }
  v.fill(Rect{0, h - 1, w, 1}, st);
  v.text(0, h - 1, bottom, st);
}

}  // namespace sshos
