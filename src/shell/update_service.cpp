#include "shell/update_service.hpp"

#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <utility>

namespace sshos {
namespace {

// Un jour entre deux vérifications, et jamais moins de trente secondes après
// l'ouverture du bureau : un accès réseau au démarrage se paie sur le temps
// d'affichage de la première trame.
constexpr std::int64_t kPeriodSeconds = 86400;
constexpr std::int64_t kFloorSeconds = 30;

// Vrai si ce pid désigne encore un processus vivant. `kill(pid, 0)` ne
// signale rien : il ne fait que le test de permission et d'existence.
bool pid_alive(pid_t pid) {
  if (pid <= 0) return false;
  if (::kill(pid, 0) == 0) return true;
  return errno != ESRCH;  // EPERM = vivant, mais pas à nous
}

// Lit au plus kMaxStateBytes octets. Le plafond est appliqué À LA LECTURE et
// non après : lire un fichier d'un gigaoctet pour ensuite décider qu'il est
// trop gros annulerait la raison d'être du plafond, puisque la lecture se
// fait dans le fil unique du démon.
//
// ÉQUIVALENCE DÉCLARÉE (campagne de mutation, M11) : agrandir ce tampon ne
// change AUCUN verdict -- l'analyseur rejette de toute façon tout ce qui
// dépasse kMaxStateBytes. Ce que le plafond borne, c'est la mémoire et le
// temps passés dans la boucle du démon, ce qu'aucun test unitaire ne peut
// observer sans fabriquer un fichier d'un gigaoctet.
std::string read_capped(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string buf(kMaxStateBytes + 1, '\0');
  in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
  buf.resize(static_cast<std::size_t>(in.gcount()));
  return buf;
}

}  // namespace

UpdateService::UpdateService(const Platform& plat, std::string state_path,
                             Launcher launch)
    : plat_(&plat), state_path_(std::move(state_path)), launch_(std::move(launch)) {}

std::string UpdateService::updater_path() const {
  if (state_.prefix.empty()) return {};
  return state_.prefix + "/libexec/sshos-update";
}

void UpdateService::reload() {
  const std::int64_t wall =
      std::chrono::duration_cast<std::chrono::seconds>(
          plat_->now().time_since_epoch())
          .count();
  state_ = parse_update_state(read_capped(state_path_), wall);

  // Un travail annoncé « en cours » dont le pid n'existe plus est un travail
  // INTERROMPU, pas un travail qui court : c'est le cas du démon redémarré
  // pendant une application. Sans cette lecture, l'entrée resterait inerte à
  // vie et plus personne ne pourrait relancer quoi que ce soit.
  stale_worker_ = false;
  const bool in_progress = state_.status == UpdateStatus::Checking ||
                           state_.status == UpdateStatus::Applying;
  if (in_progress && child_ < 0 && !pid_alive(state_.pid)) {
    stale_worker_ = true;
    message_ = "mise a jour interrompue";
  } else if (!in_progress) {
    message_ = state_.message;
  }
}

void UpdateService::schedule_from(std::int64_t checked_at) {
  const std::int64_t wall =
      std::chrono::duration_cast<std::chrono::seconds>(
          plat_->now().time_since_epoch())
          .count();
  // `checked_at` est une heure MURALE écrite par un script ; elle ne sert
  // qu'ICI, une seule fois, à calculer un reliquat. L'échéance elle-même
  // court sur steady_now(), parce que platform.hpp pose que tout ce qui
  // expire dans ce projet s'y mesure -- un ajustement d'horloge ne doit pas
  // faire attendre un jour de plus ni vérifier en boucle.
  const std::int64_t elapsed = checked_at > 0 ? wall - checked_at : kPeriodSeconds;
  // ÉQUIVALENCE DÉCLARÉE (campagne de mutation, M2) : le bornage de `left`
  // est aujourd'hui inatteignable. parse_update_state ramène déjà
  // `checked_at` dans [0, maintenant], donc `elapsed` est positif et
  // `kPeriodSeconds - elapsed` ne peut pas dépasser kPeriodSeconds ; et la
  // borne basse est couverte par le std::max qui suit. Le clamp est gardé
  // comme filet : il rend cette fonction juste toute seule, sans dépendre
  // d'une garantie prise ailleurs, et le jour où l'analyseur cesserait de
  // borner, c'est ici que le bogue serait rattrapé.
  const std::int64_t left = std::clamp(kPeriodSeconds - elapsed,
                                       static_cast<std::int64_t>(0), kPeriodSeconds);
  const std::int64_t delay = std::max(left, kFloorSeconds);
  deadline_ = plat_->steady_now() + std::chrono::seconds(delay);
  have_deadline_ = true;
}

void UpdateService::tick() {
  reload();

  // Rien à attendre quand la mise à jour est impossible, quand un enfant
  // travaille déjà, ou quand il n'y a tout simplement pas d'installation --
  // le cas de l'arbre de développement, qui n'a ni préfixe ni script.
  //
  // Sans cette dernière condition, un démon sans installation se réveillerait
  // toutes les trente secondes pour constater qu'il n'a rien à lancer, et
  // repartirait pour trente secondes. Ce n'est pas du scrutin actif, mais
  // c'est un réveil par minute que personne n'a demandé.
  if (state_.status == UpdateStatus::UpdatesDisabled || child_ > 0 ||
      updater_path().empty()) {
    have_deadline_ = false;
    return;
  }

  if (!loaded_) {
    schedule_from(state_.checked_at);
    loaded_ = true;
    return;
  }
  if (!have_deadline_) schedule_from(state_.checked_at);

  if (plat_->steady_now() >= deadline_) {
    // L'échéance est échue : on vérifie, et on repart pour un tour même si
    // le lancement échoue -- sinon un échec de fork figerait la vérification
    // pour toujours.
    have_deadline_ = false;
    // Automatique : silencieuse. C'est la regle -- une verification qui
    // echoue toute seule ne doit pas harceler.
    launch("--check", /*manual=*/false);
    if (child_ < 0) schedule_from(0);
  }
}

int UpdateService::delay_ms() const {
  if (!have_deadline_) return -1;
  const auto left = deadline_ - plat_->steady_now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
  if (ms <= 0) return 0;
  return static_cast<int>(ms);
}

UpdateEntry UpdateService::entry() const {
  // Un travail en cours dont le pid est mort n'est pas un travail en cours.
  const bool working =
      child_ > 0 || (!stale_worker_ && (state_.status == UpdateStatus::Checking ||
                                        state_.status == UpdateStatus::Applying));

  if (state_.status == UpdateStatus::UpdatesDisabled) {
    std::string why = state_.message.empty() ? std::string("raison inconnue")
                                             : state_.message;
    return {"Mise a jour indisponible (" + why + ")", false, "update:check"};
  }
  if (working) {
    if (state_.status == UpdateStatus::Applying) {
      return {"Mise a jour en cours...", false, "update:apply"};
    }
    return {"Verification en cours...", false, "update:check"};
  }
  if (state_.status == UpdateStatus::RestartPending) {
    return {"Redemarrer pour terminer", true, "update:restart"};
  }
  // Aucune comparaison n'est possible quand on ne sait pas d'où vient
  // l'installation, et un historique réécrit n'a pas d'ancêtre commun : dans
  // les deux cas la seule sortie honnête est une réinstallation.
  if (state_.status == UpdateStatus::HistoryRewritten ||
      state_.installed_commit == "unknown") {
    return {"Reinstaller depuis GitHub", true, "update:apply"};
  }
  if (state_.status == UpdateStatus::Available) {
    return {"Mettre a jour", true, "update:apply"};
  }
  return {"Verifier les mises a jour", true, "update:check"};
}

bool UpdateService::badge() const {
  const UpdateEntry e = entry();
  return e.enabled && (e.id == "update:apply" || e.id == "update:restart") &&
         (state_.status == UpdateStatus::Available ||
          state_.status == UpdateStatus::RestartPending);
}

std::string UpdateService::message() const { return message_; }

void UpdateService::run(std::string_view id) {
  const UpdateEntry e = entry();
  if (!e.enabled) return;
  // ÉQUIVALENCE DÉCLARÉE (campagne de mutation, M7) : cette garde est
  // aujourd'hui redondante avec celle du dessus, puisqu'un enfant vivant
  // rend l'entrée inerte dans entry(). Elle est gardée parce qu'elle dit
  // l'invariant à l'endroit où il compte -- « un seul travail à la fois » --
  // au lieu de le faire dépendre du calcul d'un libellé.
  if (child_ > 0) return;

  if (id == "update:restart") {
    // Le redémarrage ne passe par aucun script : c'est la session qui ferme
    // le bureau, et le client qui se rattache.
    if (state_.status == UpdateStatus::RestartPending) wants_restart_ = true;
    return;
  }

  if (id == "update:check") {
    launch("--check", /*manual=*/true);
  } else if (id == "update:apply") {
    launch("--apply", /*manual=*/true);
  }
}

void UpdateService::launch(std::string_view mode, bool manual) {
  const std::string exe = updater_path();
  if (exe.empty()) return;  // on n'invente pas un chemin
  const pid_t pid = launch_({exe, std::string(mode)});
  if (pid > 0) {
    child_ = pid;
    child_is_manual_ = manual;
    have_deadline_ = false;  // rien à attendre tant qu'il travaille
  }
}

namespace {

// Sept caracteres : ce que git montre, et assez pour etre unique en pratique.
std::string short_commit(const std::string& c) {
  if (c.empty() || c == "unknown") return {};
  return c.substr(0, std::min<std::size_t>(7, c.size()));
}

}  // namespace

std::string UpdateService::news() const {
  if (state_.commits_ahead <= 0) return {};
  return std::to_string(state_.commits_ahead) +
         (state_.commits_ahead > 1 ? " nouveautes" : " nouveaute");
}

std::string UpdateService::version_line() const {
  if (!state_.installed_version.empty() && !state_.remote_version.empty()) {
    return "Version " + state_.installed_version + " -> " + state_.remote_version;
  }
  // A defaut de numeros, les empreintes courtes : elles ne disent pas
  // grand-chose, mais elles disent au moins que ce n'est pas la meme.
  const std::string a = short_commit(state_.installed_commit);
  const std::string b = short_commit(state_.remote_commit);
  if (a.empty() || b.empty()) return {};
  return a + " -> " + b;
}

std::string UpdateService::take_report() {
  std::string r;
  r.swap(report_);
  return r;
}

// Le texte est SANS ACCENTS, comme celui de la modale « Fermer la session ».
// Il est aussi borne : le message vient d'un script, et une modale qui
// deborde de son cadre a deja ete un defaut de ce projet.
void UpdateService::build_report() {
  const std::string why = message_.empty() ? std::string("raison inconnue")
                                           : message_.substr(0, 60);
  switch (state_.status) {
    case UpdateStatus::UpToDate:
      report_ = "Vous etes a jour.";
      break;
    case UpdateStatus::Available: {
      report_ = "Mise a jour disponible";
      const std::string n = news();
      if (!n.empty()) report_ += " : " + n;
      report_ += ".";
      const std::string v = version_line();
      if (!v.empty()) report_ += "\n" + v;
      break;
    }
    case UpdateStatus::RestartPending:
      report_ = "Mise a jour installee. Redemarrez pour terminer.";
      break;
    case UpdateStatus::HistoryRewritten:
      report_ = "Historique reecrit : une reinstallation est necessaire.";
      break;
    case UpdateStatus::CheckFailed:
      report_ = "Verification impossible : " + why;
      break;
    case UpdateStatus::ApplyFailed:
      report_ = "Mise a jour echouee : " + why;
      break;
    case UpdateStatus::UpdatesDisabled:
      report_ = "Mise a jour indisponible : " + why;
      break;
    default:
      // L'etat n'a pas bouge : le script est mort sans rien conclure.
      report_ = "Verification sans resultat, voir update.log";
      break;
  }
}

void UpdateService::on_child_exit(pid_t pid, int status) {
  if (!owns(pid)) return;
  child_ = -1;

  const UpdateStatus before = state_.status;
  reload();

  // L'ENFANT EST MORT SANS RIEN CHANGER : c'est un échec, pas un succès
  // silencieux. Sans cette règle, rien ne distinguerait « rien fait » de
  // « fait » -- un script tué au démarrage laisserait l'état d'avant et on
  // le lirait comme un verdict.
  const bool unchanged = state_.status == before;
  const bool failed = status != 0;
  if (unchanged && failed) {
    message_ = "echec de la mise a jour, voir update.log";
  }

  if (child_is_manual_) {
    if (unchanged && failed) {
      report_ = "Echec, voir update.log";
    } else {
      build_report();
    }
  }
  child_is_manual_ = false;

  schedule_from(state_.checked_at);
}

}  // namespace sshos
