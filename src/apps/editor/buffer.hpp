#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sshos {

// Une position dans le tampon : ligne, puis colonne en OCTETS. En octets
// et non en caractères parce que toutes les éditions travaillent sur la
// chaîne ; la conversion vers des colonnes d'écran appartient au rendu.
struct TextPos {
  size_t line = 0;
  size_t col = 0;
  bool operator==(const TextPos&) const = default;
};

// LE TAMPON : un vecteur de lignes, pas un gap buffer. Pour les tailles de
// fichiers concernées, c'est plus simple et suffisant -- et un gap buffer
// mal écrit perd du texte, ce qu'un vecteur de lignes ne peut pas faire.
//
// INVARIANT : il y a TOUJOURS au moins une ligne. Un fichier vide vaut une
// ligne vide, pas zéro ligne -- sans quoi le curseur n'aurait nulle part
// où être et chaque accès demanderait sa garde.
class TextBuffer {
 public:
  TextBuffer();

  // Charge du texte. `\n` sépare ; un saut de ligne FINAL ne crée pas de
  // ligne vide de plus, et son absence est retenue pour que
  // l'enregistrement rende le fichier tel qu'il était.
  void load(const std::string& text);

  // Le texte à écrire. Rend exactement ce qui a été chargé tant que rien
  // n'a été modifié -- saut de ligne final compris, ou son absence.
  std::string text() const;

  size_t line_count() const { return lines_.size(); }
  const std::string& line(size_t i) const;
  bool modified() const { return modified_; }
  void mark_saved() { modified_ = false; }

  // Insère un caractère (déjà encodé en UTF-8) et rend la position d'après.
  TextPos insert(TextPos at, const std::string& s);
  // Coupe la ligne en deux.
  TextPos split_line(TextPos at);
  // Efface le caractère À GAUCHE. En début de ligne, fusionne avec la
  // précédente -- c'est ce que fait tout éditeur, et l'oublier laisse un
  // retour arrière sans effet une fois sur vingt.
  TextPos erase_before(TextPos at);
  // Efface le caractère SOUS le curseur. En fin de ligne, fusionne avec la
  // suivante.
  TextPos erase_at(TextPos at);

  // Cherche `needle` À PARTIR de `from`, en bouclant par le début. Rend
  // false si le texte ne s'y trouve nulle part.
  bool find(const std::string& needle, TextPos from, TextPos& out) const;

  // Ramène une position dans les bornes du tampon.
  TextPos clamp(TextPos p) const;

 private:
  std::vector<std::string> lines_;
  bool modified_ = false;
  // Le fichier chargé finissait-il par un saut de ligne ? En rajouter un
  // qui n'y était pas fait grossir le fichier d'un octet à chaque
  // enregistrement, et rend un diff bruyant.
  bool trailing_newline_ = true;
};

}  // namespace sshos
