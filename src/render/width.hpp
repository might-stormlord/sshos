#pragma once

#include <string_view>

namespace sshos {

// 0 (combinant, contrôle), 1 (normal) ou 2 (pleine chasse).
// Table embarquée, jamais wcwidth() : wcwidth dépend de la locale du DÉMON,
// qui n'a aucune raison d'être celle du client — le démon tourne détaché,
// avec l'environnement fossilisé de la première session SSH.
int char_width(char32_t cp);

// Politique East Asian Ambiguous. Étroit par défaut ; le client la fixe à
// l'attache après sonde (spec §4.1).
void set_ambiguous_wide(bool wide);
bool ambiguous_wide();

// Cellules occupées par une chaîne UTF-8 entière. Tout composant qui aligne
// du texte doit passer par ici : compter les octets donnerait deux cellules
// à « è », qui n'en prend qu'une, et compter les points de code n'en
// donnerait qu'une à un idéogramme, qui en prend deux.
int text_cells(std::string_view s);

// Coupe à `cells` cellules au plus, jamais au milieu d'une séquence UTF-8 ni
// au milieu d'un caractère de pleine chasse, et signale la coupure avec
// `mark` -- dont la largeur est prise SUR le budget, jamais en plus : une
// élision qui déborde de la place qu'on lui a donnée écrase ce qu'il y a à
// côté, et dans un cadre, ce qu'il y a à côté est la bordure.
std::string elide_to_cells(std::string_view s, int cells, std::string_view mark);

}  // namespace sshos
