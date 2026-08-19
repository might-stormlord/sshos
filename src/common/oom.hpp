#pragma once

namespace sshos {

// LE TUEUR DE MEMOIRE DU NOYAU, ET QUI IL DOIT EPARGNER.
//
// Quand la machine manque de memoire, le noyau choisit une victime d'apres
// un score. Le demon pese quelques megaoctets et porte pourtant TOUT l'etat
// du bureau -- fenetres, historiques, shells : le sacrifier pour recuperer
// six megaoctets detruit une session de travail entiere pour rien. Mesure
// faite le 18 aout 2026 sur l'installation de l'utilisateur : le demon a
// 6,4 Mo affichait un `oom_score` de 666 contre 704 pour un processus de
// 543 Mo. A 6 % pres, le noyau les traitait comme aussi sacrifiables l'un
// que l'autre.
//
// -1000 est la valeur documentee de « jamais celui-la » (proc(5)).
inline constexpr int kOomProtected = -1000;

// Met le processus courant hors d'atteinte. Rend false si le noyau refuse
// ou si /proc est absent. L'echec n'est jamais fatal : un demon non protege
// reste un demon.
//
// TOUTE VALEUR NEGATIVE DEMANDE CAP_SYS_RESOURCE, ce qui n'est PAS la meme
// chose qu'etre root : un conteneur Docker tourne en root et retire
// pourtant cette capacite de son jeu par defaut. Le demon y demarre donc
// sans protection, et c'est le comportement voulu -- mesure sur la CI de ce
// depot, ou la confusion entre les deux a fait echouer un test qui passait
// sur la machine de developpement.
bool protect_from_oom();

// Rend un enfant ORDINAIRE de nouveau, et c'est indispensable : le reglage
// s'HERITE a travers fork() et survit a execve(). Sans cet appel, un
// `make -j12` lance dans une fenetre du bureau heriterait de l'immunite du
// demon, et le noyau irait tuer PostgreSQL a sa place.
//
// Appelable entre fork() et execve() : n'alloue rien et n'appelle que
// open/write/close. Remonter de -1000 a 0 est toujours permis, meme sans
// privilege -- seule la descente est gardee.
void drop_oom_protection();

}  // namespace sshos
