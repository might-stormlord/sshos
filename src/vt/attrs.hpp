#pragma once

#include "render/cell.hpp"
#include "vt/sink.hpp"

namespace sshos {

// SGR : les paramètres d'un `CSI ... m` appliqués au style courant.
//
// Le type de sortie est le `Style` du rendu, pas un type VT parallèle.
// `ColorKind{Default, Indexed, Rgb}` dit déjà exactement ce que SGR sait
// dire, et le pont vers l'écran (tâche 13) devient une copie de champ à
// champ au lieu d'une table de conversion entretenue à deux endroits.
// C'est le sens inverse du choix fait pour `ScreenCell`, qui ne partage
// RIEN avec `Cell` : une cellule de grille et une cellule de rendu ont des
// invariants différents, une couleur n'en a qu'un.
//
// LA RÈGLE CENTRALE : la boucle avance par GROUPES, pas par paramètres.
// Un groupe est un code et ses sous-paramètres -- ce que le parseur marque
// avec `Param::sub`, c'est-à-dire ce qui suivait un `:`. Sans cela,
// `CSI 4:3m` (souligné ondulé) ferait lire le `3` comme un italique, et
// `CSI 58:2::255:0:0m` (couleur de soulignement) déverserait cinq nombres
// dans la boucle, dont un `0` qui remettrait tout à zéro au milieu de la
// séquence. Les codes qu'on n'interprète pas doivent donc quand même être
// CONSOMMÉS entiers : c'est la seule façon de ne pas décaler la lecture de
// ceux qui suivent.
//
// Les couleurs étendues existent sous deux formes, les deux dans la
// nature : `38;5;n` et `38;2;r;g;b` (xterm, tout à plat) contre `38:5:n` et
// `38:2::r:g:b` (ISO 8613-6, en sous-paramètres, avec un champ d'espace
// colorimétrique vide au milieu). La forme à plat se lit par position ; la
// forme en sous-paramètres se lit en prenant les TROIS DERNIERS éléments du
// groupe, ce qui traite `38:2::r:g:b` et `38:2:r:g:b` sans avoir à compter
// les champs vides.
//
// `58` et `59` (couleur du soulignement) sont consommés et jetés : le
// rendu n'a pas de champ pour eux. Les ignorer sans les consommer serait
// le bogue décrit plus haut.
//
// `21` vaut DOUBLE SOULIGNEMENT, pas « gras éteint ». L'ECMA-48 dit l'un,
// xterm fait l'autre, et c'est xterm que `TERM=xterm-256color` promet à
// l'invité -- lui mentir sur ce point le ferait écrire en gras là où il
// demande à en sortir.
//
// Une liste vide vaut `SGR 0` : `CSI m` remet tout à zéro.
void apply_sgr(const Params& params, Style& style);

}  // namespace sshos
