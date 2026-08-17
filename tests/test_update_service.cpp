#include "shell/update_service.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "harness.hpp"

using sshos::UpdateEntry;
using sshos::UpdateService;

namespace {

// Le même instant mural partout : « maintenant » ne doit jamais venir de
// l'horloge réelle, sinon les cas de bornage dépendent du moment où la
// suite tourne.
constexpr std::int64_t kWall = 1755400000;
constexpr std::int64_t kDay = 86400;

// Horloge injectable. Le mural et le monotone avancent séparément : c'est
// exactement la distinction que platform.hpp impose, et un service qui les
// confondrait passerait ce test s'ils bougeaient ensemble.
struct FakePlatform : sshos::Platform {
  std::int64_t wall = kWall;
  std::chrono::steady_clock::duration mono{};

  std::chrono::system_clock::time_point now() const override {
    return std::chrono::system_clock::time_point(std::chrono::seconds(wall));
  }
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point(mono);
  }
  std::string read_file(std::string_view) const override { return {}; }

  void advance_mono(std::chrono::seconds s) { mono += s; }
};

// Même construction de chemin que tests/test_daemonize.cpp : deux suites
// lancées en parallèle sur un même /tmp ne doivent pas se marcher dessus.
std::string temp_path(const char* suffix) {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream os;
  os << "/tmp/sshos-test-" << ::getpid() << '-' << std::hex << dist(rng) << '-'
     << suffix;
  return os.str();
}

// Écrit un fichier d'état jetable et rend son chemin. L'appelant l'efface.
std::string write_state(const std::string& body) {
  const std::string p = temp_path("state");
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << body;
  return p;
}

// Un lanceur qui n'exécute rien : il note ce qu'on lui a demandé et rend un
// pid choisi. Aucun test de ce fichier ne forke pour de bon.
struct FakeLauncher {
  std::vector<std::vector<std::string>> calls;
  pid_t next_pid = 4242;

  UpdateService::Launcher fn() {
    return [this](const std::vector<std::string>& argv) {
      calls.push_back(argv);
      return next_pid;
    };
  }
};

// Un pid dont on est SÛR qu'il est mort : on en fabrique un et on le
// récolte. Un numéro inventé pourrait appartenir à un vrai processus.
pid_t a_dead_pid() {
  const pid_t p = ::fork();
  if (p == 0) ::_exit(0);
  int st = 0;
  ::waitpid(p, &st, 0);
  return p;
}

const std::string kPrefix = "/home/u/.local";

std::string state_body(const std::string& status, const std::string& extra = "") {
  return "schema=1\nprefix=" + kPrefix + "\nsource=git\nstatus=" + status + "\n" +
         extra;
}

}  // namespace

// LE LIBELLÉ ET L'INERTIE POUR CHACUN DES SEPT ÉTATS. MenuItem n'a pas de
// quoi dire « inerte » ; c'est le service qui le porte, et c'est ce qui rend
// le cas testable ici plutôt que seulement à travers la session.
TEST(update_service_labels_every_state) {
  struct Row {
    const char* status;
    const char* label;
    bool enabled;
    bool badge;
    const char* id;
  };
  const Row rows[] = {
      {"idle", "Verifier les mises a jour", true, false, "update:check"},
      {"checking", "Verification en cours...", false, false, "update:check"},
      {"up-to-date", "Verifier les mises a jour", true, false, "update:check"},
      {"available", "Mettre a jour", true, true, "update:apply"},
      {"applying", "Mise a jour en cours...", false, false, "update:apply"},
      {"restart-pending", "Redemarrer pour terminer", true, true, "update:restart"},
      {"check-failed", "Verifier les mises a jour", true, false, "update:check"},
      {"apply-failed", "Verifier les mises a jour", true, false, "update:check"},
  };
  for (const Row& r : rows) {
    FakePlatform plat;
    FakeLauncher launcher;
    // Un pid vivant pour que « checking » et « applying » soient vraiment
    // en cours : un pid mort les ferait basculer en échec, ce qui est
    // l'objet d'un autre cas.
    const std::string body =
        state_body(r.status, "pid=" + std::to_string(::getpid()) + "\n");
    const std::string path = write_state(body);
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();

    const UpdateEntry e = svc.entry();
    CHECK_EQ(e.label, std::string(r.label));
    CHECK_EQ(e.enabled, r.enabled);
    CHECK_EQ(e.id, std::string(r.id));
    CHECK_EQ(svc.badge(), r.badge);
    std::remove(path.c_str());
  }
}

// UN COMMIT INCONNU NE SE COMPARE À RIEN, et on ne prétend pas le
// contraire : l'entrée propose de réinstaller, pas de mettre à jour.
TEST(update_service_offers_a_reinstall_when_the_installed_commit_is_unknown) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path =
      write_state(state_body("up-to-date", "installed_commit=unknown\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.entry().label, std::string("Reinstaller depuis GitHub"));
  CHECK(svc.entry().enabled);
  CHECK_EQ(svc.entry().id, std::string("update:apply"));
  std::remove(path.c_str());
}

// UN HISTORIQUE RÉÉCRIT N'EST PAS UNE MISE À JOUR. Ce dépôt a force-poussé
// `main` deux fois : proposer « Mettre a jour » vers un historique sans
// relation avec celui en place serait un mensonge.
TEST(update_service_offers_a_reinstall_when_history_was_rewritten) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("history-rewritten"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.entry().label, std::string("Reinstaller depuis GitHub"));
  CHECK(svc.entry().enabled);
  std::remove(path.c_str());
}

