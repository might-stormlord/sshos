#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "apps/files/copy.hpp"
#include "apps/files/dir.hpp"

namespace sshos {

// La largeur du liseré des raccourcis. Assez pour « Documents », pas assez
// pour peser sur la liste : sur une fenêtre de quatre-vingts colonnes il
// prend un huitième de la place, et `F9` le retire quand il gêne.
inline constexpr int kPlacesWidth = 12;

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
  // On ne demande à être réveillé QUE pendant une copie : un
  // rafraîchissement permanent coûterait une trame par intervalle sur un
  // bureau qui ne change pas.
  int refresh_ms() const override;
  void on_refresh() override;

  // Ce que l'application est en train de faire. Le renommage et la
  // suppression sont des ÉTATS, pas des raccourcis : la suppression est le
  // seul geste irréversible du projet, et un raccourci qui détruit sans
  // repasser par une question détruit tôt ou tard par erreur.
  enum class Mode { Normal, Renaming, Confirming, Creating };

  // UN PANNEAU : son répertoire, sa liste, son filtre, sa sélection, son
  // historique, son tri. Deux panneaux sont deux vues INDÉPENDANTES du
  // disque -- c'est tout l'intérêt de la vue scindée, et le partage du
  // moindre de ces champs le ruinerait.
  //
  // Ce qui reste hors d'ici est ce qui appartient à la FENÊTRE : le mode de
  // saisie, les fichiers cachés, la taille. Un renommage en cours n'a pas
  // de raison d'être par panneau, puisqu'on ne peut taper que dans un seul.
  struct Pane {
    DirListing listing;
    std::vector<DirEntry> visible;
    std::string filter;
    std::string status;
    std::set<std::string> marked;
    std::vector<std::string> back;
    std::vector<std::string> forward;
    size_t sel = 0;
    size_t top = 0;
    SortBy sort_by = SortBy::Name;
    bool sort_desc = false;
  };

  // --- pour les tests ---
  // La navigation n'a aucun effet observable hors de son dessin : ces
  // accès permettent de la vérifier sans lire une grille de caractères.
  // Aucun code de production ne doit s'en servir.
  const std::string& path_for_tests() const { return pane().listing.path; }
  const std::vector<DirEntry>& visible_for_tests() const {
    return pane().visible;
  }
  size_t selected_for_tests() const { return pane().sel; }
  size_t top_for_tests() const { return pane().top; }
  const std::string& status_for_tests() const { return pane().status; }
  const std::string& filter_for_tests() const { return pane().filter; }
  Mode mode_for_tests() const { return mode_; }
  bool split_for_tests() const { return split_; }
  bool places_for_tests() const { return places_; }
  size_t active_pane_for_tests() const { return active_; }
  const Pane& pane_for_tests(size_t i) const { return panes_[i]; }
  bool copy_active_for_tests() const { return job_.active(); }
  void on_tick_for_tests() { on_refresh(); }
  SortBy sort_by_for_tests() const { return pane().sort_by; }
  bool sort_desc_for_tests() const { return pane().sort_desc; }
  const std::string& edit_for_tests() const { return edit_; }
  const std::set<std::string>& marked_for_tests() const {
    return pane().marked;
  }

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
  Pane& pane() { return panes_[active_]; }
  const Pane& pane() const { return panes_[active_]; }
  Pane& other() { return panes_[1 - active_]; }
  // La largeur d'un panneau : toute la fenêtre, ou sa moitié moins la
  // cloison qui les sépare.
  int pane_width() const;
  // Dessine UN panneau dans sa vue.  dit lequel a la main : sans
  // cette marque, une fenetre scindee ne dit pas ou la frappe ira.
  void draw_pane(View v, const Pane& pn, bool focused);
  void render_panes(View v);
  // Le liseré des raccourcis, et où chacun mène. UN SEUL calcul, partagé
  // par le dessin et par le clic, comme partout ailleurs dans ce projet.
  struct Place {
    std::string label;
    std::string path;
  };
  static const std::vector<Place>& places();
  void draw_places(View v) const;
  int rows_for_list() const;
  // La géométrie des colonnes, UN SEUL calcul partagé par le dessin et par
  // le clic sur l'en-tête. Une largeur nulle veut dire « cette colonne a
  // cédé la place » : sur une fenêtre étroite, les chiffres partent AVANT
  // les noms -- un nom coupé à trois lettres ne sert à rien, une taille
  // absente se retrouve ailleurs.
  struct Columns {
    int name_w = 0;
    int size_x = 0;
    int size_w = 0;
    int date_x = 0;
    int date_w = 0;
  };
  Columns columns(int w) const;
  // Trie par cette colonne, ou inverse le sens si c'est déjà la sienne.
  void sort_on(SortBy by);
  void activate();
  void go_up();
  // Charge ce répertoire SANS toucher à l'historique. Rend false et laisse
  // tout en place si la lecture échoue -- descendre dans un répertoire
  // illisible pour y afficher une liste vide donnerait l'impression d'un
  // dossier vide. `came_from` sert à reposer le curseur sur le dossier
  // d'où l'on sort : le remettre en tête obligerait à le retrouver dans
  // une liste de deux cents entrées.
  bool load(const std::string& path, const std::string& came_from);
  // Va là, et retient d'où l'on vient. TOUT déplacement passe par ici --
  // descendre, remonter, cliquer le fil d'Ariane -- sinon `Alt+flèche` ne
  // saurait défaire que la moitié des gestes.
  void go_to(const std::string& path);
  void go_back();
  void go_forward();

