#pragma once

#include <sys/types.h>

namespace sshos {

// Ce que la récolte rapporte. Séparé de `Session` pour que la boucle de
// récolte se teste seule, avec de vrais enfants et sans démon autour.
class ChildSink {
 public:
  virtual ~ChildSink() = default;
  virtual void on_child_exit(pid_t pid, int status) = 0;
};

// Récolte TOUS les enfants morts, et rend leur nombre.
//
// LES SIGNAUX STANDARDS NE SONT PAS MIS EN FILE. Trois enfants morts entre
// deux lectures du `signalfd` ne produisent qu'UN enregistrement, dont le
// `ssi_pid` n'en nomme qu'un seul. Récolter le pid rapporté laisserait donc
// les deux autres en zombies -- et leurs maîtres de pseudo-terminaux
// jamais fermés, jusqu'à épuiser `kernel.pty.max` (4096). D'où la boucle :
// on appelle `waitpid(-1, WNOHANG)` jusqu'à ce qu'elle rende 0 (plus rien
// de mort) ou -1 (plus d'enfant du tout).
int reap_children(ChildSink& sink);

}  // namespace sshos
