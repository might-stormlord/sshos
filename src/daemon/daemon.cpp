#include "daemon/daemon.hpp"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/frameclock.hpp"
#include "common/net.hpp"
#include "common/outqueue.hpp"
#include "common/platform.hpp"
#include "common/proto.hpp"
#include "daemon/host.hpp"
#include "daemon/reap.hpp"
#include "daemon/session.hpp"
#include "input/parser.hpp"
#include "render/diff.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

constexpr size_t kBackpressureCeiling = 1u << 20;  // 1 Mo
constexpr int kFrameIntervalMs = 33;               // 30 fps

// Item 2 (voir le rapport de tâche) : borne temporelle de la connexion
// `pending` — voir son commentaire plus bas. Un vrai client écrit son Hello
// tout de suite après connect(), avant même de lire quoi que ce soit
// (quelques millisecondes tout au plus, voir run_client() dans
// src/client/client.cpp) ; 5 s est très large au-dessus de ce cas normal
// tout en bornant strictement un pair muet, lent ou hostile qui n'écrirait
// jamais et ne fermerait jamais non plus.
constexpr std::chrono::milliseconds kPendingHelloTimeout{5000};

struct Client {
  Fd fd;
  // Clé epoll de CETTE connexion. Sa génération est neuve à chaque accept(),
  // donc un événement en retard sur un numéro de descripteur déjà recyclé
  // porte l'ancienne génération et n'est reconnu par personne.
  uint64_t key = 0;
  Decoder dec;
  InputParser input;
  OutQueue out{kBackpressureCeiling};
  std::unique_ptr<Differ> differ;
  int cols = 80;
  int rows = 24;
};

// epoll_event.data est une UNION : impossible d'y ranger un descripteur
// pour les fds du démon et un u64 pour ceux des applications. Toute la
// boucle est donc passée aux clés (voir make_key dans daemon/host.hpp), y
// compris pour ses propres descripteurs.
void epoll_mod(int ep, uint64_t key, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = key;
  ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

void epoll_add(int ep, uint64_t key, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = key;
  ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

// Le registrar concret. La session ne voit jamais l'epoll ; le démon ne
// sait jamais ce qu'une clé désigne.
struct EpollRegistrar : FdRegistrar {
  int ep = -1;
  void watch(uint64_t key, int fd, uint32_t events) override {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = key;
    if (::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST) {
      ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
    }
  }
  void unwatch(int fd) override { ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr); }
};

Fd make_signalfd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGCHLD);
  ::sigprocmask(SIG_BLOCK, &mask, nullptr);
  return Fd(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
}

}  // namespace

