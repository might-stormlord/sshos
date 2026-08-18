#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common/platform.hpp"

namespace sshos {

// LE JOURNAL DU DEMON : quelques lignes par vie de processus, pas une
// trace d'execution.
//
// Il existe pour repondre a UNE question, posee pour de vrai le 18 aout
// 2026 apres la perte d'une session de travail : « pourquoi le bureau
// a-t-il disparu ? » Sans journal il n'y avait rien a lire -- pas de
// `dmesg` dans le conteneur, pas de fichier d'image memoire, et un demon
// muet. On en etait reduit aux hypotheses.
//
// CE QU'IL NE PEUT PAS DIRE, et c'est justement ce qui le rend utile : un
// processus tue par SIGKILL -- ce que fait le tueur de memoire du noyau --
// n'ecrit rien, par construction. Une vie qui commence par « demarrage » et
// ne se termine par AUCUNE ligne est donc la signature d'une mort brutale,
// par opposition a un arret demande. C'est la seule facon de distinguer les
// deux apres coup.
inline constexpr size_t kJournalMaxBytes = 64u * 1024u;

class Journal {
 public:
  // Un chemin VIDE desactive le journal sans rien casser : le demon doit
  // demarrer meme sans HOME.
  Journal(const Platform& plat, std::string path,
          size_t max_bytes = kJournalMaxBytes);

  // Horodate et ajoute une ligne. NE LEVE JAMAIS et n'echoue jamais
  // bruyamment : un disque plein ne doit pas emporter le bureau avec lui.
  void note(std::string_view evenement);

  const std::string& path() const { return path_; }

 private:
  const Platform* plat_;
  std::string path_;
  size_t max_bytes_;
};

// `<repertoire de donnees>/journal.log`, ou vide si on ne sait pas ou.
std::string daemon_journal_path();

// Installe de quoi ecrire une derniere ligne sur SIGSEGV, SIGBUS, SIGFPE et
// SIGABRT, puis relance le signal pour que le comportement par defaut
// s'applique quand meme. Sans ca, un plantage et une mort par manque de
// memoire laissent exactement la meme trace -- aucune.
//
// NE TOUCHE PAS a un signal deja pris en charge : sous ASan/UBSan, le
// gestionnaire installe par la bibliotheque produit un rapport bien plus
// riche que cette ligne, et l'ecraser serait une perte seche.
void arm_crash_note(const std::string& path);

}  // namespace sshos
