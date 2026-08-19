#include "shell/update_state.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace sshos {
namespace {

// Retire les espaces de bord et le retour chariot d'une fin de ligne
// Windows. Un script qui écrit avec `printf` ne doit pas produire un fichier
// que le démon lit différemment selon la machine.
std::string_view trim(std::string_view s) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r';
  };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

// Lit un entier décimal signé, sans exception et sans dépasser. Rend false
// dès qu'un caractère n'est pas un chiffre : « 12abc » n'est pas 12, c'est
// illisible. Une valeur qui déborde est illisible aussi -- accepter
// INT64_MAX ici ferait déborder l'arithmétique qui suit.
bool read_int64(std::string_view s, std::int64_t& out) {
  if (s.empty()) return false;
  bool negative = false;
  std::size_t i = 0;
  if (s[0] == '-' || s[0] == '+') {
    negative = s[0] == '-';
    i = 1;
    if (s.size() == 1) return false;
  }
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    const int d = c - '0';
    // Garde AVANT la multiplication : le débordement signé n'est pas un
    // comportement défini, et -fsanitize=undefined l'attraperait.
    if (v > (9223372036854775807LL - d) / 10) return false;
    v = v * 10 + d;
  }
  out = negative ? -v : v;
  return true;
}

// Une version se DESSINE dans une modale : elle ne doit contenir que des
// chiffres et des points, et rester courte. Tout le reste est rejete plutot
// qu'affiche -- la valeur vient d'un script, et un texte arbitraire dans un
// cadre est exactement ce que le projet a deja paye une fois.
std::string_view clean_version(std::string_view s) {
  if (s.empty() || s.size() > 12) return {};
  for (const char c : s) {
    if ((c < '0' || c > '9') && c != '.') return {};
  }
  return s;
}

bool read_status(std::string_view s, UpdateStatus& out) {
  static constexpr std::pair<std::string_view, UpdateStatus> kTable[] = {
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
  for (const auto& row : kTable) {
    if (row.first == s) {
      out = row.second;
      return true;
    }
  }
  return false;
}

}  // namespace

namespace {

// UNE NOTE EST DESSINEE DANS UNE MODALE : elle ne doit rien pouvoir piloter.
// Tout octet de controle part -- un echappement qui survivrait pourrait
// repeindre l'ecran du bureau depuis un fichier editable a la main. C'est la
// meme raison qui fait refuser aux numeros de version tout ce qui n'est pas
// chiffres et points.
//
// Puis on borne, en reculant sur une frontiere UTF-8 : couper au milieu
// d'une sequence laisserait un demi-caractere dans le cadre.
std::string clean_note(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (const char c : v) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7f) out.push_back(c);
  }
  if (out.size() > kMaxUpdateNoteChars) {
    out.resize(kMaxUpdateNoteChars);
    while (!out.empty() &&
           (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80) {
      out.pop_back();
    }
  }
  return out;
}

// « note_12 » -> 12. Rend 0 -- donc « pas une note » -- pour tout le reste :
// un « note_ » nu, un « note_0 », un indice qui deborde.
std::size_t note_index(std::string_view key) {
  constexpr std::string_view prefixe = "note_";
  if (key.size() <= prefixe.size() || key.compare(0, prefixe.size(), prefixe) != 0) {
    return 0;
  }
  std::size_t n = 0;
  for (const char c : key.substr(prefixe.size())) {
    if (c < '0' || c > '9') return 0;
    n = n * 10 + static_cast<std::size_t>(c - '0');
    if (n > 10000) return 0;
  }
  return n;
}

}  // namespace

