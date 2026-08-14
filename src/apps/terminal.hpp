#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include "app/app.hpp"
#include "pty/pty.hpp"
#include "vt/attrs.hpp"
#include "vt/charset.hpp"
#include "vt/modes.hpp"
#include "vt/parser.hpp"
#include "vt/screen.hpp"
#include "vt/scrollback.hpp"
#include "vt/sink.hpp"

namespace sshos {

// LE LIANT. Un pseudo-terminal d'un côté, une grille de l'autre, et rien
// entre les deux qui ne soit interprété : aucun octet venu de l'invité
// n'est jamais relayé tel quel vers le client. Le démon lit, met à jour sa
// grille, et re-synthétise sa propre sortie. C'est ce qui rend
// structurellement impossible qu'un `vim` dans une fenêtre perturbe le
// bureau -- pas plus qu'un programme ne perturbe le compositeur d'un vrai
// système.
//
// La classe implémente `ParserSink` : c'est elle qui traduit les appels de
// la machine à états en ordres pour l'écran, et elle seule qui connaît les
// deux côtés.
class Terminal : public App, public ParserSink {
 public:
  // UN ONGLET : son pseudo-terminal, sa grille, son historique, ses modes.
  // Tout est par onglet et rien n'est partage -- deux onglets sont deux
  // terminaux, pas deux vues du meme.
  //
  // Le parseur de chaque onglet appelle le MEME puits (le Terminal), qui
  // ne saurait donc pas de qui viennent les octets. D'ou `feeding_` :
  // pose juste avant `feed()`, il dit a quel onglet appartient ce qui
  // arrive. Un seul thread, un seul appel a la fois : le pointeur ne peut
  // pas etre pris de vitesse.
  struct Tab {
    explicit Tab(Terminal& owner) : parser(owner) {}
    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;

    Pty pty;
    Parser parser;
    Screen screen{80, 24};
    Scrollback history;
    Modes modes;
    uint64_t token = 0;
    bool watching = false;
    std::string pending;
    std::string spawn_error;
    // Le titre AFFICHE. Vide veut dire « celui que l'invite a pose », ou a
    // defaut le numero -- renommer est un choix de l'utilisateur, et il
    // doit survivre au prochain `OSC 2` du shell.
    std::string custom_title;
    std::string guest_title;
  };
  Terminal();
  // La commande à lancer. Vide -- le cas normal -- veut dire « le shell de
  // connexion de l'utilisateur », lu dans `getpwuid()`. Le constructeur
  // explicite sert aux tests, qui ont besoin d'un enfant dont ils
  // choisissent la durée de vie, et servira au jour où le menu proposera
  // « lancer telle commande ».
  explicit Terminal(std::vector<std::string> argv);
  ~Terminal() override;

  // --- App ---
  void attach(Host& host) override;
  void render(View v) override;
  void on_key(const KeyEvent& k) override;
  void on_mouse(const MouseEvent& m) override;
  void on_resize(Size s) override;
  void on_child_exit(int status) override;
  IoStatus on_io(uint64_t token, uint32_t events) override;
  bool wants_cursor(Pos& out) const override;
  // Une ligne de plus qu'avant : la barre d'onglets la prend, et un
  // terminal reduit a sa taille minimale doit garder une grille utile.
  Size min_size() const override { return {20, 6}; }
  CloseCheck can_close() const override;

  // --- ParserSink ---
  void print(char32_t c) override;
  void execute(uint8_t byte) override;
  void csi(const Params& params, std::string_view intermediates,
           uint8_t final_byte) override;
  void esc(std::string_view intermediates, uint8_t final_byte) override;
  void osc(std::string_view data) override;

  // Ce que le Terminal est en train de faire.
  enum class Mode { Normal, Renaming };

