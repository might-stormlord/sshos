#include "app/catalog.hpp"

#include "apps/battement.hpp"
#include "apps/bloc.hpp"
#include "apps/editor/editor.hpp"
#include "apps/files/files.hpp"
#include "apps/monitor/monitor.hpp"
#include "apps/terminal.hpp"

namespace sshos {
namespace {

std::unique_ptr<App> make_bloc() { return std::make_unique<Bloc>(); }
std::unique_ptr<App> make_battement() { return std::make_unique<Battement>(); }
std::unique_ptr<App> make_terminal() { return std::make_unique<Terminal>(); }
std::unique_ptr<App> make_files() { return std::make_unique<Files>(); }
std::unique_ptr<App> make_monitor() { return std::make_unique<Monitor>(); }
std::unique_ptr<App> make_editor() { return std::make_unique<Editor>(); }

}  // namespace

// Ce fichier inclut apps/bloc.hpp, donc app/ dépend ici de apps/. C'est la
// seule entorse à la règle de dépendance du projet, et elle est
// délibérée : le catalogue est par définition la liste de ce qui existe.
// Le contrat lui-même (app/app.hpp) reste totalement ignorant de ses
// implémentations.
const std::vector<CatalogEntry>& catalog() {
  static const std::vector<CatalogEntry> entries = {
      // Le Terminal EN TETE : c'est desormais la raison d'etre du bureau.
      {"terminal", "Terminal", &make_terminal},
      {"fichiers", "Fichiers", &make_files},
      {"moniteur", "Moniteur", &make_monitor},
      {"editeur", "Editeur", &make_editor},
      {"bloc", "Bloc", &make_bloc},
      {"battement", "Battement", &make_battement},
  };
  return entries;
}

const CatalogEntry* catalog_find(std::string_view id) {
  for (const auto& e : catalog()) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

}  // namespace sshos
