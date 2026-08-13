#include "apps/files/dir.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace sshos {
namespace {

constexpr char kParent[] = "..";

// Le pliage de casse, ASCII SEULEMENT. Plier l'Unicode demanderait une
// table de plus, et le tri d'un gestionnaire de fichiers n'est pas
// l'endroit où la payer : ce qui compte est qu'il soit STABLE et
// prévisible, pas qu'il suive les règles de collation d'une locale.
char fold(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Compare deux noms sans tenir compte de la casse. Rend <0, 0 ou >0.
int compare_folded(const std::string& a, const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char ca = fold(a[i]);
    const char cb = fold(b[i]);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

EntryKind kind_of(const std::string& full, unsigned char d_type) {
  // `d_type` évite un `stat()` par entrée -- sur un répertoire de dix
  // mille fichiers, c'est dix mille appels système en moins. Mais tous les
  // systèmes de fichiers ne le remplissent pas : `DT_UNKNOWN` oblige à
  // demander pour de vrai.
  switch (d_type) {
    case DT_DIR:
      return EntryKind::Dir;
    case DT_REG:
      return EntryKind::File;
    case DT_LNK:
      return EntryKind::Link;
    case DT_UNKNOWN:
      break;
    default:
      return EntryKind::Other;
  }
  struct stat st {};
  if (::lstat(full.c_str(), &st) != 0) return EntryKind::Other;
  if (S_ISLNK(st.st_mode)) return EntryKind::Link;
  if (S_ISDIR(st.st_mode)) return EntryKind::Dir;
  if (S_ISREG(st.st_mode)) return EntryKind::File;
  return EntryKind::Other;
}

}  // namespace

std::string parent_path(const std::string& path) {
  // Pas de garde pour la racine ni pour le vide : les deux retombent sur
  // le `cut` ci-dessous, qui rend « / » dans les deux cas. Une garde en
  // tête a été écrite, puis retirée -- la campagne de mutation l'a montrée
  // inobservable.
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  const size_t cut = p.rfind('/');
  if (cut == std::string::npos) return "/";
  if (cut == 0) return "/";
  return p.substr(0, cut);
}

std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

DirListing read_dir(const std::string& path, bool show_hidden) {
  DirListing out;
  out.path = path;

  DIR* d = ::opendir(path.c_str());
  if (d == nullptr) {
    // Le message porte la RAISON. « Impossible d'ouvrir » sans le pourquoi
    // laisse l'utilisateur devant le même panneau blanc qu'un silence.
    out.error = std::string("lecture impossible : ") + std::strerror(errno);
    return out;
  }

  // La racine n'a pas de parent : l'offrir ferait tourner en rond.
  if (path != "/") out.entries.push_back(DirEntry{kParent, EntryKind::Dir, 0});

  for (;;) {
    errno = 0;
    const dirent* e = ::readdir(d);
    if (e == nullptr) break;
    const std::string name = e->d_name;
    if (name == "." || name == kParent) continue;
    if (!show_hidden && !name.empty() && name[0] == '.') continue;

    const std::string full = join_path(path, name);
    DirEntry entry;
    entry.name = name;
    entry.kind = kind_of(full, e->d_type);
    if (entry.kind == EntryKind::File) {
      struct stat st {};
      if (::stat(full.c_str(), &st) == 0) {
        entry.size = static_cast<uint64_t>(st.st_size);
      }
    }
    out.entries.push_back(std::move(entry));
  }
  ::closedir(d);

  sort_entries(out.entries);
  return out;
}

void sort_entries(std::vector<DirEntry>& entries) {
  // La comparaison ci-dessous est un ORDRE TOTAL : aucun couple de noms
  // distincts n'y est égal. `stable_sort` n'apporte donc rien de plus que
  // `sort`, et la mutation qui les échange est équivalente -- c'est le
  // départage par le nom brut qui porte la garantie, pas le choix de
  // l'algorithme.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const DirEntry& a, const DirEntry& b) {
                     // `..` d'abord, quoi qu'il arrive : c'est la sortie.
                     const bool pa = a.name == kParent;
                     const bool pb = b.name == kParent;
                     if (pa != pb) return pa;
                     // Puis les dossiers, convention de tous les
                     // gestionnaires : mélanger les deux rend une
                     // arborescence profonde illisible.
                     const bool da = a.kind == EntryKind::Dir;
                     const bool db = b.kind == EntryKind::Dir;
                     if (da != db) return da;
                     const int c = compare_folded(a.name, b.name);
                     // ORDRE TOTAL. Sans ce départage, « README » et
                     // « readme » comparent égaux, et leur ordre dépend
                     // alors de l'algorithme de tri -- la liste sauterait
                     // sous les yeux de l'utilisateur sans que rien n'ait
                     // changé sur le disque. Départager par le nom BRUT
                     // rend l'ordre entièrement déterminé, ce qui rend du
                     // même coup la stabilité du tri sans objet.
                     if (c != 0) return c < 0;
                     return a.name < b.name;
                   });
}

std::vector<DirEntry> filter_entries(const std::vector<DirEntry>& entries,
                                     const std::string& needle) {
  // Raccourci : un besoin vide garde tout. La boucle ci-dessous en
  // ferait autant -- `find("")` rend 0, jamais npos -- donc la mutation
  // qui le retire est ÉQUIVALENTE. Il reste parce qu'il évite de plier la
  // casse de chaque nom pour rien, à chaque frappe.
  if (needle.empty()) return entries;

  std::string folded_needle;
  folded_needle.reserve(needle.size());
  for (char c : needle) folded_needle.push_back(fold(c));

  std::vector<DirEntry> out;
  out.reserve(entries.size());
  for (const DirEntry& e : entries) {
    // `..` survit TOUJOURS : c'est la sortie, pas un résultat de
    // recherche. Sans cela, un filtre qui ne trouve rien enferme dans le
    // répertoire.
    if (e.name == kParent) {
      out.push_back(e);
      continue;
    }
    std::string folded_name;
    folded_name.reserve(e.name.size());
    for (char c : e.name) folded_name.push_back(fold(c));
    if (folded_name.find(folded_needle) != std::string::npos) out.push_back(e);
  }
  return out;
}

}  // namespace sshos
