#include "common/outqueue.hpp"

#include <sys/socket.h>

#include <cerrno>

namespace sshos {

namespace {

// Seuil de capacité au-delà duquel un tampon totalement vidé -- que ce
// soit par un flush() qui l'a drainé en entier ou par un rejet dans
// push() -- rend sa mémoire au système plutôt que de la garder en
// réserve. Même idiome que Decoder::compact() (proto.cpp : voir
// kReleaseCapacityThreshold là-bas), mais une constante absolue plutôt
// qu'indexée sur ceiling_. ceiling_ est un paramètre par connexion, choisi
// par l'appelant -- minuscule dans un test, potentiellement très généreux
// en production pour tolérer un pair en rafale sans le sacrifier -- qui
// répond à une question différente (« à partir de quand renoncer à cette
// connexion ») de celle que ce seuil répond (« à partir de quand cette
// allocation est-elle clairement hors norme pour du trafic ordinaire »).
// Les coupler aurait lié la libération mémoire au moment où la connexion
// est de toute façon sur le point d'être abandonnée, ce qui rendrait ce
// garde-fou inopérant dans le cas courant qu'il vise : un pic ponctuel
// largement sous le plafond de rejet -- exactement l'exemple mesuré par la
// relecture (une rafale de 50 Mio, avec ceiling_ configuré bien au-dessus,
// qui restait pincée indéfiniment).
//
// Valeur choisie dans le même esprit que kMaxMessageBytes (proto.hpp),
// sans en dépendre (OutQueue transporte des octets, pas des trames -- pas
// de raison de la coupler au format du dessus) : les trames réelles
// (diffs de terminal) font quelques centaines d'octets à quelques
// kilooctets, et même une accumulation de plusieurs dizaines d'entre elles
// pendant qu'un pair traîne reste en pratique sous 8 Mio. Ce seuil n'est
// donc franchi que par un évènement réellement inhabituel -- soit une telle
// accumulation, soit une trame isolée proche du pire cas théorique (un
// repaint complet d'un écran extrême, ~24 Mio -- voir le commentaire de
// kMaxMessageBytes, proto.hpp) -- jamais par le trafic courant qu'il s'agit
// justement de ne pas pénaliser d'un cycle libère/réalloue à chaque
// vidage.
constexpr size_t kReleaseCapacityThreshold = 8ull * 1024 * 1024;

}  // namespace

// Vide buf_ en rendant sa capacité physique au système si elle a dépassé
// kReleaseCapacityThreshold, sinon un simple clear(). std::string{}.swap()
// plutôt que shrink_to_fit() -- non contraignant côté norme -- est
// l'idiome garanti pour rendre l'allocation : voir Decoder::compact()
// (proto.cpp), repris ici à l'identique. Utilisée à la fois par compact()
// (vidage complet par flush()) et par push() (rejet au-delà du plafond) :
// les deux chemins qui peuvent laisser buf_ vide doivent tous les deux
// pouvoir rendre sa capacité, pas seulement le premier.
void OutQueue::release_buffer() {
  if (buf_.capacity() >= kReleaseCapacityThreshold) {
    std::string{}.swap(buf_);
  } else {
    buf_.clear();
  }
  off_ = 0;
}

void OutQueue::push(std::string_view bytes) {
  // Vérifié AVANT d'ajouter, pas après : une poussée unique et énorme ne
  // doit pas gonfler buf_ bien au-delà du plafond avant d'être jetée --
  // l'état final est identique (tampon vide, drapeau posé), sans le pic
  // intermédiaire. `current` (taille logique non envoyée) est toujours
  // <= ceiling_ en entrée -- invariant maintenu par ce même contrôle à
  // chaque appel précédent -- donc `ceiling_ - current` ne peut jamais
  // déborder. Comparer par soustraction (`bytes.size() > ceiling_ -
  // current`) plutôt que par somme (`current + bytes.size() > ceiling_`)
  // évite tout risque de dépassement de size_t si bytes.size() était
  // énorme : aucune addition de deux tailles arbitraires n'a lieu.
  const size_t current = size();
  if (bytes.size() > ceiling_ - current) {
    // Capturé ICI, avant que release_buffer() ne remette off_ à zéro :
    // c'est le seul instant où cette information existe (voir le
    // commentaire de take_overflow(), outqueue.hpp, pour ce que Clean et
    // Dirty impliquent côté appelant, et pourquoi wants_write() ne peut
    // pas s'y substituer).
    const bool had_partial_send = off_ > 0;
    overflow_ = had_partial_send ? Overflow::Dirty : Overflow::Clean;
    release_buffer();
    return;
  }
  buf_.append(bytes);
}

bool OutQueue::flush(int fd) {
  while (off_ < buf_.size()) {
    const ssize_t n = ::send(fd, buf_.data() + off_, buf_.size() - off_,
                             MSG_NOSIGNAL);
    if (n > 0) {
      off_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  compact();
  return true;
}

OutQueue::Overflow OutQueue::take_overflow() {
  const Overflow v = overflow_;
  overflow_ = Overflow::None;
  return v;
}

void OutQueue::compact() {
  if (off_ == 0) return;
  if (off_ == buf_.size()) {
    // Cas ordinaire : un flush() réussi qui vide le tampon en entier, à
    // chaque appel réussi. release_buffer() ne libère la capacité
    // physique que si elle a réellement explosé (voir
    // kReleaseCapacityThreshold) -- pas de cycle libère/réalloue sur ce
    // chemin pour du trafic normal.
    release_buffer();
    return;
  }
  if (off_ > (1 << 16)) {
    buf_.erase(0, off_);
    off_ = 0;
  }
}

}  // namespace sshos
