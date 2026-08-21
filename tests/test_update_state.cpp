#include "shell/update_state.hpp"

#include <cstdint>
#include <string>

#include "harness.hpp"

using sshos::parse_update_state;
using sshos::UpdateState;
using sshos::UpdateStatus;

namespace {

// Un instant fixe : « maintenant » ne doit jamais venir de l'horloge réelle
// dans un test, sinon le cas « checked_at dans le futur » dépend du moment
// où la suite tourne.
constexpr std::int64_t kNow = 1755400000;

UpdateState parse(const std::string& raw) { return parse_update_state(raw, kNow); }

}  // namespace

TEST(update_state_reads_a_well_formed_file) {
  const UpdateState s = parse(
      "schema=1\n"
      "prefix=/home/u/.local\n"
      "source=git\n"
      "installed_commit=aaaa\n"
      "previous_commit=bbbb\n"
      "remote_commit=cccc\n"
      "checked_at=1755300000\n"
      "status=available\n"
      "pid=\n"
      "message=une ligne\n");

  CHECK(s.status == UpdateStatus::Available);
  CHECK_EQ(s.prefix, std::string("/home/u/.local"));
  CHECK_EQ(s.source, std::string("git"));
  CHECK_EQ(s.installed_commit, std::string("aaaa"));
  CHECK_EQ(s.previous_commit, std::string("bbbb"));
  CHECK_EQ(s.remote_commit, std::string("cccc"));
  CHECK_EQ(s.checked_at, static_cast<std::int64_t>(1755300000));
  CHECK_EQ(s.message, std::string("une ligne"));
}

// UN SCHEMA INCONNU EST UN FICHIER ABSENT. Interpréter au hasard un format
// qu'on ne connaît pas est pire que de ne rien afficher : on montrerait à
// l'utilisateur un état qu'on a deviné.
TEST(update_state_treats_an_unknown_schema_as_absent) {
  const UpdateState s = parse("schema=2\nstatus=available\nmessage=coucou\n");
  CHECK(s.status == UpdateStatus::Idle);
  CHECK(s.message.empty());
}

TEST(update_state_treats_a_missing_schema_as_absent) {
  const UpdateState s = parse("status=available\n");
  CHECK(s.status == UpdateStatus::Idle);
}

TEST(update_state_treats_an_absent_or_empty_file_as_idle) {
  CHECK(parse("").status == UpdateStatus::Idle);
  CHECK(parse("\n\n\n").status == UpdateStatus::Idle);
}

// AU-DELÀ DU PLAFOND, C'EST UN FICHIER ABSENT. Il est lu dans le fil UNIQUE
// du démon : un résumé de compilation de plusieurs mégaoctets y ferait une
// lecture, une allocation et une analyse synchrones dans la boucle
// d'affichage, à chaque relecture.
TEST(update_state_refuses_a_file_over_the_cap) {
  std::string raw = "schema=1\nstatus=available\nmessage=";
  raw.append(sshos::kMaxStateBytes, 'x');
  CHECK(parse(raw).status == UpdateStatus::Idle);
}

// LA PREMIÈRE OCCURRENCE GAGNE. Une ligne ajoutée après coup -- par un
// message d'erreur mal assaini qui contiendrait un retour à la ligne -- ne
// doit pas pouvoir écraser une valeur déjà lue.
TEST(update_state_lets_the_first_occurrence_of_a_key_win) {
  const UpdateState s =
      parse("schema=1\nstatus=apply-failed\nstatus=up-to-date\n");
  CHECK(s.status == UpdateStatus::ApplyFailed);
}

TEST(update_state_ignores_a_line_without_a_separator) {
  const UpdateState s = parse("schema=1\nn importe quoi\nstatus=available\n");
  CHECK(s.status == UpdateStatus::Available);
}

TEST(update_state_splits_on_the_first_equals_sign) {
  CHECK_EQ(parse("schema=1\nmessage=a=b=c\n").message, std::string("a=b=c"));
}

TEST(update_state_ignores_an_unknown_key) {
  const UpdateState s = parse("schema=1\ninvente=oui\nstatus=available\n");
  CHECK(s.status == UpdateStatus::Available);
}

