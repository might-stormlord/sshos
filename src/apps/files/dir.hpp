#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sshos {

// Ce qu'une entrée de répertoire a besoin d'être pour être affichée et
// triée. Rien de plus : ni permissions, ni dates, que la v1 n'affiche pas.
enum class EntryKind : uint8_t { Dir, File, Link, Other };

struct DirEntry {
  std::string name;
  EntryKind kind = EntryKind::File;
  uint64_t size = 0;
  bool operator==(const DirEntry&) const = default;
};

// Le contenu d'un répertoire, LU EN UNE PASSE.
//
// Aucune lecture n'a lieu pendant le rendu : le démon est mono-thread, et
// un `readdir()` sur un montage NFS mort gèlerait toutes les fenêtres et
// tous les clients. On lit à l'ouverture, on garde, et on filtre en
// mémoire.
struct DirListing {
  std::string path;
  std::vector<DirEntry> entries;
  // Vide en cas de succès. Un répertoire illisible rend une liste vide ET
  // un message : ne rien dire laisserait l'utilisateur devant un panneau
  // blanc sans savoir pourquoi.
  std::string error;
};

// Lit `path`. `show_hidden` inclut les noms commençant par un point.
//
// `..` est TOUJOURS en tête, sauf à la racine -- où il n'existe pas, et où
// l'offrir ferait tourner en rond.
DirListing read_dir(const std::string& path, bool show_hidden);

// Le tri : DOSSIERS D'ABORD, puis par nom, insensible à la casse mais
// STABLE. Deux noms qui ne diffèrent que par la casse doivent garder un
// ordre déterministe, sinon la liste saute d'un rafraîchissement à
// l'autre. `..` reste en tête quoi qu'il arrive.
void sort_entries(std::vector<DirEntry>& entries);

// Le filtre : une sous-chaîne, insensible à la casse. Il ne relit RIEN --
// filtrer est une opération sur ce qui est déjà en mémoire. `..` survit
// toujours au filtre : c'est la sortie, pas un résultat de recherche.
std::vector<DirEntry> filter_entries(const std::vector<DirEntry>& entries,
                                     const std::string& needle);

// Le chemin du parent, ou le même chemin à la racine.
std::string parent_path(const std::string& path);

// Assemble un chemin et un nom, sans jamais produire de double barre.
std::string join_path(const std::string& dir, const std::string& name);

}  // namespace sshos
