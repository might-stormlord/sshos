#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
  Size min_size() const override { return {20, 5}; }
  CloseCheck can_close() const override;

  // --- ParserSink ---
  void print(char32_t c) override;
  void execute(uint8_t byte) override;
  void csi(const Params& params, std::string_view intermediates,
           uint8_t final_byte) override;
  void esc(std::string_view intermediates, uint8_t final_byte) override;
  void osc(std::string_view data) override;

  // --- pour les tests ---
  // Le liant n'a AUCUN effet observable sans un vrai PTY : ces accès
  // permettent de le nourrir d'octets comme s'ils en venaient, et de lire
  // ce qu'il en a fait. Aucun code de production ne doit s'en servir.
  void feed_for_tests(std::string_view bytes);
  const Screen& screen_for_tests() const { return screen_; }
  const Scrollback& scrollback_for_tests() const { return history_; }
  const Modes& modes_for_tests() const { return modes_; }
  std::string take_written_for_tests();
  pid_t pid_for_tests() const { return pty_.pid(); }

 private:
  // Écrit vers l'invité. Sans PTY -- en test, ou après la mort de
  // l'enfant -- les octets sont retenus au lieu d'être perdus : c'est ce
  // qui rend l'encodage vérifiable sans lancer de shell.
  void to_guest(std::string_view bytes);

  // Applique ce que l'invité a demandé et qui a un effet mécanique sur la
  // grille. Le registre reste la seule source de vérité ; ceci n'en est
  // que la conséquence.
  void sync_modes();

  Pty pty_;
  Parser parser_{*this};
  Screen screen_{80, 24};
  Scrollback history_;
  Modes modes_;

  Host* host_ = nullptr;
  uint64_t token_ = 0;
  bool watching_ = false;
  // Retenu quand il n'y a pas de PTY sous la main.
  std::string pending_;
  // L'erreur de démarrage, s'il y en a eu une. Elle s'affiche dans la
  // fenêtre au lieu de laisser une grille vide inexplicable.
  std::string spawn_error_;
  Size size_{80, 24};
  std::vector<std::string> argv_;
};

}  // namespace sshos
