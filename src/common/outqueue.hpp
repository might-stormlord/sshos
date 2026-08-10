#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace sshos {

// File de sortie non bloquante. Le démon n'écrit JAMAIS en bloquant : un
// client sur une liaison lente gèlerait tout le bureau, y compris les
// processus des autres fenêtres.
class OutQueue {
 public:
  // Résultat de take_overflow() : distingue un débordement qui n'a rien
  // envoyé sur le fil (Clean) d'un débordement survenu après qu'un flush()
  // avait déjà émis une partie de la trame jetée (Dirty) -- voir le
  // commentaire de take_overflow() ci-dessous pour ce que chaque valeur
  // implique côté appelant.
  enum class Overflow { None, Clean, Dirty };

  // GARDE-FOU STRUCTUREL, pas seulement documentaire : l'ordre de
  // déclaration ci-dessus doit rester None < Clean < Dirty. La fusion
  // monotone de la sévérité, dans OutQueue::push() (src/common/outqueue.cpp)
  // -- `if (this_rejection > overflow_) overflow_ = this_rejection;` -- ne
  // calcule le pire des deux rejets que PARCE QUE les valeurs sous-jacentes
  // de l'enum croissent dans cet ordre précis ; un simple commentaire à cet
  // endroit s'est déjà montré insuffisant (voir la relecture de ce round,
  // qui a réordonné l'enum en { None, Dirty, Clean } et compilé sans le
  // moindre avertissement sous -Wall -Wextra -Wpedantic -Werror). Si cette
  // assertion casse : soit tu as réordonné/inséré/retiré une valeur par
  // erreur et il faut restaurer None, Clean, Dirty, soit c'est volontaire et
  // il faut alors changer la fusion de push() pour ne plus dépendre de
  // l'ordre de déclaration (par ex. une fonction de sévérité explicite au
  // lieu de l'opérateur `>` intégré).
  static_assert(
      static_cast<std::underlying_type_t<Overflow>>(Overflow::None) <
              static_cast<std::underlying_type_t<Overflow>>(Overflow::Clean) &&
          static_cast<std::underlying_type_t<Overflow>>(Overflow::Clean) <
              static_cast<std::underlying_type_t<Overflow>>(Overflow::Dirty),
      "OutQueue::Overflow doit rester déclaré dans l'ordre None < Clean < "
      "Dirty : OutQueue::push() (src/common/outqueue.cpp) fusionne la "
      "sévérité accumulée avec `if (this_rejection > overflow_) overflow_ = "
      "this_rejection;`, une comparaison directe des valeurs de l'enum qui "
      "n'est correcte que si l'ordre de déclaration encode la sévérité "
      "croissante.");

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

