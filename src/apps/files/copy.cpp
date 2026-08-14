#include "apps/files/copy.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "apps/files/dir.hpp"

namespace sshos {
namespace {

// Le nom d'un chemin, sans son répertoire. « /a/b/c » rend « c ».
std::string base_name(const std::string& path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  const size_t cut = p.rfind('/');
  return cut == std::string::npos ? p : p.substr(cut + 1);
}

}  // namespace

void CopyJob::start(std::vector<std::string> sources, std::string dest,
                    CopyKind kind) {
  cancel();
  kind_ = kind;
  // À L'ENVERS, POUR DE VRAI : la pile se dépile par la fin, donc empiler
  // dans l'ordre les traiterait dans l'ordre inverse. Ça ne change rien au
  // résultat, mais tout à ce qu'on voit passer dans la ligne d'état -- et
  // au fichier sur lequel on s'arrête quand quelque chose échoue.
  for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
    pending_.push_back(Item{*it, join_path(dest, base_name(*it))});
  }
  active_ = !pending_.empty();
}

void CopyJob::cancel() {
  if (in_ >= 0) ::close(in_);
  if (out_ >= 0) ::close(out_);
  in_ = -1;
  out_ = -1;
  active_ = false;
  pending_.clear();
  current_.clear();
  current_from_.clear();
  current_to_.clear();
  done_ = 0;
  failed_ = 0;
  error_.clear();
}

void CopyJob::fail(const std::string& what) {
  ++failed_;
  if (error_.empty()) error_ = what;
}

void CopyJob::finish_current() {
  if (in_ >= 0) ::close(in_);
  if (out_ >= 0) ::close(out_);
  in_ = -1;
  out_ = -1;
  // DÉPLACER, C'EST COPIER PUIS EFFACER -- mais seulement quand la copie a
  // réussi. Effacer la source d'une copie qui a échoué détruirait
  // l'original pour rien.
  if (kind_ == CopyKind::Move && !current_from_.empty()) {
    ::unlink(current_from_.c_str());
  }
  ++done_;
  current_.clear();
  current_from_.clear();
  current_to_.clear();
}

bool CopyJob::take_next() {
  while (!pending_.empty()) {
    const Item it = pending_.back();
    pending_.pop_back();

    struct stat st {};
    if (::lstat(it.from.c_str(), &st) != 0) {
      fail(std::string(base_name(it.from)) + " : " + std::strerror(errno));
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      // UN RÉPERTOIRE SE CRÉE, puis son contenu s'empile. C'est le
      // parcours PARESSEUX : un `readdir()` quand on y arrive, pas un
      // parcours complet avant de commencer.
      if (::mkdir(it.to.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
        fail(std::string(base_name(it.from)) + " : " + std::strerror(errno));
        continue;
      }
      DIR* d = ::opendir(it.from.c_str());
      if (d == nullptr) {
        fail(std::string(base_name(it.from)) + " : " + std::strerror(errno));
        continue;
      }
      for (;;) {
        const dirent* e = ::readdir(d);
        if (e == nullptr) break;
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        pending_.push_back(Item{join_path(it.from, name),
                                join_path(it.to, name)});
      }
      ::closedir(d);
      // Un déplacement retire le répertoire APRÈS son contenu ; on
      // l'empile donc sous ses enfants, où il sera dépilé en dernier.
      if (kind_ == CopyKind::Move) {
        pending_.insert(pending_.begin(), Item{it.from, std::string()});
      }
      ++done_;
      continue;
    }

    // Un déplacement dont la source est un répertoire déjà vidé : sa
    // destination est vide, c'est le signal qu'il ne reste qu'à l'ôter.
    if (it.to.empty()) {
      ::rmdir(it.from.c_str());
      continue;
    }

    // LE RENOMMAGE D'ABORD, quand il est possible : déplacer dans le même
    // système de fichiers ne doit pas recopier deux gigaoctets pour rien.
    // `EXDEV` est le seul cas où il faut retomber sur la copie.
    if (kind_ == CopyKind::Move && ::rename(it.from.c_str(), it.to.c_str()) == 0) {
      ++done_;
      continue;
    }

    const int in = ::open(it.from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
      fail(base_name(it.from) + " : " + std::strerror(errno));
      continue;
    }
    // `O_EXCL` : une copie n'écrase JAMAIS. Écraser sans le dire est la
    // pire chose qu'un gestionnaire de fichiers puisse faire.
    const int out = ::open(it.to.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           st.st_mode & 07777);
    if (out < 0) {
      ::close(in);
      fail(base_name(it.to) + " : " + std::strerror(errno));
      continue;
    }
    in_ = in;
    out_ = out;
    current_ = base_name(it.from);
    current_from_ = it.from;
    current_to_ = it.to;
    return true;
  }
  return false;
}

bool CopyJob::step(size_t budget) {
  if (!active_) return false;
  if (in_ < 0 && !take_next()) {
    active_ = false;
    return false;
  }

  // UNE TRANCHE, PUIS ON REND LA MAIN. Le tampon est borné par le budget
  // pour que la mémoire retenue ne dépende pas de la taille du fichier.
  constexpr size_t kChunk = 64 * 1024;
  char buf[kChunk];
  size_t moved = 0;
  while (moved < budget) {
    const size_t want = std::min(kChunk, budget - moved);
    const ssize_t n = ::read(in_, buf, want);
    if (n < 0) {
      if (errno == EINTR) continue;
      fail(current_ + " : " + std::strerror(errno));
      // L'INCOMPLET NE RESTE PAS. Un fichier à moitié copié ressemble à un
      // fichier, et c'est ce qui le rend dangereux.
      ::close(in_);
      ::close(out_);
      in_ = -1;
      out_ = -1;
      ::unlink(current_to_.c_str());
      current_.clear();
      current_from_.clear();
      current_to_.clear();
      return true;
    }
    if (n == 0) {
      finish_current();
      return true;
    }
    size_t written = 0;
    while (written < static_cast<size_t>(n)) {
      const ssize_t w = ::write(out_, buf + written,
                                static_cast<size_t>(n) - written);
      if (w < 0) {
        if (errno == EINTR) continue;
        fail(current_ + " : " + std::strerror(errno));
        ::close(in_);
        ::close(out_);
        in_ = -1;
        out_ = -1;
        ::unlink(current_to_.c_str());
        current_.clear();
        current_from_.clear();
        current_to_.clear();
        return true;
      }
      written += static_cast<size_t>(w);
    }
    moved += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace sshos
