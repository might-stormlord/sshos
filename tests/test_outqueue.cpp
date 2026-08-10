#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

#include "common/fd.hpp"
#include "common/frameclock.hpp"
#include "common/outqueue.hpp"
#include "harness.hpp"

using sshos::FrameClock;
using sshos::OutQueue;

namespace {

struct Pair {
  sshos::Fd a;
  sshos::Fd b;
};

// Fabrique une paire de sockets AF_UNIX connectés, avec `a` en non bloquant.
// Rend faux si socketpair() échoue, plutôt que d'ignorer son retour comme le
// helper du plan : sans ce contrôle, `sv` reste {-1, -1} et le test entier
// tournerait sur le descripteur -1 -- chaque send() échouerait avec EBADF,
// flush() rendrait false, et outqueue_reports_a_dead_peer passerait pour la
// mauvaise raison (EBADF, pas la fermeture du pair). L'appelant, dans chaque
// TEST(...), doit arrêter le cas avec REQUIRE(make_pair(p)) : cette fonction
// ne peut pas porter elle-même ce REQUIRE, puisqu'elle rend une valeur
// (Pair&, en sortie) et que le `return;` nu de la macro ne compile que dans
// une fonction void (voir tests/harness.hpp).
bool make_pair(Pair& out) {
  int sv[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  out.a = sshos::Fd(sv[0]);
  out.b = sshos::Fd(sv[1]);
  // set_nonblock() lève std::system_error en cas d'échec (voir
  // src/common/fd.cpp) : tests/main.cpp rattrape toute exception qui
  // s'échappe d'un TEST(...) et marque le cas en échec sans faire tomber le
  // reste de la suite, donc pas besoin de la convertir ici en bool.
  sshos::set_nonblock(out.a.get());
  return true;
}

}  // namespace

TEST(outqueue_drains_completely_when_the_socket_accepts) {
  Pair p;
  REQUIRE(make_pair(p));
  OutQueue q(1 << 20);
  q.push("hello");
  CHECK(q.flush(p.a.get()));
  CHECK(!q.wants_write());
  CHECK_EQ(q.size(), static_cast<size_t>(0));

  char buf[16] = {};
  CHECK_EQ(::read(p.b.get(), buf, sizeof buf), static_cast<ssize_t>(5));
  CHECK_EQ(std::string(buf, 5), std::string("hello"));
}

// Personne ne lit : le tampon noyau se remplit, EAGAIN arrive, et le reste
// doit rester en file -- pas d'écriture bloquante, jamais.
//
// Rendu déterministe : le plan poussait 4 Mio en espérant que le tampon par
// défaut du socketpair sature, ce qui dépend de net.core.wmem_default, un
// réglage de la machine qui exécute la suite -- instable par construction.
// Ici, SO_SNDBUF/SO_RCVBUF sont fixés explicitement à une petite valeur
// connue sur les deux extrémités avant toute écriture, puis la quantité
// poussée est dimensionnée à partir de ce qui est *effectivement* obtenu
// (lu via getsockopt), pas de ce qui a été demandé : le noyau Linux double
// la valeur demandée et impose un plancher (SOCK_MIN_SNDBUF), donc supposer
// la valeur demandée serait retomber dans le même piège. Mesuré
// empiriquement sur la machine de développement : demander 4096 rend un
// SO_SNDBUF/SO_RCVBUF effectif de 8192, pour une capacité réellement
// inscriptible avant EAGAIN d'environ 1792 octets (l'écart vient de
// l'overhead par sk_buff du noyau, qui ne fait pas partie du contrat de
// SO_SNDBUF). Pousser 8 fois la valeur *effective* de SO_SNDBUF laisse une
// marge large sur ce nombre sans dépendre d'un détail d'implémentation
// aussi fin, tout en restant assez petit pour rester rapide sous ASan
// (contrairement aux 4 Mio du plan).
TEST(outqueue_keeps_the_remainder_and_asks_for_epollout) {
  Pair p;
  REQUIRE(make_pair(p));

  constexpr int kRequestedBuf = 4096;
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);

  int actual_sndbuf = 0;
  socklen_t len = sizeof actual_sndbuf;
  REQUIRE_EQ(
      ::getsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &actual_sndbuf, &len), 0);
  REQUIRE(actual_sndbuf > 0);

  const size_t push_size = static_cast<size_t>(actual_sndbuf) * 8;
  // Plafond largement au-dessus de la poussée : ce test vise la saturation
  // du tampon noyau, pas le plafond de la file elle-même (couvert par
  // outqueue_drops_the_whole_queue_past_the_ceiling).
  OutQueue q(push_size * 4);
  q.push(std::string(push_size, 'x'));
  CHECK(q.flush(p.a.get()));  // EAGAIN n'est pas une erreur
  CHECK(q.wants_write());
  CHECK(q.size() > 0);
}

