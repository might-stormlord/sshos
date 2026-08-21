#include "daemon/config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "common/paths.hpp"

namespace sshos {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

DesktopConfig parse_config(std::string_view texte) {
  DesktopConfig out;
  size_t i = 0;
  while (i <= texte.size()) {
    const size_t fin = texte.find('\n', i);
    const std::string_view ligne =
        texte.substr(i, fin == std::string_view::npos ? std::string_view::npos : fin - i);
    i = fin == std::string_view::npos ? texte.size() + 1 : fin + 1;

    const std::string_view net = trim(ligne);
    if (net.empty() || net.front() == '#') continue;
    const size_t egal = net.find('=');
    if (egal == std::string_view::npos) continue;
    const std::string_view cle = trim(net.substr(0, egal));
    const std::string_view valeur = trim(net.substr(egal + 1));
    if (cle.empty()) continue;
    // LA DERNIERE GAGNE : on assigne sans regarder si la cle etait deja la.
    if (cle == "start_dir") out.start_dir = std::string(valeur);
  }
  return out;
}

std::string render_config(const DesktopConfig& c) {
  std::string out =
      "# Reglages du bureau termos. Modifiable a la main ; le demon le relit\n"
      "# a son demarrage et le reecrit quand on change un reglage depuis le\n"
      "# bureau. Une ligne inconnue est ignoree, pas effacee au prochain tour.\n";
  // RIEN POUR UNE VALEUR VIDE. « start_dir = » se relirait en chaine vide,
  // ce qui est deja le defaut : autant que le fichier dise ce qui est
  // choisi, et rien d'autre.
  if (!c.start_dir.empty()) out += "start_dir = " + c.start_dir + "\n";
  return out;
}

DesktopConfig load_config(const std::string& path) {
  if (path.empty()) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  // BORNE : un fichier de reglages est court par nature, et rien ne garantit
  // que ce chemin porte bien ce qu'on croit.
  constexpr std::streamsize kMax = 64 * 1024;
  std::string buf(static_cast<size_t>(kMax) + 1, '\0');
  in.read(buf.data(), kMax);
  buf.resize(static_cast<size_t>(in.gcount()));
  return parse_config(buf);
}

bool save_config(const std::string& path, const DesktopConfig& c) {
  if (path.empty()) return false;
  // Le repertoire de donnees peut ne pas exister encore : meme raison que
  // pour le journal, et meme remede -- chaque segment, pas seulement le
  // dernier.
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = path.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); ++i) {
      if (i < dir.size() && dir[i] != '/') continue;
      ::mkdir(dir.substr(0, i).c_str(), 0700);  // EEXIST est le cas normal
    }
  }

  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << render_config(c);
    if (!out) return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());  // rien ne traine a cote du vrai fichier
    return false;
  }
  return true;
}

namespace {

// `~` et `~/...` seulement. Un `~autre` designe le home d'un AUTRE compte,
// ce que le shell sait resoudre et pas nous : on le laisse tel quel plutot
// que de fabriquer un chemin faux. Un `~` ailleurs qu'en tete est un nom de
// fichier parfaitement legitime.
std::string expand_home(const std::string& p, const std::string& home) {
  if (p.empty() || p[0] != '~') return p;
  if (p.size() == 1) return home;
  if (p[1] != '/') return p;
  return home + p.substr(1);
}

}  // namespace

Settings::Settings(std::string path, std::string home)
    : path_(std::move(path)), home_(std::move(home)), config_(load_config(path_)) {}

std::string Settings::start_dir() const {
  if (config_.start_dir.empty()) return home_;
  return expand_home(config_.start_dir, home_);
}

bool Settings::set_start_dir(std::string dir) {
  config_.start_dir = std::move(dir);
  return save_config(path_, config_);
}

std::string desktop_config_path() {
  const std::string dir = user_data_dir();
  if (dir.empty()) return {};
  return dir + "/config";
}

}  // namespace sshos