  // Le fil d'Ariane : un segment cliquable par niveau, avec le chemin
  // complet qu'il désigne. UN SEUL calcul, partagé par le dessin et par le
  // clic, comme partout ailleurs dans ce projet.
  struct Segment {
    int x = 0;
    int w = 0;
    std::string path;
  };
  std::vector<Segment> path_segments(const Pane& pn, int w) const;
  // Le nom sélectionné, ou une chaîne vide si la sélection ne désigne rien
  // qu'on ait le droit de toucher -- `..` en particulier.
  std::string touchable_selection() const;
  void commit_rename();
  // Crée ce que `creating_dir_` dit, sous le nom saisi. Un gestionnaire
  // qui ne sait que détruire oblige à sortir dans un terminal pour la
  // moitié du travail.
  void commit_create();
  // Met la sélection au presse-papiers. `cut` dit si le coller déplacera.
  void take_clipboard(bool cut);
  void paste_clipboard();
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

  // DEUX PANNEAUX, toujours construits ; le second ne se voit que scindé.
  // Les garder tous deux vivants évite d'avoir à recharger le disque à
  // chaque bascule de `F3`.
  Pane panes_[2];
  size_t active_ = 0;
  bool split_ = false;
  bool places_ = false;
  bool show_hidden_ = false;
  // D'OÙ L'ON VIENT, et ce qu'on vient de défaire. Une nouvelle descente
  // efface la seconde : garder une branche qu'on vient d'abandonner ferait
  // avancer `Alt+droite` vers un dossier sans rapport avec là où l'on est.
  // LES NOMS, PAS LES RANGS. Le filtre et le tri renumérotent la liste sous
  // les pieds de la sélection ; un rang marqué désignerait alors un autre
  // fichier. Vidé à chaque changement de répertoire : les noms d'avant
  // auraient des homonymes ici, et l'action porterait sur eux.
  Mode mode_ = Mode::Normal;
  // Le nom en cours de saisie pendant un renommage.
  std::string edit_;
  // Ce qu'on est en train de créer : un dossier, ou un fichier vide. Deux
  // touches éloignées pour deux choses aussi proches se retiendraient mal ;
  // c'est le même geste, et `Maj` en change la sorte.
  bool creating_dir_ = true;
  // DES CHEMINS ABSOLUS, pas des noms : retenir des noms ferait coller
  // depuis le mauvais répertoire dès qu'on aurait navigué entre les deux
  // gestes -- c'est-à-dire toujours.
  std::vector<std::string> clipboard_;
  bool clipboard_cut_ = false;
  CopyJob job_;
  Size size_{40, 12};
  Host* host_ = nullptr;
};

}  // namespace sshos