// UNE HORLOGE QUI A RECULÉ NE DOIT PAS FAIRE ATTENDRE UN JOUR DE PLUS, ET
// UNE VALEUR ABSURDE NE DOIT PAS DÉBORDER : checked_at + 86400 sur
// INT64_MAX est un débordement signé, que -fsanitize=undefined attrape et
// qui n'est de toute façon pas un comportement défini. Les deux se
// traitent d'un seul geste, par bornage à [0, maintenant].
TEST(update_state_clamps_checked_at_outside_the_plausible_range) {
  CHECK_EQ(parse("schema=1\nchecked_at=9223372036854775807\n").checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse("schema=1\nchecked_at=-5\n").checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse("schema=1\nchecked_at=pas un nombre\n").checked_at,
           static_cast<std::int64_t>(0));
  // Dans le futur : l'horloge a reculé depuis l'écriture.
  CHECK_EQ(parse("schema=1\nchecked_at=1999999999\n").checked_at,
           static_cast<std::int64_t>(0));
  // Plausible : conservé tel quel.
  CHECK_EQ(parse("schema=1\nchecked_at=1755300000\n").checked_at,
           static_cast<std::int64_t>(1755300000));
  // La borne haute exacte est acceptée.
  CHECK_EQ(parse("schema=1\nchecked_at=1755400000\n").checked_at, kNow);
}

TEST(update_state_truncates_an_oversized_message) {
  std::string raw = "schema=1\nmessage=";
  raw.append(sshos::kMaxStateMessageBytes + 50, 'z');
  raw.push_back('\n');
  CHECK_EQ(parse(raw).message.size(), sshos::kMaxStateMessageBytes);
}

TEST(update_state_reads_every_status_value) {
  struct Row {
    const char* text;
    UpdateStatus want;
  };
  const Row rows[] = {
      {"idle", UpdateStatus::Idle},
      {"checking", UpdateStatus::Checking},
      {"applying", UpdateStatus::Applying},
      {"up-to-date", UpdateStatus::UpToDate},
      {"available", UpdateStatus::Available},
      {"restart-pending", UpdateStatus::RestartPending},
      {"check-failed", UpdateStatus::CheckFailed},
      {"apply-failed", UpdateStatus::ApplyFailed},
      {"history-rewritten", UpdateStatus::HistoryRewritten},
      {"updates-disabled", UpdateStatus::UpdatesDisabled},
  };
  for (const Row& r : rows) {
    const std::string raw = std::string("schema=1\nstatus=") + r.text + "\n";
    CHECK(parse(raw).status == r.want);
  }
  // Une valeur inconnue ne devine pas : elle retombe sur l'état vierge.
  CHECK(parse("schema=1\nstatus=invente\n").status == UpdateStatus::Idle);
}

TEST(update_state_reads_a_pid_only_when_it_is_a_number) {
  CHECK_EQ(parse("schema=1\npid=1234\n").pid, 1234);
  CHECK_EQ(parse("schema=1\npid=\n").pid, -1);
  CHECK_EQ(parse("schema=1\npid=abc\n").pid, -1);
  CHECK_EQ(parse("schema=1\npid=-7\n").pid, -1);
}

// LES FINS DE LIGNE ET LES ESPACES DE BORD NE CHANGENT PAS LE SENS. Un
// script qui écrit avec printf sur une machine ou l'autre ne doit pas
// produire un fichier que le démon lit différemment.
TEST(update_state_tolerates_carriage_returns_and_edge_spaces) {
  CHECK(parse("schema=1\r\nstatus=available\r\n").status == UpdateStatus::Available);
  CHECK(parse("schema=1\n  status = available  \n").status == UpdateStatus::Available);
}

// LA DERNIERE LIGNE PEUT NE PAS AVOIR DE FIN DE LIGNE.
TEST(update_state_reads_a_last_line_without_a_newline) {
  CHECK(parse("schema=1\nstatus=available").status == UpdateStatus::Available);
}

