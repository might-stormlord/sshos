#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "apps/files/dir.hpp"

namespace sshos {

// LE GESTIONNAIRE DE FICHIERS. Panneau unique, barre de chemin, liste
// triée dossiers d'abord. Pas de vue en arbre : la spec la refuse, et un
// panneau qui tient dans une fenêtre de vingt lignes vaut mieux qu'un
// arbre qu'on ne peut pas déplier.
//
// L'application ne relit le disque QU'AUX CHANGEMENTS DE RÉPERTOIRE. Le
// démon est mono-thread : relire à chaque frappe -- ou pire, à chaque
// rendu -- ferait payer un `readdir()` par touche, et gèlerait toutes les
// fenêtres sur un montage lent.
class Files : public App {
 public:
  Files();
  explicit Files(std::string start);

  void attach(Host& host) override;
  void render(View v) override;
  void on_key(const KeyEvent& k) override;
  void on_mouse(const MouseEvent& m) override;
  void on_resize(Size s) override;
  bool wants_cursor(Pos& out) const override;
  Size min_size() const override { return {24, 6}; }

  // Ce que l'application est en train de faire. Le renommage et la
  // suppression sont des ÉTATS, pas des raccourcis : la suppression est le
  // seul geste irréversible du projet, et un raccourci qui détruit sans
  // repasser par une question détruit tôt ou tard par erreur.
  enum class Mode { Normal, Renaming, Confirming };

  // --- pour les tests ---
  // La navigation n'a aucun effet observable hors de son dessin : ces
  // accès permettent de la vérifier sans lire une grille de caractères.
  // Aucun code de production ne doit s'en servir.
  const std::string& path_for_tests() const { return listing_.path; }
  const std::vector<DirEntry>& visible_for_tests() const { return visible_; }
  size_t selected_for_tests() const { return sel_; }
  size_t top_for_tests() const { return top_; }
  const std::string& status_for_tests() const { return status_; }
  const std::string& filter_for_tests() const { return filter_; }
  Mode mode_for_tests() const { return mode_; }
  const std::string& edit_for_tests() const { return edit_; }
  const std::set<std::string>& marked_for_tests() const { return marked_; }

 private:
  // Relit le répertoire courant et refait la liste visible. LE SEUL
  // endroit qui touche au disque.
  void reload();
  // Refait la liste visible depuis ce qui est déjà en mémoire. Appelée à
  // chaque frappe du filtre : elle ne relit RIEN.
  void refilter();
  // Ramène la sélection dans la liste, puis le défilement sur la
  // sélection. L'ordre compte : borner le défilement sur une sélection
  // hors bornes le poserait n'importe où.
  void settle();
  // Le nombre de lignes que la liste peut montrer : la fenêtre moins la
  // barre de chemin et la ligne d'état.
  int rows_for_list() const;
  void activate();
  void go_up();
  // Le nom sélectionné, ou une chaîne vide si la sélection ne désigne rien
  // qu'on ait le droit de toucher -- `..` en particulier.
  std::string touchable_selection() const;
  void commit_rename();
  void commit_delete();

  // Le nom sous la sélection, ou vide si elle ne désigne rien de marquable
  // -- `..` en particulier, qui n'est pas un fichier mais la sortie.
  std::string markable_at(size_t i) const;
  // Marque ou démarque, et rend true si quelque chose a changé.
  bool toggle_mark(size_t i);
  // Marque tout ce qui va de `a` à `b`, dans un sens comme dans l'autre.
  void mark_range(size_t a, size_t b);
  // Ce sur quoi une action porte : les marqués s'il y en a, sinon la seule
  // ligne sous la sélection. C'est la règle de tous les gestionnaires, et
  // elle évite d'avoir à marquer un fichier pour agir sur lui.
  std::vector<std::string> targets() const;

  DirListing listing_;
  std::vector<DirEntry> visible_;
  std::string filter_;
  std::string status_;
  size_t sel_ = 0;
  size_t top_ = 0;
  bool show_hidden_ = false;
  // LES NOMS, PAS LES RANGS. Le filtre et le tri renumérotent la liste sous
  // les pieds de la sélection ; un rang marqué désignerait alors un autre
  // fichier. Vidé à chaque changement de répertoire : les noms d'avant
  // auraient des homonymes ici, et l'action porterait sur eux.
  std::set<std::string> marked_;
  Mode mode_ = Mode::Normal;
  // Le nom en cours de saisie pendant un renommage.
  std::string edit_;
  Size size_{40, 12};
  Host* host_ = nullptr;
};

}  // namespace sshos