UpdateState parse_update_state(std::string_view raw, std::int64_t now_epoch) {
  UpdateState out;
  if (raw.size() > kMaxStateBytes) return out;

  // Les clés déjà vues. La PREMIÈRE occurrence gagne : une ligne ajoutée
  // après coup ne doit pas pouvoir écraser une valeur déjà lue.
  std::vector<std::string_view> seen;
  const auto first_time = [&seen](std::string_view key) {
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return false;
    seen.push_back(key);
    return true;
  };

  bool schema_ok = false;
  UpdateState parsed;
  std::vector<std::pair<std::size_t, std::string>> notes;

  std::size_t pos = 0;
  while (pos <= raw.size()) {
    const std::size_t nl = raw.find('\n', pos);
    const std::string_view line =
        raw.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                     : nl - pos);
    pos = nl == std::string_view::npos ? raw.size() + 1 : nl + 1;

    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;  // ni clé ni valeur

    const std::string_view key = trim(line.substr(0, eq));
    const std::string_view value = trim(line.substr(eq + 1));
    if (key.empty() || !first_time(key)) continue;

    if (key == "schema") {
      std::int64_t v = 0;
      schema_ok = read_int64(value, v) && v == kUpdateStateSchema;
    } else if (key == "prefix") {
      parsed.prefix = std::string(value);
    } else if (key == "source") {
      parsed.source = std::string(value);
    } else if (key == "installed_commit") {
      parsed.installed_commit = std::string(value);
    } else if (key == "previous_commit") {
      parsed.previous_commit = std::string(value);
    } else if (key == "remote_commit") {
      parsed.remote_commit = std::string(value);
    } else if (key == "installed_version") {
      parsed.installed_version = std::string(clean_version(value));
    } else if (key == "remote_version") {
      parsed.remote_version = std::string(clean_version(value));
    } else if (key == "commits_ahead") {
      std::int64_t v = 0;
      if (read_int64(value, v) && v > 0 && v < 1000000) {
        parsed.commits_ahead = static_cast<int>(v);
      }
    } else if (key == "checked_at") {
      std::int64_t v = 0;
      // Hors de [0, maintenant] : ramené à zéro. Une valeur dans le futur
      // veut dire que l'horloge a reculé depuis l'écriture ; une valeur
      // absurde ferait déborder l'arithmétique des 24 h.
      if (read_int64(value, v) && v >= 0 && v <= now_epoch) parsed.checked_at = v;
    } else if (key == "status") {
      UpdateStatus st = UpdateStatus::Idle;
      if (read_status(value, st)) parsed.status = st;
    } else if (key == "pid") {
      std::int64_t v = 0;
      if (read_int64(value, v) && v > 0) parsed.pid = static_cast<pid_t>(v);
    } else if (key == "stage") {
      // Meme discipline que `message` : il est dessine, donc borne.
      parsed.stage = std::string(
          value.substr(0, std::min(value.size(), std::size_t{40})));
    } else if (key == "message") {
      parsed.message = std::string(value.substr(
          0, std::min(value.size(), kMaxStateMessageBytes)));
    } else if (const std::size_t n = note_index(key); n != 0) {
      // Rangees telles quelles ici ; l'ORDRE est refait apres la boucle, a
      // partir des indices -- le fichier peut les donner dans n'importe
      // quel ordre, et une modale qui les afficherait dans celui du
      // fichier mentirait sur la chronologie.
      notes.emplace_back(n, clean_note(value));
    }
    // Toute autre clé est ignorée : le format peut gagner des champs sans
    // que les anciens lecteurs aient à s'en soucier.
  }

  if (!schema_ok) return out;  // état vierge, sans message

  // LA SUITE 1, 2, 3... ET ON S'ARRETE AU PREMIER TROU. Ramasser « note_9 »
  // apres un « note_2 » absent afficherait une liste dont l'ordre ne veut
  // plus rien dire -- et rien ne garantit que le trou soit accidentel.
  for (std::size_t rang = 1; rang <= kMaxUpdateNotes; ++rang) {
    const auto it = std::find_if(notes.begin(), notes.end(),
                                 [rang](const auto& p) { return p.first == rang; });
    if (it == notes.end()) break;
    if (it->second.empty()) break;  // une note vide n'a rien a dire
    parsed.notes.push_back(it->second);
  }
  return parsed;
}

}  // namespace sshos
