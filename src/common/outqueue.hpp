#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sshos {

// File de sortie non bloquante. Le démon n'écrit JAMAIS en bloquant : un
// client sur une liaison lente gèlerait tout le bureau, y compris les
// processus des autres fenêtres.
class OutQueue {
 public:
  explicit OutQueue(size_t ceiling) : ceiling_(ceiling) {}

  void push(std::string_view bytes);

  // Rend false quand cette liaison n'est plus utilisable et que l'appelant
  // doit y renoncer -- pas seulement quand le pair est mort. EAGAIN/EWOULDBLOCK
  // (le tampon noyau est plein, pas une erreur) et EINTR (à refaire) sont
  // absorbés en interne et ne comptent pas. Tout le reste fait rendre false :
  // EPIPE/ECONNRESET (le pair a effectivement disparu), mais aussi EBADF (bogue
  // de l'appelant, descripteur déjà fermé), ENOBUFS, EFAULT... Le code
  // appelant ne peut donc pas déduire de `false` seul que le pair est parti ;
  // errno reste tel que send() l'a laissé au moment de l'échec (aucun appel
  // intermédiaire ne le touche avant ce return) pour qui veut distinguer ces
  // cas.
  bool flush(int fd);

  // Vrai tant qu'il reste des octets : EPOLLOUT doit être ARMÉ. Le
  // désarmer dès que la file est vide, sinon un epoll niveau-déclenché
  // tourne à 100 % de CPU sur un socket en permanence inscriptible.
  bool wants_write() const { return off_ < buf_.size(); }

  size_t size() const { return buf_.size() - off_; }

  // Vrai une seule fois après un dépassement de plafond : l'appelant doit
  // alors repartir sur un repaint complet, mais ce repaint ne suffit pas
  // toujours à remettre le pair d'aplomb. Deux cas, à distinguer par
  // l'appelant :
  //  - rien n'était encore parti pour cette frame (aucun flush() partiel
  //    depuis le dernier push() qui a débordé) : le pair n'a rien reçu de la
  //    séquence jetée, un repaint complet la remplace proprement ;
  //  - un flush() antérieur avait déjà écrit une partie du tampon quand le
  //    push() suivant a dépassé le plafond (push() vide tout, y compris ce
  //    qui restait à envoyer d'un flush() précédent) : le pair a alors déjà
  //    reçu un préfixe -- potentiellement une séquence d'échappement coupée
  //    en deux. Le repaint qui suit ne resynchronise pas ce flux : il
  //    s'ajoute après un fragment inachevé que le terminal du pair
  //    interprétera de travers. La vraie correction (émettre un préfixe de
  //    resynchronisation avant le repaint) revient à l'appelant, pas à cette
  //    classe. Cette interface ne donne toutefois pas à l'appelant de quoi
  //    distinguer les deux cas : take_overflow() rend un simple bool, sans
  //    dire si le tampon jeté avait déjà un préfixe parti.
  bool take_overflow();

 private:
  void compact();

  std::string buf_;
  size_t off_ = 0;
  size_t ceiling_;
  bool overflowed_ = false;
};

}  // namespace sshos
