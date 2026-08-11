#include "app/catalog.hpp"

#include "apps/battement.hpp"
#include "apps/bloc.hpp"

namespace sshos {
namespace {

std::unique_ptr<App> make_bloc() { return std::make_unique<Bloc>(); }
std::unique_ptr<App> make_battement() { return std::make_unique<Battement>(); }

}  // namespace

// Ce fichier inclut apps/bloc.hpp, donc app/ dépend ici de apps/. C'est la
// seule entorse à la règle de dépendance du projet, et elle est
// délibérée : le catalogue est par définition la liste de ce qui existe.
// Le contrat lui-même (app/app.hpp) reste totalement ignorant de ses
// implémentations.
const std::vector<CatalogEntry>& catalog() {
  static const std::vector<CatalogEntry> entries = {
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
