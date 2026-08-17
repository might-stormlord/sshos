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
