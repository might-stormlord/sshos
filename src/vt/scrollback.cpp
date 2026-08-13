#include "vt/scrollback.hpp"

namespace sshos {

// Les emplacements sont réservés une fois pour toutes : ce sont EUX qu'on
// recycle. Un vecteur de ligne qui a déjà servi garde sa capacité, et la
// ligne suivante s'y écrit sans allouer -- sur un terminal qui défile des
// heures, c'est toute la différence.
Scrollback::Scrollback(size_t capacity) : capacity_(capacity) {
  ring_.resize(capacity_);
}

void Scrollback::push(const ScreenCell* cells, size_t count) {
  // Capacité nulle : l'historique est désactivé. Le test vient AVANT tout
  // le reste, sinon le modulo qui choisit l'emplacement divise par zéro.
  if (capacity_ == 0) return;

  // Le rognage. On s'arrête au dernier blanc PAR DÉFAUT : une cellule
  // vide mais peinte est du contenu visible.
  size_t len = count;
  const ScreenCell blank{};
  while (len > 0 && cells[len - 1] == blank) --len;

  size_t slot = 0;
  if (size_ < capacity_) {
    slot = (head_ + size_) % capacity_;
    ++size_;
  } else {
    // Plein : la plus ancienne s'en va, et c'est son emplacement -- avec
    // sa mémoire déjà réservée -- qui accueille la nouvelle.
    slot = head_;
    head_ = (head_ + 1) % capacity_;
  }
  ring_[slot].assign(cells, cells + len);

  // La consultation reste sur CE QU'ELLE MONTRE. Le décalage compte
  // depuis le bas : pour que la ligne regardée ne bouge pas sous les
  // yeux, il doit grandir d'autant que la sortie. Collé au présent (0),
  // il ne bouge pas -- c'est le cas normal, et on suit la sortie.
  if (offset_ > 0) {
    ++offset_;
    if (offset_ > size_) offset_ = size_;
  }
}

const ScrollbackLine& Scrollback::at(size_t index) const {
  if (index >= size_) return empty_;
  return ring_[(head_ + index) % capacity_];
}

void Scrollback::clear() {
  size_ = 0;
  offset_ = 0;
  // La TÊTE ne bouge pas, et n'a pas à bouger : tout se lit et s'écrit
  // relativement à elle. La remettre à zéro serait une seconde façon
  // d'être cohérent -- et la campagne de mutation l'a montré, une façon
  // qui rend l'autre indiscernable. Une seule suffit.
  //
  // Les emplacements restent eux aussi : ils ne portent plus rien de
  // lisible, et leur mémoire servira aux lignes suivantes.
}

void Scrollback::scroll_back(size_t n) {
  // Écrit comme une soustraction pour ne pas déborder : `offset_ + n`
  // avec un `n` proche du maximum ferait le tour et rendrait un décalage
  // minuscule au lieu du plafond.
  if (n > size_ - offset_) {
    offset_ = size_;
  } else {
    offset_ += n;
  }
}

void Scrollback::scroll_forward(size_t n) {
  if (n > offset_) {
    offset_ = 0;
  } else {
    offset_ -= n;
  }
}

void Scrollback::scroll_to_bottom() { offset_ = 0; }

}  // namespace sshos