// LA BORNE EXACTE, PAS « BEAUCOUP TROP GRAND ». Un test qui n'essaie qu'un
// fichier dix fois trop gros ne dit rien de l'endroit où le plafond tombe :
// une campagne de mutation décalant la comparaison d'un seul octet passait
// au travers.
TEST(update_state_accepts_exactly_the_cap_and_refuses_one_byte_more) {
  const std::string head = "schema=1\nstatus=available\nmessage=";
  std::string at_cap = head;
  at_cap.append(sshos::kMaxStateBytes - head.size(), 'x');
  CHECK_EQ(at_cap.size(), sshos::kMaxStateBytes);
  CHECK(parse(at_cap).status == UpdateStatus::Available);

  std::string over = at_cap;
  over.push_back('x');
  CHECK(parse(over).status == UpdateStatus::Idle);
}

// « 12abc » N'EST PAS 12, C'EST ILLISIBLE. Un fichier tronqué en pleine
// écriture, ou un script qui aurait laissé passer une unité, donnerait
// sinon un nombre parfaitement plausible tiré d'une valeur qui ne l'est pas.
TEST(update_state_refuses_a_number_with_trailing_garbage) {
  CHECK_EQ(parse("schema=1\npid=12abc\n").pid, -1);
  CHECK_EQ(parse("schema=1\npid=abc12\n").pid, -1);
  CHECK_EQ(parse("schema=1\nchecked_at=17553abc\n").checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse("schema=1\nchecked_at=1 755\n").checked_at,
           static_cast<std::int64_t>(0));
}

// UN NOMBRE TROP GRAND POUR SON TYPE EST ILLISIBLE, PAS TRONQUÉ. Vingt
// chiffres dépassent ce qu'un int64 porte ; laisser l'addition déborder
// serait un comportement indéfini, que le type Debug attrape à l'exécution
// et que le type Release transformerait en valeur arbitraire.
TEST(update_state_refuses_a_number_too_large_for_its_type) {
  CHECK_EQ(parse("schema=1\npid=99999999999999999999\n").pid, -1);
  CHECK_EQ(parse("schema=1\nchecked_at=99999999999999999999\n").checked_at,
           static_cast<std::int64_t>(0));
  CHECK_EQ(parse("schema=1\nchecked_at=-99999999999999999999\n").checked_at,
           static_cast<std::int64_t>(0));
}

// DES VERSIONS, PAS DES EMPREINTES. « cce9d11 -> 3512ffe » ne dit rien a
// personne ; « 1.12 -> 1.13 » se lit.
TEST(update_state_reads_version_numbers) {
  const UpdateState s = parse(
      "schema=1\ninstalled_version=1.12\nremote_version=1.13\n"
      "commits_ahead=1\n");
  CHECK_EQ(s.installed_version, std::string("1.12"));
  CHECK_EQ(s.remote_version, std::string("1.13"));
  CHECK_EQ(s.commits_ahead, 1);
}

// UNE VERSION SE DESSINE DANS UNE MODALE. Elle vient d'un script, donc tout
// ce qui n'est pas chiffres et points est REJETE plutot qu'affiche -- un
// texte arbitraire dans un cadre est un defaut que ce projet a deja paye.
TEST(update_state_refuses_a_version_that_is_not_a_number) {
  CHECK(parse("schema=1\ninstalled_version=1.2; rm -rf /\n").installed_version.empty());
  CHECK(parse("schema=1\ninstalled_version=v1.2\n").installed_version.empty());
  CHECK(parse("schema=1\ninstalled_version=\033[31m\n").installed_version.empty());
  CHECK(parse("schema=1\ninstalled_version=1234567890123\n").installed_version.empty());
  CHECK(parse("schema=1\ninstalled_version=\n").installed_version.empty());
  // Et ce qui est legitime passe.
  CHECK_EQ(parse("schema=1\ninstalled_version=12.345\n").installed_version,
           std::string("12.345"));
}

TEST(update_state_ignores_an_absurd_commit_count) {
  CHECK_EQ(parse("schema=1\ncommits_ahead=abc\n").commits_ahead, 0);
  CHECK_EQ(parse("schema=1\ncommits_ahead=-4\n").commits_ahead, 0);
  CHECK_EQ(parse("schema=1\ncommits_ahead=7\n").commits_ahead, 7);
}

// ---------------------------------------------------------------------------
// Les notes de version : ce qui a change, et non seulement combien.
// ---------------------------------------------------------------------------

