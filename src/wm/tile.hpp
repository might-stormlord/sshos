#pragma once

#include <vector>

#include "render/cell.hpp"

namespace sshos {

// LE RANGEMENT DES FENÊTRES. Rend une géométrie par fenêtre, dans l'ordre
// où on les lui donne, remplissant `work` sans trou ni chevauchement.
//
// La disposition est une GRILLE EN COLONNES, choisie pour trois raisons :
// elle est prévisible (on sait où sa fenêtre va atterrir), elle donne le
// cas à deux fenêtres que tout le monde attend -- deux moitiés pleine
// hauteur -- et elle n'a pas de « fenêtre maîtresse » dont la disparition
// réorganiserait tout le reste.
//
//   1 fenêtre  : toute la zone
//   2 fenêtres : deux moitiés, côte à côte, pleine hauteur
//   3 fenêtres : la colonne de gauche coupée en deux, celle de droite
//                pleine hauteur
//   4 fenêtres : deux colonnes de deux
//
// Les restes de division vont aux PREMIÈRES colonnes et aux PREMIÈRES
// lignes : sur une largeur impaire, c'est la fenêtre de gauche qui gagne
// la colonne en trop. Réparti autrement, un aller-retour de rangement
// ferait glisser les fenêtres d'une cellule à chaque fois.
std::vector<Rect> tile_rects(const Rect& work, int count);

// Le cote vers lequel on ancre une fenetre.
enum class SnapDir { Left, Right, Up, Down };

// LA MOITIE de la zone de travail, du cote demande -- le geste d'ancrage
// des bureaux modernes. Gauche et droite donnent une demi-largeur PLEINE
// HAUTEUR ; haut et bas, une demi-hauteur pleine largeur.
//
// Le reste d'une division impaire va a la moitie GAUCHE (ou HAUTE) : deux
// fenetres ancrees dos a dos doivent se toucher sans laisser de colonne
// vide entre elles, et sans se chevaucher.
Rect snap_rect(const Rect& work, SnapDir d);

// La moitie qui RESTE libre apres un ancrage. C'est la que le bureau
// proposera l'autre fenetre.
Rect snap_opposite(const Rect& work, SnapDir d);

}  // namespace sshos
