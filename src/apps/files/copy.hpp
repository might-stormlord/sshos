#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sshos {

// COPIER SANS BLOQUER LE DÉMON.
//
// Le démon est mono-thread. Copier deux gigaoctets avec un `read`/`write`
// en boucle gèlerait toutes les fenêtres et tous les clients pendant la
// copie -- y compris l'horloge, y compris les autres sessions. Ce travail
// avance donc PAR TRANCHES : `step(budget)` copie au plus `budget` octets
// puis rend la main, et la boucle d'événements repasse quand elle veut.
//
// C'est aussi ce qui le rend interruptible : entre deux tranches, rien ne
// retient l'application.
//
// L'arborescence est parcourue PARESSEUSEMENT, pour la même raison : un
// `readdir()` par répertoire, quand on y arrive, plutôt qu'un parcours
// complet avant de commencer. Le total n'est donc pas connu d'avance, et
// c'est assumé -- l'avancement se compte en éléments traités, pas en
// pourcentage d'un tout qu'il faudrait payer un parcours entier pour
// connaître.
enum class CopyKind : uint8_t { Copy, Move };

class CopyJob {
 public:
  // `sources` sont des chemins absolus, `dest` le répertoire d'arrivée.
  void start(std::vector<std::string> sources, std::string dest,
             CopyKind kind);

  bool active() const { return active_; }
  // Avance d'au plus `budget` octets, ou d'un geste de répertoire. Rend
  // true tant qu'il reste du travail.
  bool step(size_t budget);
  void cancel();

  // Ce qu'on est en train de faire, pour la ligne d'état. Vide au repos.
  const std::string& current() const { return current_; }
  int done() const { return done_; }
  // Le premier échec rencontré, et le nombre total. On CONTINUE après un
  // échec : s'arrêter au premier laisserait une copie à moitié faite dont
  // l'utilisateur ne saurait pas où elle en est.
  const std::string& error() const { return error_; }
  int failed() const { return failed_; }
  CopyKind kind() const { return kind_; }

 private:
  struct Item {
    std::string from;
    std::string to;
  };

  // Ouvre le prochain élément. Rend false quand la pile est vide.
  bool take_next();
  void fail(const std::string& what);
  void finish_current();

  bool active_ = false;
  CopyKind kind_ = CopyKind::Copy;
  std::vector<Item> pending_;
  std::string current_;
  std::string current_from_;
  std::string current_to_;
  int in_ = -1;
  int out_ = -1;
  int done_ = 0;
  int failed_ = 0;
  std::string error_;
};

}  // namespace sshos
