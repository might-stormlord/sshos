#pragma once

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

}  // namespace sshos