// FAIRE SEMBLANT DE VÉRIFIER SERAIT LE PIRE COMPORTEMENT POSSIBLE :
// l'utilisateur se croirait à jour. Quand la mise à jour est impossible,
// l'entrée le dit et ne fait rien.
TEST(update_service_says_so_when_updates_are_disabled) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(
      state_body("updates-disabled", "message=git absent\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.entry().label, std::string("Mise a jour indisponible (git absent)"));
  CHECK(!svc.entry().enabled);
  CHECK(!svc.badge());
  // Et aucune échéance : on ne va pas réveiller le démon pour rien.
  CHECK_EQ(svc.delay_ms(), -1);
  std::remove(path.c_str());
}

// UN TRAVAIL DONT LE PID EST MORT EST UN ÉCHEC, PAS UN TRAVAIL EN COURS.
// C'est le cas du démon redémarré pendant une application : sans cette
// règle, l'entrée resterait inerte à vie.
TEST(update_service_treats_a_dead_worker_pid_as_a_failure) {
  FakePlatform plat;
  FakeLauncher launcher;
  const pid_t dead = a_dead_pid();
  const std::string path =
      write_state(state_body("applying", "pid=" + std::to_string(dead) + "\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK(svc.entry().enabled);
  CHECK_EQ(svc.entry().label, std::string("Verifier les mises a jour"));
  CHECK(svc.message().find("interrompu") != std::string::npos);
  std::remove(path.c_str());
}

// PAS DE VÉRIFICATION DANS LES TRENTE PREMIÈRES SECONDES. Ouvrir le bureau
// ne doit pas déclencher un accès réseau, même quand la dernière
// vérification est très ancienne.
TEST(update_service_waits_thirty_seconds_before_its_first_check) {
  FakePlatform plat;
  FakeLauncher launcher;
  // checked_at très ancien : la vérification est due depuis longtemps.
  const std::string path =
      write_state(state_body("up-to-date", "checked_at=1\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.delay_ms(), 30 * 1000);

  plat.advance_mono(std::chrono::seconds(29));
  svc.tick();
  CHECK_EQ(svc.delay_ms(), 1000);
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(0));

  plat.advance_mono(std::chrono::seconds(1));
  svc.tick();
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(1));
  std::remove(path.c_str());
}

// LE RELIQUAT EST BORNÉ À [30 s, 24 h]. `checked_at` est une heure MURALE
// écrite par un script ; l'échéance, elle, court sur steady_now(), parce que
// platform.hpp pose que tout ce qui expire dans ce projet s'y mesure.
TEST(update_service_clamps_the_remaining_delay_to_a_day) {
  {  // vérifié il y a une heure : il reste vingt-trois heures
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(
        state_body("up-to-date",
                   "checked_at=" + std::to_string(kWall - 3600) + "\n"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    CHECK_EQ(svc.delay_ms(), static_cast<int>((kDay - 3600) * 1000));
    std::remove(path.c_str());
  }
  {  // vérifié « dans le futur » : l'analyseur a déjà ramené checked_at à 0,
     // donc la vérification est due -- et le plancher de trente secondes
     // s'applique, jamais deux jours d'attente.
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(
        state_body("up-to-date",
                   "checked_at=" + std::to_string(kWall + 999999) + "\n"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    CHECK_EQ(svc.delay_ms(), 30 * 1000);
    std::remove(path.c_str());
  }
}

// UN CLIC SUR UNE ENTRÉE INERTE NE LANCE RIEN. La garde ne repose pas sur
// le libellé ni sur l'interface : le service revoit son état avant d'agir.
TEST(update_service_refuses_to_run_while_a_child_is_alive) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(
      state_body("applying", "pid=" + std::to_string(::getpid()) + "\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:apply");
  svc.run("update:check");
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(0));
  std::remove(path.c_str());
}

// LE LANCEUR REÇOIT LE BON SCRIPT ET LE BON MODE. Le chemin se déduit du
// préfixe inscrit dans l'état : c'est l'installation qui sait où elle est.
TEST(update_service_launches_the_updater_with_the_right_mode) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:apply");
  REQUIRE_EQ(launcher.calls.size(), static_cast<std::size_t>(1));
  CHECK_EQ(launcher.calls[0][0], kPrefix + "/libexec/sshos-update");
  CHECK_EQ(launcher.calls[0][1], std::string("--apply"));

  // Tant que l'enfant vit, on ne relance pas.
  svc.run("update:apply");
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(1));
  std::remove(path.c_str());
}

TEST(update_service_launches_a_check_from_the_menu) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("up-to-date"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:check");
  REQUIRE_EQ(launcher.calls.size(), static_cast<std::size_t>(1));
  CHECK_EQ(launcher.calls[0][1], std::string("--check"));
  std::remove(path.c_str());
}

// L'ENFANT EST MORT SANS RIEN CHANGER : c'est un échec, pas un succès
// silencieux. Sans cette règle, rien ne distingue « rien fait » de « fait ».
TEST(update_service_fails_when_the_child_dies_without_touching_the_state) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:apply");
  CHECK(svc.owns(launcher.next_pid));
  svc.on_child_exit(launcher.next_pid, 1 << 8);  // sorti avec 1

  CHECK(svc.message().find("echec") != std::string::npos);
  CHECK(svc.entry().enabled);
  std::remove(path.c_str());
}

// UN ENFANT QU'ON N'A PAS LANCÉ NE NOUS CONCERNE PAS. Le récolteur du démon
// est unique et voit mourir tous les enfants du processus.
TEST(update_service_ignores_a_child_exit_for_a_pid_it_did_not_launch) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK(!svc.owns(9999));
  svc.on_child_exit(9999, 0);
  CHECK_EQ(svc.entry().label, std::string("Mettre a jour"));
  CHECK(svc.message().empty());
  std::remove(path.c_str());
}

// UN FICHIER D'ÉTAT ABSENT N'EST PAS UNE ERREUR : c'est le tout premier
// lancement, avant toute installation.
TEST(update_service_starts_idle_when_there_is_no_state_file) {
  FakePlatform plat;
  FakeLauncher launcher;
  UpdateService svc(plat, "/tmp/sshos-test-ce-fichier-n-existe-pas", launcher.fn());
  svc.tick();

  CHECK_EQ(svc.entry().label, std::string("Verifier les mises a jour"));
  CHECK(!svc.badge());
  CHECK(svc.message().empty());
}

// SANS PRÉFIXE CONNU, ON NE LANCE RIEN. Un état sans `prefix` ne dit pas où
// vit le script de mise à jour, et inventer un chemin serait pire que de ne
// rien faire.
TEST(update_service_refuses_to_run_without_a_known_prefix) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state("schema=1\nstatus=available\n");
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:apply");
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(0));
  std::remove(path.c_str());
}

