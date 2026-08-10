#pragma once

#include <termios.h>

#include <string>
#include <utility>
#include <vector>

namespace sshos {

std::string tty_setup_sequence();
std::string tty_restore_sequence();

// Réservé aux tests : littéral async-signal-safe utilisé par le gestionnaire
// de plantage (voir tty_guard.cpp) pour restaurer le terminal sans allouer.
// Un test compare son contenu, octet pour octet, à tty_restore_sequence() --
// les deux DOIVENT rester identiques, et rien d'autre que ce test ne le
// garantit si l'un des deux change sans l'autre.
const char* crash_restore_literal_for_tests();

// Réservé aux tests : rejoue exactement la logique de restauration que le
// gestionnaire de signal fatal exécuterait (séquences d'échappement puis
// tcsetattr, sur le descripteur actuellement armé par un TtyGuard vivant),
// sans passer par un vrai signal. Permet de vérifier que le filet de
// sécurité restaure bien le termios -- pas seulement les séquences
// d'échappement -- sans avoir à faire planter le processus de test.
void run_crash_restore_for_tests();

// Variables que le démon doit rafraîchir à chaque attache, appliquées
// uniquement aux NOUVEAUX enfants (modèle update-environment de tmux) :
// réécrire l'environnement d'un shell déjà lancé est impossible, et
// fossiliser SSH_AUTH_SOCK fait réclamer une passphrase à vie.
std::vector<std::pair<std::string, std::string>> collect_env_delta();

// RAII : le destructeur remet le terminal en état. Non déplaçable — un
// second propriétaire restaurerait deux fois, dont une trop tôt.
class TtyGuard {
 public:
  explicit TtyGuard(int fd);
  ~TtyGuard();

  TtyGuard(const TtyGuard&) = delete;
  TtyGuard& operator=(const TtyGuard&) = delete;
  TtyGuard(TtyGuard&&) = delete;
  TtyGuard& operator=(TtyGuard&&) = delete;

  // Filet de sécurité pour les signaux fatals : installe des gestionnaires
  // qui restaurent puis relancent le signal avec la disposition par défaut.
  // Le gestionnaire n'alloue pas (le corrupteur de tas de glibc appelle
  // abort() depuis l'intérieur de malloc(), verrou de tas tenu -- un
  // gestionnaire qui alloue à cet instant précis se bloque pour toujours) et
  // restaure aussi le termios sauvegardé, pas seulement les séquences
  // d'échappement : sans lui, tout plantage laisserait le terminal en mode
  // brut, un état dont l'utilisateur ne peut sortir qu'en connaissant
  // `stty sane`.
  static void install_crash_handlers();

 private:
  int fd_;
  termios saved_{};
  bool armed_ = false;
};

}  // namespace sshos
