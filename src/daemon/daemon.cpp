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
#include <string>
#include <vector>

#include "common/frameclock.hpp"
#include "common/net.hpp"
#include "common/outqueue.hpp"
#include "common/platform.hpp"
#include "common/proto.hpp"
#include "daemon/session.hpp"
#include "input/parser.hpp"
#include "render/diff.hpp"
#include "render/surface.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

constexpr size_t kBackpressureCeiling = 1u << 20;  // 1 Mo
constexpr int kFrameIntervalMs = 33;               // 30 fps

struct Client {
  Fd fd;
  Decoder dec;
  InputParser input;
  OutQueue out{kBackpressureCeiling};
  std::unique_ptr<Differ> differ;
  int cols = 80;
  int rows = 24;
};

void epoll_mod(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

void epoll_add(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

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
  Session session(plat, 80, 24);
  Surface screen(80, 24);
  FrameClock clock{std::chrono::milliseconds(kFrameIntervalMs)};

  Fd ep(::epoll_create1(EPOLL_CLOEXEC));
  Fd sigfd = make_signalfd();
  Fd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));

  epoll_add(ep.get(), listener.get(), EPOLLIN);
  epoll_add(ep.get(), sigfd.get(), EPOLLIN);
  epoll_add(ep.get(), timer.get(), EPOLLIN);

  std::unique_ptr<Client> client;
  std::string scratch(65536, '\0');
  bool running = true;

  // A2 : descripteurs fermés pendant le lot epoll en cours. epoll_wait()
  // remplit son tableau d'événements d'après l'état au moment de l'attente ;
  // si drop_client() ferme un descripteur puis qu'un accept_peer() ultérieur
  // du MÊME lot lui redonne ce numéro (le noyau réutilise les plus petits
  // numéros libres), la garde `fd != client->fd.get()` laisserait passer un
  // événement périmé vers le nouveau client. Vidée au début de chaque tour.
  std::vector<int> closed_this_batch;

  const auto drop_client = [&](const char* reason) {
    if (!client) return;
    if (reason != nullptr) {
      client->out.push(encode(Msg{Detached{reason}}));
      client->out.flush(client->fd.get());
    }
    ::epoll_ctl(ep.get(), EPOLL_CTL_DEL, client->fd.get(), nullptr);
    closed_this_batch.push_back(client->fd.get());
    client.reset();
  };

  while (running) {
    closed_this_batch.clear();

    epoll_event evs[16];
    const int timeout = clock.delay_ms(FrameClock::Clock::now());
    const int n = ::epoll_wait(ep.get(), evs, 16, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = evs[i].data.fd;
      const uint32_t events = evs[i].events;

      if (fd == sigfd.get()) {
        signalfd_siginfo si{};
        while (::read(sigfd.get(), &si, sizeof si) == sizeof si) {
          if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) running = false;
        }
        continue;
      }

      if (fd == timer.get()) {
        uint64_t ticks = 0;
        while (::read(timer.get(), &ticks, sizeof ticks) == sizeof ticks) {
        }
        continue;
      }

      if (fd == listener.get()) {
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
            // Le nouveau prend la main : l'ancien est détaché, pas partagé.
            drop_client("un autre client a pris la main");
            client = std::make_unique<Client>();
            client->fd = std::move(fresh);
            epoll_add(ep.get(), client->fd.get(), EPOLLIN);
            break;
          }
        }
        continue;
      }

      // A2 : un événement pour un descripteur déjà fermé dans ce lot est
      // périmé par construction, même si son numéro a été redonné à un
      // client tout neuf par l'accept ci-dessus.
      if (std::find(closed_this_batch.begin(), closed_this_batch.end(), fd) !=
          closed_this_batch.end()) {
        continue;
      }

      if (!client || fd != client->fd.get()) continue;

      // EPOLLHUP est signalé quel que soit le masque demandé : un
      // répartiteur qui ne teste que EPOLLIN boucle à 100 % de CPU.
      if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
        drop_client(nullptr);
        continue;
      }

      if ((events & EPOLLOUT) != 0) {
        if (!client->out.flush(client->fd.get())) {
          drop_client(nullptr);
          continue;
        }
        if (!client->out.wants_write()) epoll_mod(ep.get(), fd, EPOLLIN);
      }

      if ((events & EPOLLIN) != 0) {
        bool closed = false;
        for (;;) {
          const ssize_t got = ::read(fd, scratch.data(), scratch.size());
          if (got > 0) {
            client->dec.feed(std::string_view(scratch.data(), static_cast<size_t>(got)));
            continue;
          }
          if (got == 0) closed = true;
          if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          if (got < 0 && errno == EINTR) continue;
          if (got < 0) closed = true;
          break;
        }

        while (auto m = client->dec.next()) {
          if (const auto* h = std::get_if<Hello>(&*m)) {
            if (h->build_id != kBuildId) {
              client->out.push(encode(Msg{Incompatible{
                  "version du demon differente : relancez `sshos --kill`"}}));
              client->out.flush(client->fd.get());
              drop_client(nullptr);
              break;
            }
            client->cols = h->cols;
            client->rows = h->rows;
            client->differ = std::make_unique<Differ>(
                OutputProfile::detect(h->term, h->colorterm, h->utf8));
            set_ambiguous_wide(false);
            screen.resize(h->cols, h->rows);
            session.resize(h->cols, h->rows);
            client->out.push(encode(Msg{Welcome{}}));
            clock.mark_dirty();
          } else if (const auto* in = std::get_if<Input>(&*m)) {
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

        if (session.wants_quit()) running = false;
        if (closed) drop_client(nullptr);
      }
    }

    // Composition : au plus une fois par intervalle, après avoir tout drainé.
    const auto now = FrameClock::Clock::now();
    if (client && client->differ && clock.delay_ms(now) == 0) {
      session.render(screen);
      const std::string ansi = client->differ->frame(screen, std::nullopt);
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
          epoll_mod(ep.get(), client->fd.get(), EPOLLIN | EPOLLOUT);
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
