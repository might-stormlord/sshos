#pragma once

#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

namespace sshos {

// Ce qu'il faut pour lancer un invité. Tout est décidé AVANT le fork :
// entre fork et execve, seules les fonctions sûres vis-à-vis des signaux
// sont permises, et allouer n'en fait pas partie.
struct PtySpawn {
  std::string path;
  std::vector<std::string> argv;
  std::vector<std::string> env;  // « CLE=valeur », cf. child_env()
  unsigned short cols = 80;
  unsigned short rows = 24;
};

// Un pseudo-terminal et le processus qui vit dedans. Ne connaît ni le
// rendu, ni l'epoll : il rend un descripteur et un pid, et c'est
// l'application qui les confie à son Host.
class Pty {
 public:
  Pty() = default;
  ~Pty();
  Pty(const Pty&) = delete;
  Pty& operator=(const Pty&) = delete;

  // Rend un message d'erreur en français, vide en cas de succès. Un exec
  // raté est rapporté ICI, pas découvert plus tard par une fenêtre vide :
  // l'enfant écrit son errno dans un tuyau CLOEXEC que l'exec réussi
  // referme tout seul.
  std::string spawn(const PtySpawn& s);

  int master() const { return master_; }
  pid_t pid() const { return pid_; }

  void resize(unsigned short cols, unsigned short rows);

  // Non bloquants tous les deux : la boucle du démon est mono-thread, et un
  // read() bloquant sur un terminal muet gèlerait toutes les sessions.
  // Rendent -1 sans rien dire de plus quand il n'y a rien à faire.
  ssize_t read(char* buf, size_t n);
  ssize_t write(const char* buf, size_t n);

  // On ne ferme JAMAIS le maître sur simple réception de SIGCHLD : cela
  // jetterait la sortie encore en tampon dans la discipline de ligne. Les
  // noyaux récents livrent d'abord les données puis rendent EIO -- il
  // suffit de drainer, et de fermer quand le drainage est fini.
  void close_master();

  // SIGHUP au GROUPE de processus, pas au seul enfant : un shell a des
  // petits-enfants, et ne prévenir que lui laisse la compilation tourner.
  void hangup();
  void kill_now();

  // LA FERMETURE, EN UN SEUL ENDROIT. Le destructeur l'appelle, et tout
  // code qui se débarrasse d'un pseudo-terminal avant l'heure doit
  // l'appeler aussi : deux politiques de fermeture finissent toujours par
  // diverger, et celle qui oublie laisse un processus injoignable derrière
  // elle. Ce qu'elle fait, et ce que chaque étape a coûté, est mesuré en
  // face de sa définition.
  void shutdown();

  // Récolte sans bloquer. Rend true la fois où l'enfant est effectivement
  // récolté, false ensuite : l'appelant peut donc boucler dessus sans
  // tenir de compte.
  bool try_reap();

  // Les deux drapeaux de fin, INDÉPENDANTS DANS LES DEUX SENS : un
  // « nohup … & » garde l'esclave ouvert après la mort du shell, et un
  // enfant qui se démonise ferme l'esclave avant sa propre mort.
  bool exited() const { return exited_; }
  int exit_code() const { return code_; }
  bool killed_by_signal() const { return signalled_; }
  // `saw_eof()` N'A AUCUN LECTEUR, et ce n'est pas un oubli : sous Linux,
  // un maître dont le dernier esclave s'est fermé rend EIO (-1), pas 0, si
  // bien que `note_eof()` n'est atteinte que dans des cas de bord. Le
  // drapeau reste parce que la distinction ci-dessus est vraie et qu'un
  // lecteur futur en aura besoin -- pas parce qu'il sert aujourd'hui.
  bool saw_eof() const { return eof_; }
  void note_eof() { eof_ = true; }

 private:
  int master_ = -1;
  pid_t pid_ = -1;
  bool exited_ = false;
  bool signalled_ = false;
  bool eof_ = false;
  int code_ = 0;
};

}  // namespace sshos