// Au-delà du plafond on jette TOUT : garder un préfixe produirait un diff
// appliqué sur un écran qui n'est pas celui qu'il suppose. Aucun flush()
// n'a eu lieu avant ce push() : rien n'était parti sur le fil, le
// débordement doit donc se signaler Clean, pas seulement "vrai" -- une
// simple assertion de vérité laisserait passer un code qui rendrait Dirty
// à tort dans ce cas.
TEST(outqueue_drops_the_whole_queue_past_the_ceiling) {
  OutQueue q(4096);
  q.push(std::string(5000, 'x'));
  CHECK(q.take_overflow() == OutQueue::Overflow::Clean);
  CHECK_EQ(q.size(), static_cast<size_t>(0));
  CHECK(q.take_overflow() == OutQueue::Overflow::None);  // le drapeau se consomme
}

// Trou de mutation démontré par la relecture : `size() > ceiling_` (devenu
// `bytes.size() > ceiling_ - current` après la correction) muté en `>=` ne
// fait échouer aucun test existant, parce qu'aucun ne pousse EXACTEMENT au
// plafond. Une poussée qui atterrit pile dessus doit être acceptée --
// c'est la frontière légale, pas un débordement.
TEST(outqueue_accepts_a_push_that_lands_exactly_on_the_ceiling) {
  OutQueue q(4096);
  q.push(std::string(4096, 'z'));
  CHECK_EQ(q.size(), static_cast<size_t>(4096));
  CHECK(q.take_overflow() == OutQueue::Overflow::None);
}

// Débordement "sale" : un flush() antérieur a déjà émis une partie du
// tampon (off_ > 0) quand un push() suivant dépasse le plafond et jette
// tout. Le pair a alors déjà reçu un préfixe de trame binaire ; voir le
// commentaire de take_overflow() (outqueue.hpp) pour pourquoi ce n'est pas
// récupérable par un simple repaint. Discriminant vis-à-vis du cas propre
// ci-dessus : la seule différence de mise en scène est qu'un flush()
// partiel a réellement eu lieu avant le rejet.
TEST(outqueue_overflow_after_a_partial_send_is_reported_dirty) {
  Pair p;
  REQUIRE(make_pair(p));

  constexpr int kRequestedBuf = 4096;
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);

  int actual_sndbuf = 0;
  socklen_t len = sizeof actual_sndbuf;
  REQUIRE_EQ(
      ::getsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &actual_sndbuf, &len), 0);
  REQUIRE(actual_sndbuf > 0);

  // Une première poussée assez grande pour saturer le tampon noyau (même
  // dimensionnement mesuré que outqueue_keeps_the_remainder_and_asks_for_epollout) :
  // flush() en envoie une partie puis prend EAGAIN, laissant off_ > 0 --
  // le pair a déjà reçu un préfixe.
  const size_t first_push = static_cast<size_t>(actual_sndbuf) * 8;
  OutQueue q(first_push * 4);  // plafond large : cette poussée ne déborde pas
  q.push(std::string(first_push, 'a'));
  CHECK(q.flush(p.a.get()));
  CHECK(q.wants_write());
  // Preuve directe qu'un envoi partiel a bien eu lieu (pas seulement que
  // wants_write() est vrai -- la relecture a montré que wants_write() seul
  // ne le prouve pas, voir le commentaire de take_overflow()) : moins de
  // first_push octets restent en file, donc off_ > 0.
  CHECK(q.size() < first_push);

  // Une seconde poussée qui dépasse à elle seule le plafond : rejetée dans
  // son intégralité, alors qu'un flush() a déjà émis une partie du tampon.
  q.push(std::string(first_push * 5, 'b'));
  CHECK(q.take_overflow() == OutQueue::Overflow::Dirty);
  CHECK_EQ(q.size(), static_cast<size_t>(0));
}

TEST(outqueue_reports_a_dead_peer) {
  Pair p;
  REQUIRE(make_pair(p));
  p.b.reset();  // le pair ferme
  OutQueue q(1 << 20);
  q.push(std::string(1 << 16, 'y'));
  CHECK(!q.flush(p.a.get()));
}

