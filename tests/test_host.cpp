#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "fake_apps.hpp"
#include "fake_apps.hpp"
#include "common/fd.hpp"
#include "daemon/host.hpp"
#include "harness.hpp"

using sshos::App;
using sshos::Battement;
using sshos::Bloc;
using sshos::FdRegistrar;
using sshos::HostImpl;
using sshos::IoStatus;
using sshos::Window;

namespace {

struct FakeRegistrar : FdRegistrar {
  struct Call {
    uint64_t key = 0;
    int fd = -1;
    bool still_open = false;
  };
  std::vector<Call> watches;
  std::vector<Call> unwatches;

  void watch(uint64_t key, int fd, uint32_t) override {
    watches.push_back({key, fd, true});
  }
  void unwatch(int fd) override {
    // On relève ICI si le descripteur est encore ouvert : c'est la seule
    // façon d'observer l'ordre « unwatch avant close ». L'ordre inverse
    // laisserait une entrée epoll sur un numéro que le noyau peut
    // réattribuer à la milliseconde suivante.
    unwatches.push_back({0, fd, ::fcntl(fd, F_GETFD) >= 0});
  }
};

// La fenêtre porte son hôte, exactement comme en production. Un HostImpl
// local au test serait détruit AVANT la Window qui le référence -- l'ordre
// des destructions dans un bloc est l'inverse des déclarations -- et le
// destructeur de Battement appellerait unwatch() sur un hôte mort. C'est
// précisément ce que l'ordre des membres de Window (host avant app)
// garantit, et le test doit s'appuyer dessus plutôt que le contourner.
// Elle est aussi allouée sur le tas, comme dans WindowManager, et pour la
// même raison : `return w;` sur un objet nommé PEUT déplacer (la NRVO
// n'est pas garantie), et l'HostImpl resterait accroché à l'ancienne
// adresse.
//
// Le dernier paramètre est le drapeau de repeint : les cas qui ne le
// regardent pas partagent g_dirty, celui qui le regarde passe le sien.
bool g_dirty = false;

// La table des enfants surveillés appartient normalement à la Session ;
// les cas qui ne la regardent pas partagent celle-ci.
std::vector<sshos::ChildWatch> g_children;
std::vector<sshos::PendingApp> g_pending;

std::unique_ptr<Window> make_window(sshos::WindowId id, std::unique_ptr<App> app,
                                    FdRegistrar& reg, uint32_t& gen,
                                    bool& dirty = g_dirty,
                                    std::vector<sshos::ChildWatch>& children =
                                        g_children) {
  auto w = std::make_unique<Window>();
  w->id = id;
  w->app = std::move(app);
  w->host = std::make_unique<HostImpl>(*w, reg, gen, dirty, children, g_pending);
  return w;
}

HostImpl& host_of(Window& w) { return *static_cast<HostImpl*>(w.host.get()); }

// Application qui n'a AUCUNE défense : elle compte tout ce qu'on lui livre
// sans regarder le jeton. C'est délibéré -- Battement vérifie le sien, et
// cette prudence masquerait le filtrage que l'hôte doit faire de son côté.
struct Nosy : sshos::App {
  int calls = 0;
  uint64_t last = 0;
  void render(sshos::View) override {}
  IoStatus on_io(uint64_t token, uint32_t) override {
    ++calls;
    last = token;
    return IoStatus::Ok;
  }
};

// Le registrar du démon, reproduit à l'identique (daemon.cpp le garde dans
// son namespace anonyme). Il sert au seul test qui fasse réellement passer
// une clé par le noyau.
struct RealRegistrar : FdRegistrar {
  int ep = -1;
  uint64_t last_key = 0;
  void watch(uint64_t key, int fd, uint32_t events) override {
    last_key = key;
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = key;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
  }
  void unwatch(int fd) override { ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr); }
};

}  // namespace

TEST(host_hands_out_a_new_key_every_time_even_for_the_same_fd) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(3, std::make_unique<Bloc>(), reg, gen);
  HostImpl& host = host_of(*w);

  const uint64_t first = host.watch(7, 0);
  host.unwatch(first);
  const uint64_t second = host.watch(7, 0);  // MÊME numéro de descripteur

  CHECK(first != second);
  CHECK_EQ(sshos::key_window(first), static_cast<sshos::WindowId>(3));
  CHECK_EQ(sshos::key_window(second), static_cast<sshos::WindowId>(3));

  // Le registrar a bien vu passer les deux surveillances et le retrait
  // entre elles, avec les clés exactes que l'hôte a rendues.
  REQUIRE_EQ(reg.watches.size(), static_cast<size_t>(2));
  CHECK_EQ(reg.watches[0].key, first);
  CHECK_EQ(reg.watches[1].key, second);
  REQUIRE_EQ(reg.unwatches.size(), static_cast<size_t>(1));
  CHECK_EQ(reg.unwatches[0].fd, 7);
}