// « 5 nouveautes » ne dit pas LESQUELLES. Le script les extrait des sujets de
// commit -- il a git sous la main, le C++ ne l'aura jamais -- et les depose
// numerotees.
TEST(update_state_reads_the_release_notes) {
  const UpdateState s = parse(
      "schema=1\n"
      "status=available\n"
      "note_1=la molette atteint enfin l application\n"
      "note_2=le terminal s ouvre chez vous\n");

  REQUIRE_EQ(s.notes.size(), size_t{2});
  CHECK_EQ(s.notes[0], std::string("la molette atteint enfin l application"));
  CHECK_EQ(s.notes[1], std::string("le terminal s ouvre chez vous"));
}

// LA NUMEROTATION EST UNE SUITE, et on s'arrete au premier trou. Ramasser
// « note_9 » apres un « note_2 » absent ferait afficher une liste dont
// l'ordre ne veut plus rien dire.
TEST(update_state_stops_the_notes_at_the_first_gap) {
  const UpdateState s = parse(
      "schema=1\nstatus=available\nnote_1=une\nnote_3=trois\n");

  REQUIRE_EQ(s.notes.size(), size_t{1});
  CHECK_EQ(s.notes[0], std::string("une"));
}

// SANS NOTES, PAS DE LISTE : c'est le cas d'une installation mise a jour par
// un script plus ancien, et ce n'est pas une erreur.
TEST(update_state_without_notes_has_none) {
  const UpdateState s = parse("schema=1\nstatus=available\n");
  CHECK(s.notes.empty());
}

// ELLES SONT BORNEES EN NOMBRE. Une modale qui deborde de l'ecran a deja ete
// un defaut de ce projet, et le fichier vient d'un script.
TEST(update_state_caps_how_many_notes_it_keeps) {
  std::string raw = "schema=1\nstatus=available\n";
  for (int i = 1; i <= 40; ++i) {
    raw += "note_" + std::to_string(i) + "=note numero " + std::to_string(i) + "\n";
  }

  const UpdateState s = parse(raw);
  CHECK_EQ(s.notes.size(), sshos::kMaxUpdateNotes);
}

// ET EN LONGUEUR, chacune.
TEST(update_state_elides_a_very_long_note) {
  const UpdateState s = parse("schema=1\nstatus=available\nnote_1=" +
                              std::string(400, 'a') + "\n");

  REQUIRE_EQ(s.notes.size(), size_t{1});
  CHECK(s.notes[0].size() <= sshos::kMaxUpdateNoteChars);
}

// UN SUJET DE COMMIT NE PILOTE PAS LE TERMINAL. La valeur vient d'un script,
// passe par un fichier que n'importe qui peut editer, et FINIT DESSINEE dans
// une modale : un echappement qui y survivrait pourrait repeindre l'ecran du
// bureau. C'est exactement la raison pour laquelle les numeros de version
// refusent tout ce qui n'est pas chiffres et points.
TEST(update_state_strips_control_bytes_from_a_note) {
  const UpdateState s = parse(
      "schema=1\nstatus=available\nnote_1=avant\033[2Japres\007\n");

  REQUIRE_EQ(s.notes.size(), size_t{1});
  CHECK_EQ(s.notes[0], std::string("avant[2Japres"));
}


// --- la progression chiffree ---------------------------------------------
//
// Cinq libelles d'etape couvraient une a deux minutes d'attente : « Mise a
// jour en cours : compilation... » restait affiche pendant toute la
// compilation, sans jamais bouger. Le script sait compter -- cmake ecrit son
// propre pourcentage, la suite de tests une ligne par cas -- donc il compte,
// et le C++ se contente de LIRE. Il ne calcule rien, comme pour les numeros
// de version.

TEST(update_state_reads_a_progress_percentage) {
  const UpdateState s = parse("schema=1\nstatus=applying\nprogress=47\n");
  CHECK_EQ(s.progress, 47);
}

// Absente veut dire INCONNUE, et non zero : une installation mise a jour par
// un script plus ancien n'en depose aucune, et une barre a zero pour cent
// laisserait croire qu'il ne se passe rien.
TEST(update_state_leaves_progress_unknown_when_the_file_says_nothing) {
  const UpdateState s = parse("schema=1\nstatus=applying\n");
  CHECK_EQ(s.progress, -1);
}