// Défaut Critical de la relecture : compact() n'avait aucune branche de
// libération, y compris quand le tampon est pleinement vidé --
// std::string::clear() ne rend jamais la capacité. Une seule rafale
// pinçait buf_.capacity() pour toute la durée de vie de la connexion,
// quel que soit le trafic ultérieur. Ce test construit exactement ce
// scénario -- une rafale largement au-delà de kReleaseCapacityThreshold
// (voir outqueue.cpp), entièrement drainée par le pair -- et vérifie que
// la capacité redescend, pas seulement que size() est repassé à zéro (un
// simple clear() aurait laissé buffer_capacity_for_tests() == la
// capacité gonflée, seul size() aurait changé -- exactement le
// comportement de l'ancien code). Compilé contre l'état d'avant cette
// révision, ce test échoue (drained_capacity == grown_capacity) ; c'est
// la preuve recherchée.
TEST(outqueue_releases_capacity_once_fully_drained_past_threshold) {
  Pair p;
  REQUIRE(make_pair(p));

  constexpr int kRequestedBuf = 4096;
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);

  // Largement au-dessus de kReleaseCapacityThreshold (8 Mio), largement en
  // dessous du plafond de la file pour ne pas se faire jeter en route.
  constexpr size_t kBurst = 12ull * 1024 * 1024;
  OutQueue q(kBurst * 2);
  q.push(std::string(kBurst, 'w'));

  const size_t grown_capacity = q.buffer_capacity_for_tests();
  CHECK(grown_capacity >= kBurst);

  // Draine entièrement : flush() + lecture côté pair jusqu'à ce qu'il ne
  // reste plus rien à envoyer.
  char drain[65536];
  while (q.wants_write()) {
    REQUIRE(q.flush(p.a.get()));
    const ssize_t n = ::read(p.b.get(), drain, sizeof drain);
    REQUIRE(n > 0);
  }
  CHECK_EQ(q.size(), static_cast<size_t>(0));

  const size_t drained_capacity = q.buffer_capacity_for_tests();
  // La preuve recherchée : la capacité est redescendue à quelque chose de
  // proche de zéro, pas simplement en dessous de grown_capacity.
  CHECK(drained_capacity < grown_capacity);
  CHECK(drained_capacity < 1024u);
}

// Même défaut Critical, chemin du rejet : push() au-delà du plafond faisait
// buf_.clear() directement, sans jamais passer par compact() -- et
// compact() n'est appelé qu'en fin de flush(). Après un rejet, buf_ est
// vide donc wants_write() est faux : le démon ne rappellerait plus jamais
// flush(), donc compact() ne tournerait jamais, et la capacité resterait
// pincée indéfiniment -- le pire cas, puisque c'est juste après le pic
// que la mémoire est perdue pour de bon. push() doit donc appliquer la
// même logique de libération que compact(), sans dépendre d'un flush()
// qui ne viendra pas.
TEST(outqueue_releases_capacity_on_the_rejection_path_too) {
  // D'abord, une poussée légitime et énorme pour gonfler la capacité sans
  // jamais déborder (plafond très généreux).
  constexpr size_t kBurst = 12ull * 1024 * 1024;
  OutQueue q(kBurst * 2);
  q.push(std::string(kBurst, 'w'));
  const size_t grown_capacity = q.buffer_capacity_for_tests();
  CHECK(grown_capacity >= kBurst);

  // Puis un rejet : sans jamais appeler flush(). Si la libération ne
  // passe que par compact(), ce chemin ne peut pas la déclencher.
  q.push(std::string(kBurst * 3, 'x'));
  CHECK(q.take_overflow() == OutQueue::Overflow::Clean);
  CHECK_EQ(q.size(), static_cast<size_t>(0));

  const size_t released_capacity = q.buffer_capacity_for_tests();
  CHECK(released_capacity < grown_capacity);
  CHECK(released_capacity < 1024u);
}