// Deux fenêtres différentes ne peuvent jamais se voler un événement, quelle
// que soit la façon dont les générations tombent.
TEST(host_keys_carry_the_window_that_owns_them) {
  FakeRegistrar reg;
  uint32_t gen = 16;  // compteur PARTAGÉ, comme dans la session
  auto a = make_window(3, std::make_unique<Bloc>(), reg, gen);
  auto b = make_window(9, std::make_unique<Bloc>(), reg, gen);

  const uint64_t ka = host_of(*a).watch(11, 0);
  const uint64_t kb = host_of(*b).watch(11, 0);

  CHECK_EQ(sshos::key_window(ka), static_cast<sshos::WindowId>(3));
  CHECK_EQ(sshos::key_window(kb), static_cast<sshos::WindowId>(9));
  CHECK(!host_of(*a).owns(kb));
  CHECK(!host_of(*b).owns(ka));
}

// LE test qui justifie les générations. Sans elles, cet événement périmé
// serait livré à l'application comme s'il était le sien.
TEST(host_drops_an_event_carrying_a_stale_key) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(3, std::make_unique<Bloc>(), reg, gen);
  HostImpl& host = host_of(*w);

  const uint64_t stale = host.watch(7, 0);
  host.unwatch(stale);
  const uint64_t fresh = host.watch(7, 0);

  CHECK(!host.owns(stale));
  CHECK(host.owns(fresh));
}

// Le même, mais observé du côté qui compte : l'application ne doit JAMAIS
// voir passer l'événement périmé. Sans cette assertion, un hôte qui livre
// tout sans filtrer reste vert -- Battement vérifie son propre jeton et
// masquerait la faute.
TEST(host_never_hands_a_stale_event_to_the_application) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(3, std::make_unique<Nosy>(), reg, gen);
  HostImpl& host = host_of(*w);
  auto* app = static_cast<Nosy*>(w->app.get());

  const uint64_t stale = host.watch(7, 0);
  host.unwatch(stale);
  const uint64_t fresh = host.watch(7, 0);

  CHECK(host.deliver(stale, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->calls, 0);

  CHECK(host.deliver(fresh, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->calls, 1);
  // Le jeton rendu à l'application est EXACTEMENT celui que watch() lui a
  // donné : c'est le contrat de Host, et c'est ce qui permet à une
  // application à plusieurs descripteurs de savoir lequel s'est réveillé.
  CHECK_EQ(app->last, fresh);
}

TEST(host_unwatches_before_the_app_closes_its_descriptor) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  {
    auto w = make_window(4, std::make_unique<Battement>(), reg, gen);
    w->app->attach(host_of(*w));
    REQUIRE(!reg.watches.empty());
  }  // w meurt ici : app d'abord, host ensuite
  REQUIRE(!reg.unwatches.empty());
  for (const auto& u : reg.unwatches) {
    CHECK(u.still_open);
  }
}

// unwatch_all() est la ceinture : une fenêtre fermée alors que son
// application surveille encore quelque chose ne doit laisser aucune entrée
// epoll derrière elle.
TEST(host_unwatch_all_retires_every_live_watch) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(4, std::make_unique<Bloc>(), reg, gen);
  HostImpl& host = host_of(*w);

  const uint64_t a = host.watch(11, 0);
  const uint64_t b = host.watch(12, 0);
  host.unwatch_all();

  CHECK(!host.owns(a));
  CHECK(!host.owns(b));
  REQUIRE_EQ(reg.unwatches.size(), static_cast<size_t>(2));
  CHECK_EQ(reg.unwatches[0].fd, 11);
  CHECK_EQ(reg.unwatches[1].fd, 12);
}

TEST(battement_counts_what_it_reads) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(5, std::make_unique<Battement>(), reg, gen);
  w->app->attach(host_of(*w));
  REQUIRE_EQ(reg.watches.size(), static_cast<size_t>(1));

  auto* app = static_cast<Battement*>(w->app.get());
  CHECK_EQ(app->beats(), 0);
  app->beat();
  CHECK(host_of(*w).deliver(reg.watches[0].key, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->beats(), 1);

  // Une clé périmée n'atteint pas l'application : le compteur ne bouge pas.
  CHECK(host_of(*w).deliver(reg.watches[0].key ^ 1u, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->beats(), 1);
}

// L'écho applicatif du correctif EPOLLHUP du jalon 1 : ce qui est arrivé
// avant la fermeture doit être lu AVANT que la source soit déclarée morte.
TEST(battement_drains_before_it_declares_its_source_dead) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(6, std::make_unique<Battement>(), reg, gen);
  w->app->attach(host_of(*w));
  REQUIRE_EQ(reg.watches.size(), static_cast<size_t>(1));

  auto* app = static_cast<Battement*>(w->app.get());
  app->beat();
  app->beat();
  app->beat();
  app->cut_source();  // ferme l'extrémité d'écriture

  // Un seul réveil, portant les deux drapeaux à la fois -- exactement ce que
  // le noyau livre quand des octets et un FIN arrivent groupés.
  const IoStatus st = host_of(*w).deliver(reg.watches[0].key, EPOLLIN | EPOLLHUP);
  CHECK_EQ(app->beats(), 3);
  CHECK(st == IoStatus::Closed);
  CHECK(!app->source_alive());

  // Et la surveillance est bel et bien retirée, avant que le descripteur ne
  // soit refermé.
  REQUIRE_EQ(reg.unwatches.size(), static_cast<size_t>(1));
  CHECK(reg.unwatches[0].still_open);
  CHECK(!host_of(*w).owns(reg.watches[0].key));
}

