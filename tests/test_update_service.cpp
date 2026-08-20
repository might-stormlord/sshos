#include "shell/update_service.hpp"

#include <signal.h>
#include <sys/stat.h>
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

// UNE VERIFICATION DEMANDEE REPOND. L'utilisateur a clique ; le menu s'est
// referme au moment du clic, donc « rien ne se passe » est ce qu'il verrait
// sans cela -- et c'est la pire des reponses.
TEST(update_service_reports_the_result_of_a_manual_check) {
  struct Row { const char* status; const char* extra; const char* needle; };
  const Row rows[] = {
      {"up-to-date",        "",                       "a jour"},
      {"available",         "",                       "disponible"},
      {"restart-pending",   "",                       "Redemarrez"},
      {"history-rewritten", "",                       "reinstallation"},
      {"check-failed",      "message=pas de reseau\n", "pas de reseau"},
      {"apply-failed",      "message=tests rouges\n",  "tests rouges"},
  };
  for (const Row& r : rows) {
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(state_body("available"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();

    svc.run("update:check");
    CHECK(!svc.has_report());  // rien tant que l'enfant travaille

    // Le script a fait son travail : on reecrit l'etat sous ses pieds,
    // comme le vrai le ferait.
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << state_body(r.status, r.extra);
    }
    svc.on_child_exit(launcher.next_pid, 0);

    REQUIRE(svc.has_report());
    const std::string report = svc.take_report();
    CHECK(report.find(r.needle) != std::string::npos);
    // Pris une fois, puis disparu : sans quoi le meme pop-up reviendrait a
    // chaque reveil.
    CHECK(!svc.has_report());
    std::remove(path.c_str());
  }
}

// UNE VERIFICATION AUTOMATIQUE SE TAIT. Elle tombe une fois par jour, sans
// qu'on ait rien demande : un pop-up quotidien serait du harcelement.
TEST(update_service_says_nothing_after_an_automatic_check) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path =
      write_state(state_body("up-to-date", "checked_at=1\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  // On laisse l'echeance echoir : le service lance de lui-meme.
  plat.advance_mono(std::chrono::seconds(31));
  svc.tick();
  REQUIRE_EQ(launcher.calls.size(), static_cast<std::size_t>(1));
  CHECK_EQ(launcher.calls[0][1], std::string("--check"));

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << state_body("available");
  }
  svc.on_child_exit(launcher.next_pid, 0);

  CHECK(!svc.has_report());
  // Mais la pastille, elle, s'allume : c'est le signal discret prevu pour ca.
  CHECK(svc.badge());
  std::remove(path.c_str());
}

// UN ENFANT MORT SANS RIEN CONCLURE, DEMANDE PAR L'UTILISATEUR : on le dit
// quand meme, avec le journal comme piste.
TEST(update_service_reports_a_manual_check_that_concluded_nothing) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("up-to-date"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:check");
  svc.on_child_exit(launcher.next_pid, 1 << 8);  // sorti avec 1, etat inchange

  REQUIRE(svc.has_report());
  CHECK(svc.take_report().find("update.log") != std::string::npos);
  std::remove(path.c_str());
}

// « 1.12 -> 1.13 » PLUTOT QUE « cce9d11 -> 3512ffe ». Les empreintes ne
// disent rien a personne ; elles ne servent que de repli quand les numeros
// manquent, et on n'en invente jamais.
TEST(update_service_prefers_version_numbers_over_commit_hashes) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body(
      "available",
      "installed_commit=cce9d11e28ffaea5255fdcde2fb509446c3b899b\n"
      "remote_commit=3512ffefe23fe03e53708324d1b955876926b91c\n"
      "installed_version=1.12\nremote_version=1.13\ncommits_ahead=7\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.news(), std::string("7 nouveautes"));
  CHECK_EQ(svc.version_line(), std::string("Version 1.12 -> 1.13"));
  std::remove(path.c_str());
}

TEST(update_service_falls_back_to_short_hashes_without_versions) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body(
      "available",
      "installed_commit=cce9d11e28ffaea5255fdcde2fb509446c3b899b\n"
      "remote_commit=3512ffefe23fe03e53708324d1b955876926b91c\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.version_line(), std::string("cce9d11 -> 3512ffe"));
  // Sans compte, on ne dit pas « 0 nouveaute » : on ne dit rien.
  CHECK(svc.news().empty());
  std::remove(path.c_str());
}

// LE SINGULIER SE DIT AU SINGULIER. Une seule nouveaute annoncee comme
// « 1 nouveautes » fait bacle.
TEST(update_service_says_one_novelty_in_the_singular) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path =
      write_state(state_body("available", "commits_ahead=1\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();
  CHECK_EQ(svc.news(), std::string("1 nouveaute"));
  std::remove(path.c_str());
}

TEST(update_service_says_nothing_about_versions_when_it_knows_nothing) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();
  CHECK(svc.news().empty());
  CHECK(svc.version_line().empty());
  std::remove(path.c_str());
}

// « IL FAUT REDEMARRER » CONTRE « ON A REDEMARRE ». Le script ecrit
// restart-pending et ne peut pas savoir quand le redemarrage a eu lieu.
// Sans cette lecture, la pastille restait allumee APRES le redemarrage et
// cliquer redemandait de redemarrer -- indefiniment.
TEST(update_service_clears_restart_pending_once_it_runs_the_installed_binary) {
  FakePlatform plat;
  FakeLauncher launcher;

  // On fabrique un prefixe avec un « binaire » pose, et on fait croire au
  // service que c'est LUI qui tourne.
  const std::string dir = temp_path("prefixe");
  ::mkdir(dir.c_str(), 0700);
  ::mkdir((dir + "/libexec").c_str(), 0700);
  const std::string exe = dir + "/libexec/sshos";
  { std::ofstream out(exe); out << "binaire"; }

  const std::string body =
      "schema=1\nprefix=" + dir + "\nsource=git\nstatus=restart-pending\n";
  const std::string path = write_state(body);

  {  // le demon EST le binaire pose : le redemarrage a eu lieu
    UpdateService svc(plat, path, launcher.fn(), exe);
    svc.tick();
    CHECK(svc.running_is_installed());
    CHECK(!svc.badge());
    CHECK_EQ(svc.entry().label, std::string("Verifier les mises a jour"));
  }
  {  // le demon tourne encore sur autre chose : il faut bien redemarrer
    const std::string autre = dir + "/libexec/sshos.previous";
    { std::ofstream out(autre); out << "ancien"; }
    UpdateService svc(plat, path, launcher.fn(), autre);
    svc.tick();
    CHECK(!svc.running_is_installed());
    CHECK(svc.badge());
    CHECK_EQ(svc.entry().label, std::string("Redemarrer pour terminer"));
    ::remove(autre.c_str());
  }
  {  // binaire pose absent : on ne conclut pas, on garde l'etat du fichier
    ::remove(exe.c_str());
    UpdateService svc(plat, path, launcher.fn(), exe);
    svc.tick();
    CHECK(!svc.running_is_installed());
    CHECK_EQ(svc.entry().label, std::string("Redemarrer pour terminer"));
  }

  std::remove(path.c_str());
  ::rmdir((dir + "/libexec").c_str());
  ::rmdir(dir.c_str());
}

// « A JOUR » TOUT COURT NE DIT PAS A JOUR DE QUOI. C'est la seule occasion
// ou l'utilisateur demande explicitement a savoir ou il en est.
TEST(update_service_names_the_version_when_it_says_you_are_up_to_date) {
  {
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path =
        write_state(state_body("up-to-date", "installed_version=1.2\n"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    svc.run("update:check");
    svc.on_child_exit(launcher.next_pid, 0);
    REQUIRE(svc.has_report());
    CHECK(svc.take_report().find("version 1.2") != std::string::npos);
    std::remove(path.c_str());
  }
  {  // sans numero connu, on ne l'invente pas
    FakePlatform plat;
    FakeLauncher launcher;
    const std::string path = write_state(state_body("up-to-date"));
    UpdateService svc(plat, path, launcher.fn());
    svc.tick();
    svc.run("update:check");
    svc.on_child_exit(launcher.next_pid, 0);
    REQUIRE(svc.has_report());
    const std::string r = svc.take_report();
    CHECK(r.find("a jour") != std::string::npos);
    CHECK(r.find("version") == std::string::npos);
    std::remove(path.c_str());
  }
}

// ---------------------------------------------------------------------------
// Les notes de version.
// ---------------------------------------------------------------------------

// « 5 nouveautes » ne dit pas LESQUELLES, et c'est precisement ce que
// l'utilisateur veut savoir avant de lancer une compilation de deux minutes.
TEST(update_service_lists_what_changed_in_its_report) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:check");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << state_body("available",
                      "commits_ahead=3\n"
                      "installed_version=1.4\n"
                      "remote_version=1.9\n"
                      "note_1=le terminal s ouvre chez vous\n"
                      "note_2=le demon survit au tueur de memoire\n"
                      "note_3=la molette atteint enfin l application\n");
  }
  svc.on_child_exit(launcher.next_pid, 0);

  REQUIRE(svc.has_report());
  const std::string report = svc.take_report();
  CHECK(report.find("1.4 -> 1.9") != std::string::npos);
  CHECK(report.find("le terminal s ouvre chez vous") != std::string::npos);
  CHECK(report.find("le demon survit au tueur de memoire") != std::string::npos);
  CHECK(report.find("la molette atteint enfin l application") != std::string::npos);
  std::remove(path.c_str());
}