// Trou de mutation démontré par la relecture : la branche de décalage de
// compact() (off_ > 1<<16) est morte du point de vue de la suite -- rien
// ne casse si on la supprime. Elle ne se déclenche que lorsqu'un flush() a
// cumulé plus de 64 Kio d'envois partiels SANS vider le tampon en entier
// (un pair lent mais vivant, qui draine moins vite que le démon ne
// pousse). Sans elle, le préfixe déjà envoyé resterait indéfiniment en
// tête de buf_ -- jamais réclamé tant que le tampon n'est pas totalement
// vide -- et la mémoire physique retenue grossirait sans borne sur une
// connexion longue avec ce profil de trafic. raw_buffer_size_for_tests()
// le prouve directement : une fois le décalage déclenché, la longueur
// physique du tampon redevient exactement sa longueur logique (plus aucun
// octet déjà envoyé ne traîne en tête) ; sans la branche, elle resterait
// supérieure à size() de tout ce préfixe mort.
TEST(outqueue_compact_shifts_away_a_large_sent_prefix_before_full_drain) {
  Pair p;
  REQUIRE(make_pair(p));

  constexpr int kRequestedBuf = 4096;
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.a.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_SNDBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);
  REQUIRE_EQ(::setsockopt(p.b.get(), SOL_SOCKET, SO_RCVBUF, &kRequestedBuf,
                           sizeof kRequestedBuf),
             0);

  // Un seul gros push, largement sous le plafond : tout le mouvement
  // observé vient de plusieurs flush() partiels successifs, pas de
  // pushes répétés.
  constexpr size_t kTotal = 200000;
  OutQueue q(kTotal * 4);
  q.push(std::string(kTotal, 'z'));

  // Drain par petits bouts (lecture côté pair entre deux flush()), en
  // observant après chaque flush() le nombre d'octets déjà envoyés mais
  // pas encore réclamés -- raw_buffer_size_for_tests() - size(), le
  // "préfixe mort" -- plutôt que d'exiger qu'il soit nul à un instant fixé
  // arbitrairement : compact() tourne à la fin de CHAQUE flush() (voir
  // flush()), donc dès qu'un appel fait franchir 1<<16 au préfixe mort, ce
  // même appel le décale avant de rendre la main -- une valeur > 1<<16
  // n'est donc jamais observable de l'extérieur, seule la chute brutale
  // qui la suit l'est. C'est cette chute qu'on cherche : le préfixe mort
  // doit croître au fil des flush() partiels puis retomber d'un coup à 0.
  char drain[4096];
  size_t prev_dead = 0;
  bool shift_observed = false;
  for (int round = 0; round < 500 && q.wants_write(); ++round) {
    REQUIRE(q.flush(p.a.get()));
    const size_t dead = q.raw_buffer_size_for_tests() - q.size();
    // q.wants_write() est revérifié ICI, pas seulement en tête de boucle :
    // la chute de "dead" à 0 a DEUX causes possibles, et une seule est
    // celle qu'on cherche. (a) la branche de décalage a réclamé le préfixe
    // mort alors qu'il restait des octets non envoyés (wants_write() vrai)
    // -- c'est la preuve recherchée. (b) ce même flush() a aussi fini par
    // vider tout le tampon (off_ == buf_.size() sur la toute dernière
    // poignée d'octets) -- l'AUTRE branche de compact() (non mutée dans ce
    // test) réclame alors aussi buf_ en entier via release_buffer(),
    // produisant le même "dead == 0" sans que la branche de décalage ait
    // jamais tourné. Sans ce garde-fou, un vidage complet en fin de boucle
    // ferait passer ce test même avec la branche de décalage désactivée --
    // constaté empiriquement en mutant off_ > (1<<16) en faux : le test
    // restait vert car la boucle finissait par vider tout le tampon avant
    // sa borne de 500 tours.
    if (prev_dead > (1u << 15) && dead == 0 && q.wants_write()) {
      shift_observed = true;
      break;
    }
    prev_dead = dead;
    const ssize_t n = ::read(p.b.get(), drain, sizeof drain);
    REQUIRE(n > 0);
  }

  // La preuve recherchée : la chute a bien été observée alors qu'il restait
  // des octets à envoyer -- sans la branche de décalage, le préfixe mort ne
  // ferait que croître jusqu'au vidage complet final.
  CHECK(shift_observed);
}

TEST(frameclock_renders_immediately_then_throttles) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  CHECK_EQ(fc.delay_ms(t0), -1);  // rien à faire
  fc.mark_dirty();
  CHECK_EQ(fc.delay_ms(t0), 0);   // premier rendu : tout de suite
  fc.note_render(t0);
  CHECK_EQ(fc.delay_ms(t0), -1);

  fc.mark_dirty();
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(10)), 23);
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(40)), 0);
}

// dirty() n'était appelé par aucun test du plan : il reflète directement le
// drapeau interne, sans lien avec le plafond de cadence.
TEST(frameclock_dirty_reflects_pending_state) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  CHECK(!fc.dirty());
  fc.mark_dirty();
  CHECK(fc.dirty());
  fc.note_render(t0);
  CHECK(!fc.dirty());
}