// La valeur vient d'un script et finit DESSINEE. Meme discipline que les
// numeros de version : ce qui n'est pas un pourcentage est refuse, pas
// rogne -- rogner inventerait un chiffre.
TEST(update_state_refuses_a_progress_that_is_not_a_percentage) {
  CHECK_EQ(parse("schema=1\nprogress=101\n").progress, -1);
  CHECK_EQ(parse("schema=1\nprogress=-3\n").progress, -1);
  CHECK_EQ(parse("schema=1\nprogress=beaucoup\n").progress, -1);
  CHECK_EQ(parse("schema=1\nprogress=47%\n").progress, -1);
  CHECK_EQ(parse("schema=1\nprogress=\n").progress, -1);
}

// Les deux bornes sont valides : zero pour cent est un debut legitime, cent
// pour cent une fin legitime.
TEST(update_state_accepts_both_ends_of_the_percentage) {
  CHECK_EQ(parse("schema=1\nprogress=0\n").progress, 0);
  CHECK_EQ(parse("schema=1\nprogress=100\n").progress, 100);
}

// --- LE FAIT, ET RIEN QUE « 1 » -----------------------------------------
//
// `restart_pending` dit qu'un binaire est pose et que ce n'est pas celui qui
// tourne. Comme les numeros de version et le pourcentage, la valeur vient
// d'un script et passe par un fichier editable a la main : on ne devine pas.
// « 1 » et rien d'autre. (Campagne de mutation, M1.)
TEST(update_state_reads_the_restart_fact_only_as_one) {
  CHECK(parse("schema=1\nrestart_pending=1\n").restart_pending);

  // Tout le reste est faux -- y compris « 0 », « true » et « oui », qui
  // seraient les erreurs naturelles d'une edition a la main.
  for (const char* v : {"0", "", "true", "oui", "yes", "2", "01", "1.0"}) {
    const std::string raw =
        std::string("schema=1\nrestart_pending=") + v + "\n";
    CHECK(!parse(raw).restart_pending);
  }

  // L'ESPACE AUTOUR EST TOLERE, comme pour toute autre cle : l'analyseur
  // rogne cle et valeur avant de les lire. C'est une tolerance uniforme et
  // deliberee, pas un relachement propre a ce champ -- un fichier edite a la
  // main garde souvent une espace apres le signe.
  CHECK(parse("schema=1\nrestart_pending= 1\n").restart_pending);
  CHECK(parse("schema=1\n restart_pending =1 \n").restart_pending);
}

// LA CLE PRESENTE FAIT AUTORITE, ET LA MIGRATION NE JOUE QUE SUR SON
// ABSENCE.
//
// Un fichier ecrit par un script anterieur n'a pas la cle, mais son
// `status=restart-pending` dit la meme chose : on la reconstruit, sinon la
// mise a jour qui INTRODUIT la cle perdrait le redemarrage qu'elle vient
// elle-meme de mettre en attente -- au pire moment possible.
//
// Mais quand la cle est la et dit non, c'est elle qui gagne : sans quoi la
// migration deviendrait un « toujours vrai » des que le statut le redit, et
// le fait ne pourrait plus jamais retomber. (Campagne de mutation, M5.)
TEST(update_state_migrates_only_when_the_restart_key_is_absent) {
  // Absente + le statut le dit : on reconstruit.
  CHECK(parse("schema=1\nstatus=restart-pending\n").restart_pending);

  // Presente et negative : elle fait autorite, meme contre le statut.
  CHECK(!parse("schema=1\nstatus=restart-pending\nrestart_pending=\n")
             .restart_pending);
  CHECK(!parse("schema=1\nstatus=restart-pending\nrestart_pending=0\n")
             .restart_pending);

  // Absente et le statut ne dit rien : rien a reconstruire.
  CHECK(!parse("schema=1\nstatus=up-to-date\n").restart_pending);

  // Et l'ordre des lignes n'y change rien.
  CHECK(!parse("schema=1\nrestart_pending=\nstatus=restart-pending\n")
             .restart_pending);
}
