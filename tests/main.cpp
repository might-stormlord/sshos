#include <cstring>
#include <exception>

#include "harness.hpp"

// UBSan imprime un diagnostic sur undefined behavior puis, par défaut,
// *continue* — le processus peut sortir en 0 après un signed overflow ou un
// shift hors bornes, comme si de rien n'était. `halt_on_error`/
// `abort_on_error` corrigent ça, mais ce sont des options lues depuis
// UBSAN_OPTIONS ou depuis ce symbole faible ; un export oublié dans
// l'environnement (ou un lanceur de test qui ne le propage pas) désarme
// silencieusement le gardien. Définir __ubsan_default_options() ici fixe le
// réglage dans le binaire lui-même, quel que soit l'invocateur.
//
// GCC ne définit aucune macro de détection équivalente à
// __has_feature(undefined_behavior_sanitizer) de Clang, donc ce symbole est
// défini inconditionnellement plutôt que sous #ifdef. Inoffensif en
// Release : aucun runtime UBSan n'y est lié (CMakeLists.txt n'ajoute
// -fsanitize=undefined qu'en Debug), le symbole reste alors exporté mais
// jamais appelé.
extern "C" const char* __ubsan_default_options() {
  return "halt_on_error=1:abort_on_error=1";
}

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed_cases = 0;

  for (const auto& c : th::registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    const int before = th::failures();
    std::printf("- %s\n", c.name);
    // Un test qui lève au lieu d'échouer proprement ne doit pas emporter le
    // reste de la suite : tous les test_*.cpp sont liés dans un seul
    // binaire, donc une exception non interceptée ici saute par-dessus tous
    // les cas restants du registre et par-dessus la ligne de résumé.
    try {
      c.fn();
    } catch (const std::exception& e) {
      th::fail_uncaught(c.name, e.what());
    } catch (...) {
      th::fail_uncaught(c.name, "type inconnu (n'herite pas de std::exception)");
    }
    ++ran;
    if (th::failures() > before) ++failed_cases;
  }

  std::printf("\n%d cas, %d en echec, %d assertions echouees\n", ran,
              failed_cases, th::failures());
  return th::failures() == 0 ? 0 : 1;
}
