#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace sshos {

// Un paramètre de séquence CSI ou DCS.
//
// `-1` veut dire ABSENT, ce qui n'est pas « zéro » : `\033[H` va en 1,1
// tandis que `\033[0;0H` demande explicitement la ligne 0 et la colonne 0,
// que la spec VT rabat sur 1,1 -- même résultat ici, mais `\033[;5H` a un
// premier paramètre absent et un second à 5, et confondre les deux fait
// atterrir le curseur à la mauvaise ligne.
//
// `sub` distingue `38:2::r:g:b` de `38;2;r;g;b`. Les deux formes existent
// dans la nature ; la seconde est celle de xterm, la première celle de
// l'ISO 8613-6, que produisent plusieurs bibliothèques modernes.
struct Param {
  int value = -1;
  bool sub = false;
};

using Params = std::vector<Param>;

// La valeur d'un paramètre, ou son défaut s'il est absent ou hors liste.
int param_or(const Params& p, size_t index, int fallback);

// Ce que la machine à états appelle. AUCUNE notion de grille, de couleur
// ni de curseur : c'est ce qui rend la machine testable par un mouchard qui
// enregistre les appels, et fuzzable sans rien monter autour.
//
// Tout a un défaut vide : un puits qui ne s'intéresse qu'aux caractères
// imprimables n'écrit qu'une méthode.
struct ParserSink {
  virtual ~ParserSink() = default;

  // Un caractère imprimable, DÉJÀ décodé depuis UTF-8.
  virtual void print(char32_t c) { (void)c; }

  // Un octet de commande C0 : \n, \r, \b, \t, \a…
  virtual void execute(uint8_t byte) { (void)byte; }

  // `\033[` … final. Les intermédiaires incluent le marqueur privé
  // (`?`, `>`, `<`, `=`) quand il y en a un, en tête.
  virtual void csi(const Params& params, std::string_view intermediates,
                   uint8_t final_byte) {
    (void)params;
    (void)intermediates;
    (void)final_byte;
  }

  // `\033` + intermédiaires + final : `\033(0`, `\0337`, `\033M`…
  virtual void esc(std::string_view intermediates, uint8_t final_byte) {
    (void)intermediates;
    (void)final_byte;
  }

  // La chaîne complète d'un OSC, terminateur exclu. `OSC 0` et `OSC 2`
  // alimentent la barre de titre ; le reste est ignoré par le liant, mais
  // avalé proprement par la machine.
  virtual void osc(std::string_view data) { (void)data; }

  // Le projet n'interprète aucun DCS, mais les traverser proprement évite
  // qu'un DCS non terminé -- ce que produit un `tmux` imbriqué à chaque
  // requête de capacité -- se mette à manger l'écran caractère par
  // caractère.
  virtual void dcs_start(const Params& params, std::string_view intermediates,
                         uint8_t final_byte) {
    (void)params;
    (void)intermediates;
    (void)final_byte;
  }
  virtual void dcs_data(std::string_view chunk) { (void)chunk; }
  virtual void dcs_end() {}
};

}  // namespace sshos