// note_render() sans mark_dirty() préalable : cas plausible si l'appelant
// compose une frame pour une autre raison que le drapeau sale (ex: premier
// rendu forcé). dirty_ était déjà faux, note_render() le laisse faux et met
// seulement à jour last_ -- un simple horodatage, sans effet observable sur
// delay_ms() tant que personne n'appelle mark_dirty() ensuite.
TEST(frameclock_note_render_without_dirty_is_a_harmless_timestamp_update) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  CHECK(!fc.dirty());
  fc.note_render(t0);
  CHECK(!fc.dirty());
  CHECK_EQ(fc.delay_ms(t0), -1);
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(100)), -1);
}

// mark_dirty() appelé deux fois de suite : dirty_ est un simple booléen, pas
// un compteur, donc un seul rendu suffit à l'éteindre -- pas besoin de deux
// note_render() pour deux mark_dirty().
TEST(frameclock_mark_dirty_twice_only_needs_one_render) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  fc.mark_dirty();
  fc.mark_dirty();
  CHECK(fc.dirty());
  CHECK_EQ(fc.delay_ms(t0), 0);  // premier rendu : tout de suite
  fc.note_render(t0);
  CHECK(!fc.dirty());
  CHECK_EQ(fc.delay_ms(t0), -1);  // le second mark_dirty() n'a pas laissé de dette
}

// Trou de mutation démontré par la relecture : `elapsed >= min_interval_`
// muté en `elapsed > min_interval_` ne fait échouer aucun test existant,
// parce qu'aucun n'utilise exactement l'intervalle (33 ms) -- ce test
// verrouille ce cas précis : à elapsed == min_interval_ pile, le rendu
// doit être immédiat (0), pas une attente résiduelle.
//
// Nuance vérifiée empiriquement en écrivant ce correctif : à ce point
// précis, cette mutation est en réalité un mutant ÉQUIVALENT du point de
// vue de la seule valeur de retour. `min_interval_ - elapsed` vaut
// exactement 0 quand elapsed == min_interval_, que le retour vienne du
// premier `return 0` (branche prise avec `>=`) ou du calcul de la seconde
// ligne (branche prise avec `>`, mais 0 - 0 = 0 tout de même) : les deux
// versions rendent 0 sur CET appel précis, et aucune assertion possible
// sur le seul retour de delay_ms() ne peut donc discriminer cette mutation
// à ce point -- vérifié en rejouant la mutation contre ce test même après
// son ajout (voir le rapport de cette révision). Le test reste gardé :
// il verrouille le comportement documenté à la frontière contre d'autres
// régressions (ex. un décalage d'une unité sur min_interval_ lui-même),
// même s'il ne tue pas ce mutant précis.
TEST(frameclock_delay_ms_at_exactly_min_interval_composes_now) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  fc.mark_dirty();
  fc.note_render(t0);
  fc.mark_dirty();
  CHECK_EQ(fc.delay_ms(t0 + std::chrono::milliseconds(33)), 0);
}

// Cas réel si l'appelant passe un horodatage mis en cache antérieur au
// dernier rendu enregistré (ex: `now` capturé avant l'appel à note_render()
// d'une itération précédente de la boucle, puis réutilisé). `elapsed`
// devient négatif -- une durée signée, pas de comportement indéfini -- donc
// `elapsed >= min_interval_` est faux et la fonction rend une valeur
// *supérieure* à min_interval_ : ici 133 ms pour un intervalle de 33 ms et
// un `now` 100 ms avant `last_` (33 - (-100) = 133). Ce comportement n'est
// ni corrigé ni manifestement voulu ici : il est simplement ce que fait le
// code tel qu'il est écrit, figé par ce test pour qu'un futur changement ne
// le modifie pas sans qu'un test le remarque. Un appelant qui arme un timer
// sur ce résultat attendrait donc plus longtemps que min_interval_ avant de
// composer -- dégradé, pas incorrect au sens de bloquer indéfiniment, mais
// une surprise à signaler plutôt qu'à corriger en silence.
TEST(frameclock_now_before_last_yields_a_delay_above_min_interval) {
  using Clock = FrameClock::Clock;
  const auto t0 = Clock::now();
  FrameClock fc(std::chrono::milliseconds(33));

  fc.mark_dirty();
  fc.note_render(t0);
  fc.mark_dirty();
  const auto stale_now = t0 - std::chrono::milliseconds(100);
  CHECK_EQ(fc.delay_ms(stale_now), 133);
}
