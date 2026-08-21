// Combien de fois un client rejoue son démarrage après « Redemarrer pour
// terminer ».
//
// CE QUE CE FICHIER EXISTE POUR ATTRAPER. Le 21 août 2026, un redémarrage de
// bureau sur deux échouait sur « sshos: le redemarrage n'a pas abouti ». Le
// compte vivait dans `src/main.cpp` -- `for (int attempt = 0; attempt < 2;
// ++attempt)` -- et comptait les redémarrages de TOUTE LA VIE DU CLIENT, pas
// les allers-retours stériles. Le premier redémarrage passait, le second
// était refusé sans même essayer de relancer un démon : l'utilisateur
// retombait au shell, bureau perdu, alors que rien n'était cassé. Un
// utilisateur qui relançait `sshos` repartait avec un compteur neuf --
// d'où le « une fois sur deux » exact.
//
// Comme pour `src/client/launch.cpp` (voir test_launch.cpp), sortir la
// décision de main.cpp n'est pas un rangement : CMakeLists retire main.cpp
// de `sshos_core`, donc rien de ce qui y vit n'est atteignable par la suite.
// C'est la moitié du correctif.

#include "client/restart.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <random>
#include <sstream>
#include <string>

#include "client/client.hpp"
#include "common/net.hpp"
#include "common/proto.hpp"
#include "harness.hpp"

namespace {

// LE CAS QUI A COÛTÉ LE BUREAU. Deux mises à jour dans la vie d'un même
// client, chacune suivie d'un bureau qui s'affiche et d'un utilisateur qui
// clique : les DEUX redémarrages doivent être accordés.
TEST(restart_budget_accorde_le_second_redemarrage_d_une_meme_session) {
  sshos::RestartBudget budget;
  CHECK(budget.allow(/*fruitful=*/true));
  CHECK(budget.allow(/*fruitful=*/true));
  // Et le dixième, tant que chacun sert à quelque chose : un redémarrage
  // demandé par un clic n'a aucune raison d'être rationné.
  for (int i = 0; i < 8; ++i) CHECK(budget.allow(/*fruitful=*/true));
}

// L'INTENTION D'ORIGINE, ELLE, EST GARDÉE. Un démon qui se détache pour se
// mettre à jour sans jamais avoir servi de bureau est un aller-retour
// stérile ; en enchaîner sans fin ferait tourner le client pour rien.
TEST(restart_budget_borne_les_allers_retours_steriles) {
  sshos::RestartBudget budget;
  CHECK(budget.allow(/*fruitful=*/false));
  CHECK(!budget.allow(/*fruitful=*/false));
}

// ET LE COMPTE REPART DE ZÉRO DÈS QU'UNE SESSION A SERVI. Sans cela, un
// aller-retour stérile isolé -- suivi de mois de bureau normal -- laisserait
// le client avec un seul redémarrage en réserve pour toujours.
TEST(restart_budget_repart_de_zero_apres_une_session_utile) {
  sshos::RestartBudget budget;
  CHECK(budget.allow(/*fruitful=*/false));
  CHECK(budget.allow(/*fruitful=*/true));
  CHECK(budget.allow(/*fruitful=*/false));
  CHECK(!budget.allow(/*fruitful=*/false));
}

// Le plafond est réglable, parce qu'un test doit pouvoir l'épuiser sans
// dépendre de la valeur du produit.
TEST(restart_budget_respecte_le_plafond_qu_on_lui_donne) {
  sshos::RestartBudget budget(3);
  CHECK(budget.allow(false));
  CHECK(budget.allow(false));
  CHECK(!budget.allow(false));
}

// Un plafond nul ou négatif refuse tout de suite plutôt que de boucler :
// c'est la lecture sûre d'un réglage absurde.
TEST(restart_budget_refuse_tout_sur_un_plafond_nul) {
  sshos::RestartBudget zero(0);
  CHECK(!zero.allow(false));
  sshos::RestartBudget negatif(-1);
  CHECK(!negatif.allow(false));
  // Mais une session qui a servi reste une session qui a servi : le plafond
  // ne borne QUE le stérile.
  sshos::RestartBudget utile(0);
  CHECK(utile.allow(true));
}


// --- LE TÉMOIN NE DOIT PAS NAÎTRE MORT ----------------------------------
//
// RestartBudget ne vaut que ce que vaut le booléen qu'on lui donne. Si
// SessionTrace n'était jamais remplie, `fruitful` serait faux à chaque tour
// et le plafond de deux redémarrages reviendrait tel quel -- le défaut
// serait réintroduit sans qu'aucun des cas ci-dessus ne bronche. C'est
// exactement le défaut signature de ce projet (§9 bis du dossier de
// reprise) : un morceau juste, branché sur rien.
//
// Le démon est ici un FAUX démon, tenu par le test : il accepte, lit le
// Hello, peint une trame, attend l'entrée, puis annonce le détachement pour
// mise à jour. Aucun vrai démon n'est nécessaire pour éprouver ce que le
// client observe.

std::string nom_unique() {
  static std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "sshos-restart/" << ::getpid() << '-' << std::hex << dist(rng);
  return os.str();
}

// Récolte inconditionnelle : tous les cas tournent dans le même processus et
// d'autres fichiers appellent waitpid(-1). Un zombie oublié ici casse un
// test ailleurs, une fois sur dix (tests/test_launch.cpp dit la même chose).
class Enfant {
 public:
  explicit Enfant(pid_t pid) : pid_(pid) {}
  ~Enfant() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGKILL);
    int st = 0;
    ::waitpid(pid_, &st, 0);
  }
  Enfant(const Enfant&) = delete;
  Enfant& operator=(const Enfant&) = delete;
  pid_t get() const { return pid_; }
  // Récolte volontaire, pour lire le code de sortie. Le destructeur ne
  // repassera pas dessus.
  int recolter(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; ++i) {
      int st = 0;
      const pid_t r = ::waitpid(pid_, &st, WNOHANG);
      if (r == pid_) {
        pid_ = -1;
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
      }
      ::usleep(10000);
    }
    return -1;
  }

 private:
  pid_t pid_;
};

