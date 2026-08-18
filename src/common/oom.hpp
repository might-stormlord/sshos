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
// -- toute valeur NEGATIVE demande CAP_SYS_RESOURCE -- ou si /proc est
// absent. L'echec n'est jamais fatal : un demon non protege reste un demon.
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
