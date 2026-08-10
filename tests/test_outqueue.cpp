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
// appliqué sur un écran qui n'est pas celui qu'il suppose.
TEST(outqueue_drops_the_whole_queue_past_the_ceiling) {
  OutQueue q(4096);
  q.push(std::string(5000, 'x'));
  CHECK(q.take_overflow());
  CHECK_EQ(q.size(), static_cast<size_t>(0));
  CHECK(!q.take_overflow());  // le drapeau se consomme
}

TEST(outqueue_reports_a_dead_peer) {
  Pair p;
  REQUIRE(make_pair(p));
  p.b.reset();  // le pair ferme
  OutQueue q(1 << 20);
  q.push(std::string(1 << 16, 'y'));
  CHECK(!q.flush(p.a.get()));
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