sshos::Fd accepter(int listen_fd, int timeout_ms) {
  pollfd pfd{listen_fd, POLLIN, 0};
  if (::poll(&pfd, 1, timeout_ms) <= 0) return sshos::Fd();
  const int raw = ::accept(listen_fd, nullptr, nullptr);
  return sshos::Fd(raw);
}

// Lit jusqu'à décoder un message du type demandé, ou rend faux au délai.
template <typename T>
bool attendre_message(int fd, sshos::Decoder& dec, int timeout_ms) {
  std::string buf(4096, '\0');
  for (int reste = timeout_ms; reste > 0; reste -= 50) {
    while (auto m = dec.next()) {
      if (std::get_if<T>(&*m) != nullptr) return true;
    }
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 50) <= 0) continue;
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) return false;
    dec.feed(std::string_view(buf.data(), static_cast<size_t>(n)));
  }
  return false;
}

TEST(run_client_rapporte_ce_que_la_session_a_produit) {
  const std::string nom = nom_unique();
  sshos::Fd ecouteur;
  try {
    ecouteur = sshos::bind_abstract(nom);
  } catch (const std::exception&) {
    REQUIRE(false);
    return;
  }

  // L'entrée du client est un tube : TtyGuard échoue son tcgetattr() dessus
  // et reste désarmé, donc rien n'est écrit vers ce tube et le terminal du
  // lanceur de tests n'est pas touché (même raison qu'à test_session.cpp).
  int tube[2] = {-1, -1};
  REQUIRE(::pipe(tube) == 0);

  Enfant client(::fork());
  REQUIRE(client.get() >= 0);
  if (client.get() == 0) {
    ::dup2(tube[0], STDIN_FILENO);
    ::close(tube[1]);
    // Les trames iraient sinon polluer la sortie de la suite.
    const int nul = ::open("/dev/null", O_WRONLY);
    if (nul >= 0) ::dup2(nul, STDOUT_FILENO);
    sshos::SessionTrace t;
    const int rc = sshos::run_client(nom, &t);
    ::_exit((rc == sshos::kClientRestartRequested ? 4 : 0) |
            (t.desktop_shown ? 1 : 0) | (t.user_acted ? 2 : 0));
  }
  ::close(tube[0]);

  sshos::Fd pair = accepter(ecouteur.get(), 3000);
  REQUIRE(pair.valid());

  sshos::Decoder dec;
  REQUIRE(attendre_message<sshos::Hello>(pair.get(), dec, 3000));

  // Le bureau s'affiche...
  const std::string trame = sshos::encode(sshos::Msg{sshos::FrameMsg{"\033[H"}});
  REQUIRE(::write(pair.get(), trame.data(), trame.size()) > 0);

  // ...l'utilisateur agit. On attend que l'entrée soit VRAIMENT parvenue
  // avant d'annoncer le détachement : sans cette attente, le test mesurerait
  // une course au lieu d'un contrat.
  REQUIRE(::write(tube[1], "x", 1) == 1);
  REQUIRE(attendre_message<sshos::Input>(pair.get(), dec, 3000));

  const std::string fin =
      sshos::encode(sshos::Msg{sshos::Detached{sshos::kDetachReasonUpdate}});
  REQUIRE(::write(pair.get(), fin.data(), fin.size()) > 0);

  const int code = client.recolter(5000);
  ::close(tube[1]);

  // 4 = redémarrage demandé, 2 = l'utilisateur a agi, 1 = le bureau s'est
  // affiché. Les trois, ou le budget de redémarrage juge à l'aveugle.
  CHECK_EQ(code, 7);
}

}  // namespace
