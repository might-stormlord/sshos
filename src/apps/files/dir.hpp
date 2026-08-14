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
  // La date de dernière modification, en secondes depuis l'époque. Lue
  // DANS LA MÊME PASSE que la taille : y revenir au moment d'afficher
  // coûterait un `stat()` par entrée pendant le rendu -- l'endroit précis
  // où ce projet s'interdit de toucher au disque.
  uint64_t mtime = 0;
  bool operator==(const DirEntry&) const = default;
};

// Le critère de tri. Les dossiers passent devant sous tous les trois : c'est
// la convention de tous les gestionnaires, et un tri par taille qui mêlerait
// les deux rendrait une arborescence profonde illisible.
enum class SortBy : uint8_t { Name, Size, Time };

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

// Le tri. DOSSIERS D'ABORD, `..` toujours en tête -- l'inversion ne le
// renvoie PAS en bas, il est la sortie et non un résultat de tri.
//
// L'ordre est TOTAL sous tous les critères : deux fichiers de même taille,
// ou de même date -- ce qui est courant après une copie -- sont départagés
// par leur nom, puis par leur nom brut. Sans cela la liste sauterait d'un
// rafraîchissement à l'autre sans que rien n'ait changé sur le disque.
void sort_entries(std::vector<DirEntry>& entries, SortBy by = SortBy::Name,
                  bool descending = false);

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