// LE REDÉMARRAGE EST UNE DEMANDE, PAS UN LANCEMENT. Il ne passe par aucun
// script : c'est la session qui ferme le bureau.
TEST(update_service_asks_for_a_restart_without_launching_anything) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("restart-pending"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK(!svc.wants_restart());
  svc.run("update:restart");
  CHECK(svc.wants_restart());
  CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(0));
  std::remove(path.c_str());
}

// LE REDÉMARRAGE NE SE DEMANDE QUE QUAND IL Y A QUELQUE CHOSE À TERMINER.
// L'identifiant peut arriver depuis un menu construit un instant plus tôt,
// alors que l'état a changé entre-temps : fermer le bureau de l'utilisateur
// sur un ordre périmé serait le pire des bogues de cette fonctionnalité.
TEST(update_service_refuses_a_restart_outside_of_restart_pending) {
  const char* const kStates[] = {"available", "up-to-date", "idle",
                                 "check-failed"};
  for (const char* st : kStates) {
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(state_body(st));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();

    svc.run("update:restart");
    CHECK(!svc.wants_restart());
    CHECK_EQ(launcher.calls.size(), static_cast<std::size_t>(0));
    std::remove(path.c_str());
  }
}

// UN FICHIER D'ÉTAT TROP GROS EST UN FICHIER ABSENT, jusqu'au bout de la
// chaîne. L'analyseur a son propre cas pour le plafond ; celui-ci vérifie
// que le service, qui lit le fichier pour de vrai, aboutit au même verdict.
TEST(update_service_treats_an_oversized_state_file_as_absent) {
  FakePlatform plat;
  FakeLauncher launcher;
  std::string body = state_body("available") + "message=";
  body.append(sshos::kMaxStateBytes, 'x');
  const std::string path = write_state(body);
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.entry().label, std::string("Verifier les mises a jour"));
  CHECK(!svc.badge());
  std::remove(path.c_str());
}

// SANS INSTALLATION, PAS D'ÉCHÉANCE. C'est le cas de l'arbre de
// développement : ni préfixe, ni script. Sans cette règle, le démon se
// réveillerait toutes les trente secondes pour constater qu'il n'a rien à
// lancer -- un réveil par minute que personne n'a demandé.
TEST(update_service_schedules_nothing_without_an_installation) {
  {  // aucun fichier d'état du tout
    FakePlatform plat;
    FakeLauncher launcher;
    UpdateService svc(plat, "/tmp/sshos-test-il-n-y-a-rien-ici", launcher.fn());
    svc.tick();
    CHECK_EQ(svc.delay_ms(), -1);
  }
  {  // un état sans préfixe : on ne sait pas où vit le script
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state("schema=1\nstatus=up-to-date\n");
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    CHECK_EQ(svc.delay_ms(), -1);
    std::remove(path.c_str());
  }
  {  // avec un préfixe, l'échéance existe
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(state_body("up-to-date"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    CHECK(svc.delay_ms() > 0);
    std::remove(path.c_str());
  }
}
