#include "shell/clock.hpp"

#include <cstdio>
#include <ctime>

namespace sshos {

bool Clock::update(const Platform& plat) {
  const std::time_t t = std::chrono::system_clock::to_time_t(plat.now());
  // Heure LOCALE, pas UTC : un panneau qui affiche l'heure doit suivre le
  // fuseau réel de la machine, heure d'été comprise -- jamais un décalage
  // fixe codé en dur (Toronto est à UTC-5 en hiver/EST, UTC-4 en été/EDT ;
  // seule la base de fuseaux tzdata, interrogée par ::localtime_r, connaît
  // la bonne bascule). Un offset constant serait juste faux six mois par
  // an.
  //
  // ::tzset() est nécessaire ici, pas cosmétique : POSIX ne garantit pas que
  // ::localtime_r() l'appelle elle-même, et la glibc ne relit TZ qu'à la
  // première utilisation (vérifié empiriquement -- voir rapport de tâche) :
  // un changement ultérieur de TZ resterait sinon silencieusement ignoré.
  // Coût nominal négligeable : glibc ne re-parse le fichier de zone que si
  // TZ a effectivement changé depuis le dernier appel.
  //
  // Le démon est détaché (double fork + setsid, voir daemonize.cpp) : il ne
  // conserve aucun terminal contrôleur, mais hérite bien de l'environnement
  // du processus qui l'a lancé, TZ compris -- c'est cette valeur, figée au
  // moment du lancement, que ::tzset() lit ici.
  ::tzset();
  std::tm tm{};
  ::localtime_r(&t, &tm);

  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  char day[32];
  // %a et %b suivent la locale du processus ; le démon n'en installe
  // aucune, donc c'est la locale « C » et des abréviations ASCII stables.
  // C'est ce qu'on veut : la largeur de la date reste prévisible sur un
  // panneau vertical de seize colonnes.
  std::strftime(day, sizeof day, "%a %d %b", &tm);

  const bool changed = !primed_ || text_ != buf || date_ != day;
  primed_ = true;
  text_ = buf;
  date_ = day;
  return changed;
}

}  // namespace sshos