// SANS NOTES, LE POP-UP EST CELUI D'AVANT, au caractere pres : une
// installation mise a jour par un script plus ancien n'en depose aucune, et
// ce n'est pas une raison pour lui montrer une liste vide ou un cadre troue.
TEST(update_service_says_nothing_more_without_notes) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body("available"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  svc.run("update:check");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << state_body("available",
                      "commits_ahead=3\ninstalled_version=1.4\nremote_version=1.9\n");
  }
  svc.on_child_exit(launcher.next_pid, 0);

  REQUIRE(svc.has_report());
  const std::string report = svc.take_report();
  CHECK_EQ(report, std::string("Mise a jour disponible : 3 nouveautes.\nVersion 1.4 -> 1.9"));
  std::remove(path.c_str());
}


// --- la progression chiffree ---------------------------------------------
//
// Le service ne CALCULE rien : cmake ecrit deja son propre pourcentage et la
// suite de tests une ligne par cas, or ni l'un ni l'autre n'est a portee du
// demon. Le script compte, le service lit -- exactement le partage des
// numeros de version.

TEST(update_service_reports_the_progress_the_script_wrote) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(
      state_body("applying", "stage=compilation\nprogress=47\npid=1\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.progress_percent(), 47);
  ::unlink(path.c_str());
}

// RIEN NE TRAVAILLE, RIEN A MESURER. Un chiffre qui survivrait a la fin du
// travail ferait dessiner une barre sous un constat.
TEST(update_service_reports_no_progress_when_nothing_is_running) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path =
      write_state(state_body("up-to-date", "progress=47\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.progress_percent(), -1);
  ::unlink(path.c_str());
}

// UN TRAVAIL INTERROMPU N'EST PAS UN TRAVAIL EN COURS. Le demon a redemarre
// pendant l'application : l'etat dit encore « applying » avec un
// pourcentage, mais le pid est mort. Meme regle que progress_line().
TEST(update_service_reports_no_progress_for_an_interrupted_job) {
  FakePlatform plat;
  FakeLauncher launcher;
  const std::string path = write_state(state_body(
      "applying", "stage=compilation\nprogress=47\npid=" +
                      std::to_string(static_cast<int>(a_dead_pid())) + "\n"));
  UpdateService svc(plat, path, launcher.fn());
  svc.tick();

  CHECK_EQ(svc.progress_percent(), -1);
  ::unlink(path.c_str());
}