  // Rend l'état de débordement accumulé depuis le dernier appel, puis le
  // remet à None (drapeau qui se consomme). Un dépassement du plafond jette
  // TOUJOURS tout le tampon (voir push()) : l'appelant doit repartir sur un
  // repaint complet dans les deux cas. Mais un repaint seul ne suffit pas
  // toujours à remettre le pair d'aplomb -- deux cas, distingués par la
  // valeur rendue :
  //
  //  - Clean : rien de ce qui vient d'être jeté n'était encore parti sur le
  //    fil (aucun flush() n'avait entamé ce tampon depuis le dernier push()
  //    qui a débordé). Le pair est aligné sur les frontières de trame ; un
  //    repaint complet le répare proprement.
  //
  //  - Dirty : un flush() antérieur avait déjà émis une partie du tampon
  //    quand le push() suivant a dépassé le plafond (push() jette tout, y
  //    compris ce qui restait à envoyer d'un flush() précédent). Le pair a
  //    donc déjà reçu un préfixe de trame binaire préfixée par sa longueur.
  //    Ce n'est PAS un problème d'affichage : OutQueue ne transporte pas de
  //    l'ANSI brut vers un terminal, mais des trames que Decoder consomme
  //    AVANT toute interprétation (voir proto.hpp). Le Decoder du pair
  //    attend alors les octets manquants d'un message fantôme et ne produira
  //    plus jamais rien, sans jamais passer en échec (failed()) -- un gel
  //    silencieux et définitif, pas un écran abîmé. Aucun repaint, si
  //    complet soit-il, ne le débloque : la vraie correction (fermer la
  //    connexion, ou émettre un préfixe de resynchronisation que le pair
  //    sache reconnaître) revient à l'appelant, pas à cette classe -- voir
  //    la tâche 13.
  //
  // Imprécision assumée, documentée plutôt que prétendue résolue : Dirty
  // signifie « une émission partielle a eu lieu avant ce rejet », pas
  // « une trame précise a été coupée en deux ». off_ pourrait tomber pile
  // sur une frontière de trame, auquel cas le pair serait en réalité aligné
  // et ce code déclarerait Dirty à tort (faux positif). OutQueue ignore
  // délibérément où sont les frontières de trame -- ce n'est pas son rôle.
  // Le sur-signalement (Dirty pour un cas qui était en fait propre) est le
  // bon côté où se tromper : au pire l'appelant est trop prudent (il ferme
  // une connexion en réalité récupérable), jamais l'inverse (déclarer Clean
  // un cas réellement sale, qui gèlerait le pair en silence).
  //
  // Ne PAS reconstituer cette distinction à partir de wants_write() lu
  // avant chaque push() : wants_write() (off_ < buf_.size()) est vrai dès
  // qu'il reste des octets en attente, y compris quand le tout premier
  // send() d'un flush() a pris EAGAIN sans avoir rien émis -- off_ vaut
  // alors 0, le pair n'a rien reçu de partiel, et le débordement serait
  // Clean. Cette implication ne vaut que dans un sens (off_ != 0 implique
  // wants_write(), jamais l'inverse) : l'utiliser en substitut classerait à
  // tort ce cas en sale et ferait tuer des connexions parfaitement
  // récupérables. L'information exacte n'existe qu'à un seul instant, dans
  // push() au moment du rejet, juste avant que off_ ne soit remis à zéro --
  // c'est là, et seulement là, qu'elle est capturée.
  //
  // État accumulé, PAS état du dernier rejet : entre deux appels à
  // take_overflow(), plusieurs push() peuvent déborder. La sévérité
  // rendue est celle du PIRE rejet survenu depuis le dernier appel, jamais
  // moins -- un rejet Clean qui suit un rejet Dirty ne dégrade pas l'état
  // en attente (release_buffer() remet off_ à zéro à chaque rejet, donc un
  // second rejet isolé serait toujours vu comme Clean s'il écrasait plutôt
  // que fusionnait). Ne redescend que quand take_overflow() le consomme et
  // le remet à None -- même discipline que Decoder::failed() (tâche 7),
  // qui ne se répare pas non plus tout seul. Défaut Critical de ce round :
  // corrigé après avoir été trouvé par sondage sur le code fusionné, voir
  // le commentaire de push().
  Overflow take_overflow();

  // Diagnostic réservé aux tests : capacité physique actuelle du tampon
  // interne (buf_.capacity()), pour vérifier que la mémoire est bien rendue
  // après un pic qui l'a fait grossir -- que ce pic ait été suivi d'un
  // vidage complet (flush()) ou d'un rejet (push() au-delà du plafond).
  // N'existe que pour ça -- aucun code de production ne doit lire cette
  // valeur.
  size_t buffer_capacity_for_tests() const { return buf_.capacity(); }

  // Diagnostic réservé aux tests : longueur physique du tampon interne
  // (buf_.size()), acomptes déjà envoyés compris. Sert à vérifier que la
  // branche de décalage de compact() (off_ au-delà d'un flush() partiel
  // important) a bien réclamé le préfixe mort plutôt que de le laisser
  // traîner : une fois cette branche exécutée, cette valeur redevient
  // égale à size() (plus aucun octet déjà envoyé en tête). N'existe que
  // pour ça -- aucun code de production ne doit lire cette valeur.
  size_t raw_buffer_size_for_tests() const { return buf_.size(); }

 private:
  void compact();
  void release_buffer();

  std::string buf_;
  size_t off_ = 0;
  size_t ceiling_;
  Overflow overflow_ = Overflow::None;
};

}  // namespace sshos
