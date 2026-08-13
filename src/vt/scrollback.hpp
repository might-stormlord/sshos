#pragma once

#include <cstddef>
#include <vector>

#include "vt/screen.hpp"

namespace sshos {

// La valeur de la spec (§ configuration : `scrollback = 10000`). Elle est
// un DÉFAUT, pas une constante : le tampon se construit avec la sienne.
inline constexpr size_t kDefaultScrollbackLines = 10000;

// Une ligne d'historique : ses cellules, ROGNÉES de leurs blancs de fin.
//
// La grille est un rectangle plein ; l'historique ne peut pas l'être. À
// 10 000 lignes de 200 colonnes, une cellule de 20 octets fait 40 Mo par
// terminal, dont l'immense majorité de blancs -- une ligne de shell en
// occupe quarante. D'où la longueur variable.
//
// Ce qui est rogné est le blanc PAR DÉFAUT, pas l'espace : une cellule
// vide mais peinte d'un fond reste du contenu visible, et une marge de
// couleur disparaîtrait en remontant dans l'historique.
using ScrollbackLine = std::vector<ScreenCell>;

// L'historique d'un terminal, et le point d'où on le consulte.
//
// Tampon CIRCULAIRE : au-delà de la capacité, la plus ancienne ligne s'en
// va, et son emplacement -- avec la mémoire déjà réservée par son
// vecteur de cellules -- sert à la nouvelle. Sur un terminal qui défile
// des heures, c'est la différence entre recycler dix mille vecteurs et en
// allouer un par ligne affichée.
class Scrollback {
 public:
  explicit Scrollback(size_t capacity = kDefaultScrollbackLines);

  size_t capacity() const { return capacity_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Range une ligne sortie par le haut. `count` est la largeur de la
  // grille ; ce qui est rangé est plus court.
  void push(const ScreenCell* cells, size_t count);

  // `index` 0 est la PLUS ANCIENNE encore là. Hors bornes rend une ligne
  // vide plutôt que de planter : l'appelant est un dessin, et un dessin
  // ne doit pas pouvoir tuer le démon.
  const ScrollbackLine& at(size_t index) const;

  void clear();

  // LE DÉCALAGE DE CONSULTATION, en lignes au-dessus du bas. 0 veut dire
  // « collé au présent » ; `size()` veut dire « tout en haut de ce qui
  // reste ». Il est borné aux deux bouts.
  //
  // Ce que voit l'appelant est donc la concaténation
  // `[historique…, écran vivant]` lue `offset()` lignes plus haut que sa
  // fin.
  size_t offset() const { return offset_; }
  void scroll_back(size_t n);     // vers le passé
  void scroll_forward(size_t n);  // vers le présent
  void scroll_to_bottom();

 private:
  size_t capacity_;
  std::vector<ScrollbackLine> ring_;
  size_t head_ = 0;  // l'emplacement de la plus ancienne
  size_t size_ = 0;
  size_t offset_ = 0;
  ScrollbackLine empty_{};
};

}  // namespace sshos