int run_daemon(std::string_view socket_name) {
  Fd listener;
  try {
    listener = bind_abstract(socket_name);
  } catch (const AddressInUse&) {
    return 0;  // un démon tourne déjà : rien à faire, ce n'est pas une erreur
  } catch (const std::exception&) {
    return 1;
  }
  set_nonblock(listener.get());

  RealPlatform plat;
  Surface screen(80, 24);
  FrameClock clock{std::chrono::milliseconds(kFrameIntervalMs)};

  Fd ep(::epoll_create1(EPOLL_CLOEXEC));
  Fd sigfd = make_signalfd();
  Fd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));

  // L'ordre de déclaration EST la durée de vie : `session` meurt avant
  // `registrar`, qui meurt avant `ep`. Les applications appellent unwatch()
  // depuis leurs destructeurs -- l'epoll doit donc leur survivre.
  EpollRegistrar registrar;
  registrar.ep = ep.get();
  Session session(plat, registrar, 80, 24);

  // Valeur figee du projet : 50 ms d'ambiguite pour l'echappement.
  constexpr auto kEscAmbiguity = std::chrono::milliseconds(50);
  FrameClock::Clock::time_point last_refresh = FrameClock::Clock::now();
  bool esc_armed = false;
  FrameClock::Clock::time_point esc_deadline{};

  // Les trois descripteurs du démon vivent aussi longtemps que le
  // processus : leur génération est fixe. Les connexions, elles, en tirent
  // une neuve à chaque accept().
  epoll_add(ep.get(), make_key(0, kGenListener), listener.get(), EPOLLIN);
  epoll_add(ep.get(), make_key(0, kGenSignal), sigfd.get(), EPOLLIN);
  epoll_add(ep.get(), make_key(0, kGenTimer), timer.get(), EPOLLIN);
  uint32_t next_gen = kGenFirstDynamic;

  std::unique_ptr<Client> client;

  // Item 2 (voir le rapport de tâche) : connexion acceptée mais qui n'a pas
  // encore décliné son intention en envoyant un Hello valide. Tant qu'elle
  // reste ici, elle n'a strictement AUCUN effet sur `client` : ni éviction,
  // ni partage d'aucun état de session. Elle n'est promue en `client` (avec
  // éviction de l'ancien, s'il existe) qu'à la réception effective d'un
  // Hello compatible — voir le traitement de pending->fd plus bas. Deux
  // volets de bornage, documentés à leurs sites respectifs : (1) une
  // nouvelle connexion acceptée alors que `pending` est déjà occupée
  // remplace immédiatement l'ancienne (jamais plus d'une connexion muette
  // en attente à la fois — voir le cas AcceptOutcome::Accepted) ; (2) au-delà
  // de kPendingHelloTimeout sans Hello, elle est fermée d'office (voir la fin
  // de la boucle, après le traitement des événements de ce tour).
  std::unique_ptr<Client> pending;
  FrameClock::Clock::time_point pending_deadline{};

  std::string scratch(65536, '\0');
  bool running = true;

  // A2 : CLÉS fermées pendant le lot epoll en cours, qu'elles appartinssent
  // à `client` ou à `pending`. epoll_wait() remplit son tableau
  // d'événements d'après l'état au moment de l'attente ; si un drop_*()
  // ferme un descripteur puis qu'un accept_peer() ultérieur du MÊME lot lui
  // redonne ce numéro (le noyau réutilise les plus petits numéros libres),
  // les gardes `key == client->key` / `key == pending->key` laisseraient
  // passer un événement périmé vers le nouvel occupant de ce numéro. Vidée
  // au début de chaque tour.
  //
  // Depuis le passage aux clés générationnelles, cette liste n'est même
  // plus la seule ligne de défense : une connexion neuve tire une
  // génération neuve à chaque accept(), donc un événement en retard porte
  // une clé que PLUS PERSONNE ne reconnaît et tombe dans la branche
  // applicative finale, où il est jeté. C'est ce qui referme pour de bon le
  // trou décrit ci-dessus.
  std::vector<uint64_t> closed_this_batch;

  const auto drop_client = [&](const char* reason) {
    if (!client) return;
    if (reason != nullptr) {
      client->out.push(encode(Msg{Detached{reason}}));
      client->out.flush(client->fd.get());
    }
    ::epoll_ctl(ep.get(), EPOLL_CTL_DEL, client->fd.get(), nullptr);
    closed_this_batch.push_back(client->key);
    client.reset();
    // Sans client, plus rien n'est rendable : aucun dirty_ résiduel ne doit
    // survivre à son départ (voir FrameClock::reset() pour le scénario
    // précis que ça évite — Input/Resize reçu avant tout Hello, qui a
    // laissé dirty_ vrai sans jamais être consommé par note_render()).
    clock.reset();
    // La souris qui tenait le glissement vient de disparaître : le geste n'a
    // plus personne pour le finir. Sans cet appel il survivrait au
    // détachement et le premier mouvement du client SUIVANT le reprendrait
    // en vol.
    session.cancel_drag();
  };

  // Ferme silencieusement `pending` : contrairement à drop_client(), aucun
  // message n'est envoyé, quelle que soit la cause (fermeture par le pair,
  // délai dépassé, protocole violé — voir Decoder::failed() — ou remplacement
  // par une connexion plus récente). `pending` n'est jamais « attaché » à
  // personne : il n'y a rien à annoncer à un pair qui n'a pas fini de se
  // présenter.
  const auto drop_pending = [&]() {
    if (!pending) return;
    ::epoll_ctl(ep.get(), EPOLL_CTL_DEL, pending->fd.get(), nullptr);
    closed_this_batch.push_back(pending->key);
    pending.reset();
  };

  // Draine tout ce qui est immédiatement lisible sur c.fd dans son décodeur.
  // Renvoie vrai si le pair a fermé (read() a renvoyé 0) ou si la lecture a
  // échoué autrement qu'un EAGAIN/EWOULDBLOCK attendu en non-bloquant.
  // Factorisé : `client` et `pending` partagent exactement cette logique de
  // lecture, seul le traitement des messages décodés diffère selon le rôle.
  const auto drain_socket = [&](Client& c) {
    bool closed = false;
    for (;;) {
      const ssize_t got = ::read(c.fd.get(), scratch.data(), scratch.size());
      if (got > 0) {
        c.dec.feed(std::string_view(scratch.data(), static_cast<size_t>(got)));
        continue;
      }
      if (got == 0) closed = true;
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (got < 0 && errno == EINTR) continue;
      if (got < 0) closed = true;
      break;
    }
    return closed;
  };

  while (running) {
    closed_this_batch.clear();

    epoll_event evs[16];
    // Invariant : le démon ne doit jamais scruter activement. S'il n'y a
    // rien de rendable — pas de client, ou un client dont le differ n'est
    // pas encore construit (avant Hello, cf. la branche Hello plus bas) —
    // la seule source d'information utile est un évènement epoll : on
    // bloque indéfiniment (timeout -1) plutôt que de laisser
    // clock.delay_ms() commander un réveil immédiat qu'aucune composition
    // ne viendra jamais consommer (le bloc de composition ci-dessous est
    // lui-même gardé par `client && client->differ`, seul site qui appelle
    // note_render() et efface dirty_). Posée ici, au site de consommation
    // du délai — plutôt que seulement en gardant les appels à
    // mark_dirty() dans les branches Input/Resize ci-dessous — cette garde
    // protège l'invariant même si un futur appelant de mark_dirty()
    // oubliait sa propre garde : c'est la panne réellement observée (voir
    // le rapport de tâche), pas une hypothèse d'école.
    //
    // Item 2 : `pending` doit aussi pouvoir réveiller la boucle même quand
    // rien n'est rendable, sans quoi kPendingHelloTimeout ne bornerait rien
    // en pratique (le timeout -1 ci-dessus bloquerait indéfiniment tant
    // qu'aucun événement epoll ne survient). On raccourcit donc le délai au
    // temps restant avant l'échéance de `pending`, sans jamais l'allonger :
    // ce n'est pas du scrutin actif, juste un réveil borné en plus (voire à
    // la place) de celui du rendu.
    // L'AMBIGUITE DE L'ECHAPPEMENT. Un `ESC` seul est indecidable tant
    // qu'aucun octet ne suit : c'est peut-etre la touche, c'est peut-etre
    // le debut d'une sequence. `InputParser` le retient et attend qu'on lui
    // dise que le delai a expire -- et personne ne le lui disait.
    //
    // La consequence se voit des qu'un invite a besoin de la touche : dans
    // `vim`, l'echappement ne quittait JAMAIS le mode insertion. Il restait
    // en attente jusqu'a la frappe suivante, et se relisait alors comme un
    // accord `Alt` avec elle. Trouve en faisant tourner un vrai `vim`, pas
    // par un test unitaire : le parseur, lui, honore parfaitement le
    // `timeout()` qu'on ne lui appelait pas.
    if (client && client->input.esc_pending()) {
      const auto now = FrameClock::Clock::now();
      if (!esc_armed) {
        esc_armed = true;
        esc_deadline = now + kEscAmbiguity;
      } else if (now >= esc_deadline) {
        client->input.timeout();
        while (auto e = client->input.next()) session.on_input(*e);
        clock.mark_dirty();
        esc_armed = false;
      }
    } else {
      esc_armed = false;
    }

    const bool renderable = client && client->differ;
    int timeout = renderable ? clock.delay_ms(FrameClock::Clock::now()) : -1;
    // Avec un client attaché, on ne dort jamais plus d'une seconde. Sans ce
    // plancher, l'horloge du panneau resterait figée jusqu'à la prochaine
    // frappe : delay_ms() rend -1 tant que rien n'est sale (frameclock.hpp),
    // et rien ne peut plus jamais devenir sale tout seul. Ce n'est pas du
    // scrutin actif -- le réveil ne compose que si take_dirty() ci-dessous
    // dit qu'il y a de quoi -- et sans client il n'y a aucun plancher du
    // tout : le démon au repos continue de bloquer indéfiniment.
    if (client) {
      timeout = (timeout < 0) ? 1000 : std::min(timeout, 1000);
    }
    if (pending) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          pending_deadline - FrameClock::Clock::now());
      const int pending_timeout = static_cast<int>(std::max<long long>(0, remaining.count()));
      timeout = (timeout < 0) ? pending_timeout : std::min(timeout, pending_timeout);
    }
    // L'aide qui s'ouvre d'elle-même après un accord resté en l'air. Sans ce
    // repli, elle attendrait le réveil suivant -- au mieux la seconde de
    // l'horloge ci-dessus, au pire la frappe que l'utilisateur ne sait
    // justement pas quelle est.
    const int help_delay = session.help_delay_ms();
    if (help_delay >= 0) {
      timeout = (timeout < 0) ? help_delay : std::min(timeout, help_delay);
    }
    // Le rafraichissement periodique d'une application VISIBLE -- le
    // moniteur systeme. Sans ce repli, ses chiffres ne bougeraient qu'a la
    // frappe suivante : le demon ne dessine que sur une trame sale, et
    // rien ne salit la trame quand seul le temps passe.
    const int refresh_delay = client ? session.refresh_delay_ms() : -1;
    if (refresh_delay >= 0) {
      const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
          FrameClock::Clock::now() - last_refresh);
      const int left = std::max(0, refresh_delay - static_cast<int>(since.count()));
      if (left == 0) {
        session.mark_refresh_due();
        clock.mark_dirty();
        last_refresh = FrameClock::Clock::now();
        timeout = 0;
      } else {
        timeout = (timeout < 0) ? left : std::min(timeout, left);
      }
    }
    // Meme repli pour l'echappement en attente : sans lui, la touche
    // n'arriverait qu'au reveil suivant -- c'est-a-dire a la frappe
    // suivante, la seule chose que l'utilisateur ne fera justement pas
    // apres avoir appuye sur Echap.
    if (esc_armed) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          esc_deadline - FrameClock::Clock::now());
      const int esc_timeout = static_cast<int>(std::max<long long>(0, left.count()));
      timeout = (timeout < 0) ? esc_timeout : std::min(timeout, esc_timeout);
    }
    const int n = ::epoll_wait(ep.get(), evs, 16, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      const uint64_t key = evs[i].data.u64;
      const uint32_t events = evs[i].events;

      if (key == make_key(0, kGenSignal)) {
        signalfd_siginfo si{};
        bool child_died = false;
        // DEUX drainages, et les deux comptent. Celui-ci vide le
        // `signalfd` jusqu'a EAGAIN : un enregistrement laisse derriere
        // lui laisserait epoll nous rappeler en boucle.
        while (::read(sigfd.get(), &si, sizeof si) == sizeof si) {
          if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) running = false;
          if (si.ssi_signo == SIGCHLD) child_died = true;
        }
        // Et celui-la recolte TOUS les morts, pas seulement celui que
        // l'enregistrement nommait : les signaux standards ne sont pas mis
        // en file, et trois enfants morts entre deux lectures n'en
        // produisent qu'un seul.
        if (child_died) reap_children(session);
        continue;
      }

      if (key == make_key(0, kGenTimer)) {
        uint64_t ticks = 0;
        while (::read(timer.get(), &ticks, sizeof ticks) == sizeof ticks) {
        }
        continue;
      }

      if (key == make_key(0, kGenListener)) {
        AcceptResult r = accept_peer(listener.get(), ::getuid());
        switch (r.outcome) {
          case AcceptOutcome::Empty:
            // Rien à accepter : un réveil en avance d'epoll, ou la
            // connexion a déjà été traitée. Ce n'est pas une erreur.
            break;
          case AcceptOutcome::Rejected:
            // Uid différent du nôtre : refusé, mais gardé comme cas
            // distinct pour le diagnostic (r.cred porte les credentials du
            // pair refusé). Rien de plus à faire dans ce bouchon.
            break;
          case AcceptOutcome::TransientError:
            // EMFILE/ENFILE/ENOBUFS/ENOMEM et apparentées : passagères,
            // le prochain accept réussira probablement une fois la
            // pression retombée. Ne pas tuer le démon pour ça.
            break;
          case AcceptOutcome::FatalError:
            // Cassure de l'écouteur qui ne se résorbera pas d'elle-même.
            // Retenter à chaque tour sans condition tournerait à 100 % de
            // CPU si epoll continuait de signaler ce descripteur prêt ;
            // ne rien tenter de plus ici est le choix correct pour ce
            // jalon (un seul client à la fois, pas de journalisation
            // structurée encore).
            break;
          case AcceptOutcome::Accepted: {
            Fd fresh = std::move(r.fd);
            set_nonblock(fresh.get());
            // Item 2 (correction) : l'éviction du client attaché ne se
            // décide plus ici, à l'accept — une connexion qui vient de se
            // faire accepter n'a encore rien déclaré, elle peut tout aussi
            // bien être une simple sonde (--status, --kill, la sonde
            // d'attache initiale de main.cpp) qui ne s'attachera jamais.
            // Elle devient `pending` et n'aura d'effet sur `client` qu'à la
            // réception effective d'un Hello — voir le traitement de
            // pending->fd plus bas. Politique de bornage, premier volet : la
            // connexion la plus récente remplace toujours toute connexion
            // encore en attente d'un Hello, qu'il y ait déjà un `client`
            // attaché ou non — jamais plus d'une connexion muette à la fois
            // (second volet : kPendingHelloTimeout, appliqué en fin de tour).
            if (pending) drop_pending();
            pending = std::make_unique<Client>();
            pending->fd = std::move(fresh);
            pending->key = make_key(0, next_gen++);
            epoll_add(ep.get(), pending->key, pending->fd.get(), EPOLLIN);
            pending_deadline = FrameClock::Clock::now() + kPendingHelloTimeout;
            break;
          }
        }
        continue;
      }

      // A2 : un événement pour un descripteur déjà fermé dans ce lot est
      // périmé par construction, même si son numéro a été redonné à une
      // connexion toute neuve (client OU pending) par l'accept ci-dessus.
      //
      // Réexamen (item 2, voir le rapport de tâche) : la topologie compte
      // désormais jusqu'à DEUX connexions vivantes (`client` et `pending`)
      // au lieu d'une seule, ce qui rend la réutilisation de numéro de
      // descripteur encore plus facile à provoquer qu'avant — voir
      // daemon_handles_client_takeover_with_reused_fd_in_one_epoll_batch
      // dans tests/test_session.cpp, dont le scénario (SIGSTOP puis
      // coexistence forcée d'une fermeture et d'une connexion entrante dans
      // le même lot, confirmée par strace) continue de démontrer que la
      // réutilisation elle-même se produit bel et bien. Ce qui NE change
      // pas, en revanche, c'est la garantie sur laquelle repose l'analyse
      // d'irraisonnabilité : epoll_wait() ne rend jamais deux entrées pour
      // le même numéro de descripteur au sein d'un seul appel (garantie du
      // noyau, indépendante du nombre de connexions que CE code choisit de
      // suivre), et cette boucle n'accepte au plus qu'UNE connexion par
      // tour (une seule branche `if (fd == listener.get())`, jamais
      // rebouclée). Un même numéro de descripteur ne peut donc jamais
      // apparaître deux fois dans evs[] pour un même epoll_wait() — la
      // réattribution provoquée par un drop_*() suivi d'un accept() DANS
      // CE TOUR ne laisse tout simplement aucune entrée périmée derrière
      // elle à filtrer : l'entrée qui référençait l'ancien descripteur a
      // déjà été consommée avant que sa réattribution ne survienne (sinon
      // celle-ci n'aurait pas encore eu lieu). Remplacer cette condition
      // par `if (false)` ne fait donc échouer aucun test de la suite,
      // ancienne comme nouvelle (vérifié explicitement, voir le rapport de
      // tâche) : la garde reste structurellement inatteignable, pour une
      // raison plus générale et plus solide que celle avancée au round
      // précédent (« un seul client à la fois »), qui était accessoire et
      // non la véritable cause. Gardée telle quelle : elle protège
      // correctement une extension future (plusieurs accepts par tour, ou
      // un tableau evs[] plus grand traité en plusieurs passes), et son
      // coût est nul.
      if (std::find(closed_this_batch.begin(), closed_this_batch.end(), key) !=
          closed_this_batch.end()) {
        continue;
      }

      if (client && key == client->key) {
        if ((events & EPOLLOUT) != 0) {
          if (!client->out.flush(client->fd.get())) {
            drop_client(nullptr);
            continue;
          }
          if (!client->out.wants_write()) {
            epoll_mod(ep.get(), client->key, client->fd.get(), EPOLLIN);
          }
        }

        // Round C (voir le rapport de tâche) : on draine et on traite
        // EPOLLIN AVANT d'honorer EPOLLHUP/EPOLLERR ci-dessous, jamais
        // l'inverse. epoll_wait() coalesce couramment EPOLLIN et EPOLLHUP
        // dans le MÊME événement quand le pair écrit puis ferme aussitôt
        // (dernières frappes suivies d'une fermeture immédiate, sans
        // attendre) : à cet instant précis, les octets sont encore dans le
        // tampon de réception du socket, et honorer HUP avant de les avoir
        // lus les jetterait purement et simplement — alors même que la
        // session du démon, elle, survit au détachement (c'est tout
        // l'intérêt du démon). `closed` capture la fermeture vue par
        // drain_socket() (read() rendant 0, ou une erreur de lecture
        // franche sous EPOLLERR) ; elle est combinée plus bas avec le bit
        // HUP/ERR pour ne jamais appeler drop_client() plus d'une fois pour
        // la même fermeture.
        bool closed = false;

        if ((events & EPOLLIN) != 0) {
          closed = drain_socket(*client);

          while (auto m = client->dec.next()) {
            // Hello n'est plus jamais attendu ici : il est géré exclusivement
            // via `pending`, avant promotion (voir plus bas). Un Hello reçu
            // malgré tout sur une connexion déjà attachée (pair non
            // conforme) est silencieusement ignoré, comme le sont déjà
            // Welcome/Incompatible/Detached/FrameMsg — aucun de ces tags
            // n'a de sens venant du client une fois l'attache faite.
            if (const auto* in = std::get_if<Input>(&*m)) {
              client->input.feed(in->bytes);
              while (auto e = client->input.next()) session.on_input(*e);
              clock.mark_dirty();
            } else if (const auto* rz = std::get_if<Resize>(&*m)) {
              client->cols = rz->cols;
              client->rows = rz->rows;
              screen.resize(rz->cols, rz->rows);
              session.resize(rz->cols, rz->rows);
              if (client->differ) client->differ->invalidate();
              clock.mark_dirty();
            }
          }

          // Item 1 (voir le rapport de tâche) : Decoder::failed() documente
          // (proto.hpp) qu'un pair qui viole le protocole laisse la
          // connexion marquée en échec définitif, « à charge pour
          // l'appelant de la fermer ». Personne ne le faisait : feed()
          // n'accumule plus rien et next() ne renvoie plus jamais rien une
          // fois failed_ vrai (proto.cpp), donc rien ne distinguait plus
          // « rien à lire pour l'instant » de « ce pair ne parlera plus
          // jamais correctement » — la connexion gelait en silence, pour
          // toujours, sans être fermée. Tout ce qui a pu être décodé
          // valablement AVANT la corruption a déjà été traité par la boucle
          // ci-dessus (drainer d'abord) ; ici, on ferme si la suite ne
          // viendra jamais (tester ensuite). Reste atteignable après le
          // réordonnancement du round C : ce test s'exécute toujours sur le
          // même chemin, juste après le drainage, avant tout `continue` lié
          // à HUP/ERR ci-dessous.
          if (client->dec.failed()) {
            drop_client("message de protocole invalide");
            continue;
          }

          if (session.wants_quit()) running = false;
        }

        // EPOLLHUP est signalé quel que soit le masque demandé : un
        // répartiteur qui ne teste que EPOLLIN boucle à 100 % de CPU si le
        // bit reste ignoré indéfiniment — d'où l'appel à drop_client() ici,
        // qui retire le descripteur d'epoll (EPOLL_CTL_DEL) et empêche donc
        // tout réveil ultérieur sur lui. `closed` (vu ci-dessus via
        // drain_socket) et ce bit peuvent signaler la MÊME fermeture au
        // sein d'un seul tour (exactement le cas visé par ce round) : le
        // `||` garantit un seul appel à drop_client(), jamais deux.
        if (closed || (events & (EPOLLHUP | EPOLLERR)) != 0) {
          drop_client(nullptr);
          continue;
        }
      } else if (pending && key == pending->key) {
        // Item 2 : une connexion qui n'a pas encore décliné son intention
        // n'a aucun effet sur `client`, quoi qu'elle envoie ou qu'elle
        // fasse — voir le commentaire au-dessus de la déclaration de
        // `pending`.
        //
        // Round C : même principe de réordonnancement que pour `client`
        // ci-dessus — voir son commentaire pour le détail complet de la
        // coalescence EPOLLIN|EPOLLHUP par epoll_wait().
        bool closed = false;

        if ((events & EPOLLIN) != 0) {
          closed = drain_socket(*pending);

          // Drainer d'abord : un Hello valide arrivé avant une éventuelle
          // corruption plus loin dans le même flux compte (même principe
          // que pour `client` ci-dessus, item 1). Tout le reste (Input,
          // Resize, un Hello redondant...) est ignoré : `pending` n'a pas
          // de session à faire évoluer tant qu'il n'est pas promu.
          std::optional<Hello> hello_seen;
          while (auto m = pending->dec.next()) {
            if (const auto* h = std::get_if<Hello>(&*m); h != nullptr && !hello_seen) {
              hello_seen = *h;
            }
          }

          // Item 1, appliqué à `pending` : un pair qui viole le protocole
          // avant même d'avoir fini de se présenter n'est pas promu, quoi
          // qu'il ait pu envoyer de valide avant sa faute — voir le
          // commentaire équivalent côté `client`. Testé APRÈS le drainage
          // ci-dessus mais AVANT toute promotion, pour ne jamais attacher
          // une connexion déjà connue comme félonne.
          if (pending->dec.failed()) {
            drop_pending();
            continue;
          }

          if (hello_seen) {
            if (hello_seen->build_id != kBuildId) {
              pending->out.push(encode(Msg{Incompatible{
                  "version du demon differente : relancez `sshos --kill`"}}));
              pending->out.flush(pending->fd.get());
              drop_pending();
              continue;
            }
            // Éviction retardée jusqu'ici : c'est exactement la correction
            // de l'item 2. Le client en place n'est perturbé qu'une fois
            // que la nouvelle connexion a réellement décliné son intention
            // de s'attacher, jamais avant (drop_client() est un no-op si
            // `client` est déjà vide, ce qui couvre aussi la toute première
            // connexion du démon).
            drop_client("un autre client a pris la main");
            client = std::move(pending);
            client->cols = hello_seen->cols;
            client->rows = hello_seen->rows;
            const OutputProfile prof = OutputProfile::detect(
                hello_seen->term, hello_seen->colorterm, hello_seen->utf8);
            client->differ = std::make_unique<Differ>(prof);
            // Le nouveau client repart d'un Differ neuf, donc d'un repeint
            // complet : changer de thème ici n'a rien à invalider.
            session.set_output(prof);
            set_ambiguous_wide(false);
            screen.resize(hello_seen->cols, hello_seen->rows);
            session.resize(hello_seen->cols, hello_seen->rows);
            // Ceinture et bretelles avec drop_client() : un client qui
            // s'attache hérite d'un bureau propre, jamais d'un geste à
            // moitié fait par le précédent.
            //
            // Les deux sites sont redondants et c'est mesuré : retirer l'un
            // OU l'autre laisse
            // daemon_forgets_a_drag_left_behind_by_a_departed_client vert,
            // il faut retirer les deux pour le faire tomber. La redondance
            // est gardée à dessein -- une future voie d'attache qui ne
            // passerait pas par drop_client() (reprise de session, client
            // remplacé sans détachement préalable) retomberait ici.
            session.cancel_drag();
            client->out.push(encode(Msg{Welcome{}}));
            clock.mark_dirty();
            continue;
          }
        }

        // La sonde muette exacte que ce jalon corrige : ouvrir puis
        // refermer une connexion sans jamais rien écrire (--status,
        // --kill, la sonde d'attache de main.cpp). Fermeture silencieuse,
        // `client` n'est pas averti et continue de recevoir ses trames.
        // Round C : même combinaison `closed || HUP/ERR` que côté `client`
        // ci-dessus, et pour la même raison — un seul drop_pending() par
        // fermeture, jamais deux.
        if (closed || (events & (EPOLLHUP | EPOLLERR)) != 0) {
          drop_pending();
          continue;
        }
      } else {
        // Aucun descripteur du démon : c'est une application qui se
        // réveille. Une clé que plus aucune fenêtre ne reconnaît s'y jette
        // sans bruit -- c'est le cas NORMAL d'un réveil en retard sur une
        // surveillance déjà retirée, exactement ce que les générations
        // servent à distinguer.
        // Pas de mark_dirty() ici : c'est à l'application de dire que son
        // affichage a changé, via Host::invalidate() -- que take_dirty()
        // relève plus bas dans ce même tour. Salir d'office masquerait une
        // application qui oublie de le faire, et repeindrait le bureau pour
        // un réveil que l'application a peut-être ignoré.
        session.on_fd_event(key, events);
      }
    }

    // Item 2, second volet du bornage : une connexion `pending` qui n'a
    // jamais dit Hello et ne s'est jamais fermée non plus (pair lent, muet
    // en permanence, ou hostile) ne doit pas pouvoir occuper la place
    // indéfiniment. Vérifié une fois par tour plutôt que via un timerfd
    // séparé : le timeout d'epoll_wait ci-dessus est déjà raccourci pour
    // garantir que la boucle se réveille au plus tard à pending_deadline,
    // même sans aucun autre événement.
    if (pending && FrameClock::Clock::now() >= pending_deadline) {
      drop_pending();
    }

    // Ce que la session veut repeindre sans qu'on ait touché à une touche :
    // l'horloge qui change de minute, une application qui a appelé
    // Host::invalidate(). Interrogée une fois par tour, juste avant la
    // décision de composition.
    if (session.take_dirty()) clock.mark_dirty();

    // Détachement demandé : on congédie le client et on garde TOUT. La
    // session, ses fenêtres et les descripteurs que leurs applications
    // surveillent continuent de vivre ici ; le prochain Hello retrouvera le
    // bureau exactement dans cet état.
    if (session.take_detach()) drop_client("detache, la session continue");

    // Composition : au plus une fois par intervalle, après avoir tout drainé.
    const auto now = FrameClock::Clock::now();
    if (client && client->differ && clock.delay_ms(now) == 0) {
      session.render(screen);
      // Le repeint forcé se décide AVANT la trame : invalider après
      // l'avoir calculée ne servirait à rien de ce tour-ci.
      if (session.take_repaint()) client->differ->invalidate();
      // Le hors-bande passe DEVANT la trame : ce sont des modes de terminal
      // (la bascule souris), et ils doivent être posés avant tout dessin qui
      // en dépend. Le client recopie FrameMsg::ansi verbatim.
      const std::string oob = session.take_out_of_band();
      const std::string ansi = oob + client->differ->frame(screen, session.cursor());
      if (!ansi.empty()) client->out.push(encode(Msg{FrameMsg{ansi}}));
      clock.note_render(now);

      // A7 : un rejet de file en dépassement n'appelle pas toujours la même
      // réponse. Voir take_overflow() dans outqueue.hpp pour la définition
      // exacte de Clean/Dirty ; ici on se contente des deux réactions.
      const OutQueue::Overflow overflow = client->out.take_overflow();
      if (overflow == OutQueue::Overflow::Dirty) {
        // Rejet sale : une trame était déjà partiellement partie sur le fil
        // avant ce rejet. Le Decoder du pair attend le reste d'un message
        // dont il a lu la longueur annoncée, et ce reste ne viendra jamais —
        // il avalera silencieusement tout ce qui suit, y compris le repaint
        // censé réparer, sans même se mettre en failed(). Aucun repaint ne
        // rattrape un flux désynchronisé ; seule la fermeture répare. L'état
        // de session vit dans le démon, pas dans la connexion : le client se
        // rattache et reçoit un repaint complet sur un flux neuf et aligné.
        drop_client(nullptr);
      } else {
        if (overflow == OutQueue::Overflow::Clean) {
          // Rejet propre : rien de la file n'était encore parti sur le fil,
          // le pair est toujours aligné sur les frontières de trame.
          // invalidate() + repaint complet répare parfaitement.
          client->differ->invalidate();
          clock.mark_dirty();
        }
        if (!client->out.flush(client->fd.get())) {
          drop_client(nullptr);
        } else if (client->out.wants_write()) {
          epoll_mod(ep.get(), client->key, client->fd.get(), EPOLLIN | EPOLLOUT);
        }
      }
    }

    // Réarmer le timer si un rendu reste dû plus tard.
    // A5 : la cadence est portée deux fois — par le timeout d'epoll_wait
    // ci-dessus et par ce timerfd réarmé ici. C'est redondant et volontaire
    // (décision prise en amont de cette tâche, non remise en cause ici) ;
    // voir le rapport de tâche pour la justification.
    const int next = clock.delay_ms(FrameClock::Clock::now());
    itimerspec its{};
    if (next > 0) {
      its.it_value.tv_sec = next / 1000;
      its.it_value.tv_nsec = static_cast<long>(next % 1000) * 1000000L;
    }
    ::timerfd_settime(timer.get(), 0, &its, nullptr);
  }

  drop_client("le demon s'arrete");
  return 0;
}

}  // namespace sshos