// La clé fait l'aller-retour par le NOYAU. C'est le seul test qui exerce
// vraiment la raison d'être de la conversion de daemon.cpp aux u64 :
// epoll_event.data est une union, et ce qu'on y range doit ressortir
// intact du epoll_wait().
TEST(a_real_epoll_hands_back_exactly_the_key_the_host_registered) {
  sshos::Fd ep(::epoll_create1(EPOLL_CLOEXEC));
  REQUIRE(ep.get() >= 0);
  RealRegistrar reg;
  reg.ep = ep.get();
  uint32_t gen = sshos::kGenFirstDynamic;

  auto w = make_window(7, std::make_unique<Battement>(), reg, gen);
  w->app->attach(host_of(*w));
  const uint64_t key = reg.last_key;
  REQUIRE(key != 0u);
  CHECK_EQ(sshos::key_window(key), static_cast<sshos::WindowId>(7));

  auto* app = static_cast<Battement*>(w->app.get());
  app->beat();

  epoll_event evs[4];
  const int n = ::epoll_wait(ep.get(), evs, 4, 500);
  REQUIRE_EQ(n, 1);
  // Recopié dans une variable : epoll_event est un type compacté sur x86-64,
  // et on ne peut pas prendre de référence sur un champ compacté.
  const uint64_t got = evs[0].data.u64;
  const uint32_t what = evs[0].events;
  CHECK_EQ(got, key);
  CHECK(host_of(*w).deliver(got, what) == IoStatus::Ok);
  CHECK_EQ(app->beats(), 1);
}

// Le canal application -> session. Il est resté muet jusqu'à ce que
// l'horloge du panneau ait besoin de réclamer un repeint sans frappe.
TEST(host_invalidate_asks_for_a_repaint) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  bool dirty = false;
  auto w = make_window(3, std::make_unique<Bloc>(), reg, gen, dirty);

  CHECK(!dirty);
  host_of(*w).invalidate();
  CHECK(dirty);
}

// set_title et request_close passent par la fenêtre, pas par un canal
// détourné : c'est ce qui permet à une application d'être fermée par
// elle-même sans rien savoir du gestionnaire.
TEST(host_writes_the_title_and_the_close_request_on_its_own_window) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(3, std::make_unique<Bloc>(), reg, gen);

  host_of(*w).set_title("Titre");
  CHECK_EQ(w->title, std::string("Titre"));
  CHECK(!w->close_requested);
  host_of(*w).request_close();
  CHECK(w->close_requested);
}

// L'application est ce qui SAIT que son affichage a changé. Le démon ne
// peut pas le deviner : sans cet appel, le compteur de battements
// n'apparaîtrait à l'écran qu'à la frappe suivante.
TEST(battement_asks_for_a_repaint_when_it_reads) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  bool dirty = false;
  auto w = make_window(8, std::make_unique<Battement>(), reg, gen, dirty);
  w->app->attach(host_of(*w));
  REQUIRE_EQ(reg.watches.size(), static_cast<size_t>(1));

  auto* app = static_cast<Battement*>(w->app.get());
  app->beat();
  dirty = false;
  CHECK(host_of(*w).deliver(reg.watches[0].key, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->beats(), 1);
  CHECK(dirty);
}

// Les deux commandes du menu. La session ne sait pas ce qu'elles veulent
// dire -- elle les tend à l'application focalisée, qui les comprend.
TEST(battement_obeys_its_menu_commands) {
  FakeRegistrar reg;
  uint32_t gen = 16;
  auto w = make_window(9, std::make_unique<Battement>(), reg, gen);
  w->app->attach(host_of(*w));
  REQUIRE_EQ(reg.watches.size(), static_cast<size_t>(1));
  auto* app = static_cast<Battement*>(w->app.get());

  app->on_command("beat");
  app->on_command("beat");
  CHECK(host_of(*w).deliver(reg.watches[0].key, EPOLLIN) == IoStatus::Ok);
  CHECK_EQ(app->beats(), 2);

  // Une commande inconnue ne fait rien, et surtout ne casse rien.
  app->on_command("bruit");
  CHECK(app->source_alive());

  app->on_command("cut");
  CHECK(host_of(*w).deliver(reg.watches[0].key, EPOLLIN | EPOLLHUP) ==
        IoStatus::Closed);
  CHECK(!app->source_alive());
}
