#pragma once

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "input/events.hpp"
#include "render/cell.hpp"
#include "render/surface.hpp"

namespace sshos {

// Réponse d'une application à « puis-je te fermer ? ». Le refus porte sa
// propre question : c'est l'application qui sait ce qu'elle risque de
// perdre, pas le gestionnaire de fenêtres.
struct CloseCheck {
  bool allowed = true;
  std::string question;

  static CloseCheck allow() { return {}; }
  static CloseCheck ask(std::string q) { return {false, std::move(q)}; }
};

// Ce qu'une application répond après avoir traité un événement sur l'un de
// ses descripteurs. `Closed` veut dire « cette source est morte » : l'hôte
// retire la surveillance et l'application décide ensuite si elle veut
// vivre sans.
enum class IoStatus { Ok, Closed };

// Ce qu'une application a le droit de demander au bureau. Elle ne reçoit
// jamais l'epoll ni son propre numéro de fenêtre : watch() rend un jeton
// opaque, que on_io() lui rendra tel quel.
class App;

class Host {
 public:
  virtual ~Host() = default;
  virtual void set_title(std::string title) = 0;
  virtual void request_close() = 0;
  virtual void invalidate() = 0;
  virtual uint64_t watch(int fd, uint32_t events) = 0;
  virtual void unwatch(uint64_t token) = 0;

  // « Préviens-moi quand CET enfant meurt. » L'application ne récolte
  // jamais elle-même : `waitpid` est global au processus, et deux
  // applications qui appelleraient `waitpid(-1)` chacune de leur côté se
  // voleraient mutuellement leurs enfants.
  virtual void watch_child(pid_t pid) = 0;

  // « OUVRE ÇA DANS SA PROPRE FENÊTRE. » Une application ne sait pas
  // ouvrir de fenêtre -- elle ne connaît ni le gestionnaire ni la zone de
  // travail -- mais elle sait fabriquer une autre application. C'est ce
  // qui permet au gestionnaire de fichiers d'ouvrir un fichier dans
  // l'éditeur sans rien savoir du bureau.
  //
  // Défaut : ne rien faire. Un hôte de test n'a pas de bureau derrière
  // lui, et une application qui demande poliment ne doit pas planter là où
  // personne ne peut la servir.
  virtual void open_app(std::unique_ptr<App> app, std::string app_id) {
    (void)app;
    (void)app_id;
  }

  // OU S'OUVRE UN NOUVEAU TERMINAL. C'est un reglage de l'UTILISATEUR, garde
  // par le bureau et ecrit sur disque : une application ne sait ni ou vit ce
  // fichier ni quand le relire, et deux applications qui le liraient chacune
  // de leur cote finiraient par ne plus etre d'accord.
  //
  // `start_dir()` rend le chemin EFFECTIF -- `~` developpe, dossier de
  // l'utilisateur a defaut -- c'est celui qu'on donne a un PTY.
  // `configured_start_dir()` rend ce que l'utilisateur a TAPE, qui est ce
  // qu'on lui remontre quand il rouvre la saisie.
  //
  // Defaut : vide de part et d'autre. Un hote de test n'a pas de
  // configuration derriere lui, et vide veut dire « la ou on est ».
  virtual std::string start_dir() const { return {}; }
  virtual std::string configured_start_dir() const { return {}; }
  virtual void set_start_dir(std::string dir) { (void)dir; }
};

// Le contrat applicatif du projet, valable jusqu'au jalon 6. Tout est
// virtuel avec un défaut utilisable : une application qui ne sait que
// dessiner n'a qu'une méthode à écrire.
class App {
 public:
  virtual ~App() = default;

  // Appelée une fois, à l'ouverture de la fenêtre. La référence reste
  // valable tant que l'application vit.
  virtual void attach(Host& host) { (void)host; }

  // La View est déjà clippée sur la zone cliente : écrire en dehors est
  // ignoré, pas une erreur. C'est ce qui rend structurellement impossible
  // qu'une application peigne par-dessus la barre des tâches.
  virtual void render(View v) = 0;

  virtual void on_key(const KeyEvent& k) { (void)k; }

  // Coordonnées LOCALES à la zone cliente, jamais celles de l'écran.
  virtual void on_mouse(const MouseEvent& m) { (void)m; }

  virtual void on_resize(Size s) { (void)s; }

  // L'enfant confié à `Host::watch_child()` est mort. `status` est celui
  // de `waitpid`, brut : c'est l'application qui sait comment le présenter.
  virtual void on_child_exit(int status) { (void)status; }

  // Une commande nommée venue du menu. Le bureau ne connaît pas les
  // applications ; il leur transmet une chaîne, et celle qui la comprend
  // agit. C'est ce qui évite au menu d'avoir à connaître le TYPE de
  // l'application focalisée -- et donc à la session de dépendre de apps/.
  virtual void on_command(std::string_view cmd) { (void)cmd; }

  virtual IoStatus on_io(uint64_t token, uint32_t events) {
    (void)token;
    (void)events;
    return IoStatus::Ok;
  }

  // `out` en coordonnées locales. Rend false si l'application ne veut pas
  // de curseur visible.
  virtual bool wants_cursor(Pos& out) const {
    (void)out;
    return false;
  }

  // Porte sur la ZONE CLIENTE, pas sur le cadre. Le gestionnaire de
  // fenêtres y ajoute les décorations (voir frame_min, tâche 3).
  virtual Size min_size() const { return {10, 3}; }

  // « Reveille-moi tous les N millisecondes tant que je suis VISIBLE. »
  // -1, le defaut, veut dire « seulement quand quelque chose se passe » --
  // ce qu'attend toute application qui ne fait que reagir aux touches.
  //
  // La visibilite n'est pas a l'application de la juger : c'est la session
  // qui ignore les fenetres minimisees en collectant ces delais. Une
  // application qui devrait le deviner elle-meme finirait par se tromper,
  // et le seul defaut possible ici est de consommer du temps de calcul
  // pour une fenetre que personne ne regarde.
  virtual int refresh_ms() const { return -1; }

  // LE TRAVAIL PÉRIODIQUE, hors du rendu. `render()` ne doit toucher ni au
  // disque ni au réseau -- c'est la règle qui garde le démon vivant -- donc
  // une application qui a quelque chose à faire régulièrement le fait ICI.
  // Appelée quand la minuterie du démon échoit, jamais pendant une
  // composition.
  virtual void on_refresh() {}

  virtual CloseCheck can_close() const { return CloseCheck::allow(); }
};

}  // namespace sshos
