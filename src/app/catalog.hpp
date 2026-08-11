#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"

namespace sshos {

// Une entrée du menu de lancement. `make` est un pointeur de fonction, pas
// un std::function : le catalogue est une table statique, il n'a rien à
// capturer.
struct CatalogEntry {
  std::string id;
  std::string label;
  std::unique_ptr<App> (*make)();
};

const std::vector<CatalogEntry>& catalog();
const CatalogEntry* catalog_find(std::string_view id);

}  // namespace sshos