  // --- pour les tests ---
  // Le liant n'a AUCUN effet observable sans un vrai PTY : ces accès
  // permettent de le nourrir d'octets comme s'ils en venaient, et de lire
  // ce qu'il en a fait. Aucun code de production ne doit s'en servir.
  void feed_for_tests(std::string_view bytes);
  const Screen& screen_for_tests() const { return active().screen; }
  const Scrollback& scrollback_for_tests() const { return active().history; }
  const Modes& modes_for_tests() const { return active().modes; }
  std::string take_written_for_tests();
  pid_t pid_for_tests() const { return active().pty.pid(); }
  size_t tab_count_for_tests() const { return tabs_.size(); }
  size_t active_tab_for_tests() const { return active_; }
  std::string tab_label_for_tests(size_t i) const;
  Mode mode_for_tests() const { return mode_; }

 private:
  // Écrit vers l'invité. Sans PTY -- en test, ou après la mort de
  // l'enfant -- les octets sont retenus au lieu d'être perdus : c'est ce
  // qui rend l'encodage vérifiable sans lancer de shell.
  void to_guest(std::string_view bytes);

  Tab& active() { return *tabs_[active_]; }
  const Tab& active() const { return *tabs_[active_]; }
  // L'onglet que le parseur est en train de nourrir, ou l'actif hors
  // lecture -- c'est ce qui rend `feed_for_tests()` utilisable.
  Tab& target();
  // Ouvre un onglet et le rend actif. Rend false si le shell n'a pas
  // demarre : mieux vaut rester sur celui qui marche.
  bool open_tab();
  // Lance un shell dans cet onglet. Rend false si l'exec a echoue.
  bool open_tab_into(Tab& t);
  void close_tab(size_t i);
  void select_tab(size_t i);
  // Avance de `d` onglets, en BOUCLANT : sans cela, la moitie des onglets
  // seraient hors d'atteinte d'un seul geste.
  void cycle_tab(int d);
  // Redonne a chaque onglet la taille de sa grille -- la barre en mange une
  // ligne, et l'onglet qu'on ne regarde pas doit la perdre aussi.
  void relayout();
  // Le nom affiche d'un onglet : le sien s'il a ete renomme, sinon celui
  // que l'invite a pose, sinon son numero.
  std::string display_label(size_t i) const;
  // Renomme la FENETRE d'apres l'onglet regarde. Le cadre doit dire ce
  // qu'on regarde, pas ce qu'on regardait.
  void retitle();
  void begin_rename();
  void rename_key(const KeyEvent& k);

  // La geometrie de la barre. UN SEUL calcul, partage par le dessin et par
  // le clic : deux calculs separes finissent toujours par diverger d'une
  // cellule, et un onglet qu'on ne peut pas cliquer la ou on le voit est
  // pire qu'une barre absente.
  enum class SlotKind { Select, Close, New };
  struct Slot {
    int x = 0;
    int w = 0;
    size_t tab = 0;
    SlotKind kind = SlotKind::Select;
    std::string text;
  };
  std::vector<Slot> bar_slots() const;
  void draw_bar(View v) const;
  void bar_click(int x);

  // Applique ce que l'invité a demandé et qui a un effet mécanique sur la
  // grille. Le registre reste la seule source de vérité ; ceci n'en est
  // que la conséquence.
  void sync_modes();

  std::vector<std::unique_ptr<Tab>> tabs_;
  // LES ONGLETS FERMES QUI N'ONT PAS ENCORE ETE RECOLTES. Fermer le maitre
  // tue le shell -- le noyau envoie SIGHUP au groupe -- mais sa depouille
  // attend un waitpid(), et l'onglet qui la portait n'est plus la pour le
  // faire. Sans cette antichambre, chaque onglet ferme laisserait un
  // zombie derriere lui pour toute la vie du demon.
  std::vector<std::unique_ptr<Tab>> closing_;
  size_t active_ = 0;
  Tab* feeding_ = nullptr;
  Mode mode_ = Mode::Normal;
  std::string edit_;

  Host* host_ = nullptr;
  Size size_{80, 24};
  std::vector<std::string> argv_;
};

}  // namespace sshos
