#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sshos {

// Incrémenté à chaque changement incompatible du protocole. Comparé au
// handshake : mieux vaut un message clair qu'un affichage corrompu.
inline constexpr uint32_t kBuildId = 1;

struct Hello {
  uint32_t build_id = kBuildId;
  uint16_t cols = 0;
  uint16_t rows = 0;
  std::string term;
  std::string colorterm;
  bool utf8 = false;
  std::vector<std::pair<std::string, std::string>> env;
};

struct Welcome {};
struct Incompatible { std::string reason; };
struct Detached { std::string reason; };
struct Input { std::string bytes; };
struct Resize { uint16_t cols = 0; uint16_t rows = 0; };
struct FrameMsg { std::string ansi; };

using Msg = std::variant<Hello, Welcome, Incompatible, Detached, Input, Resize,
                         FrameMsg>;

std::string encode(const Msg& m);

// Plafond de la taille du corps d'UN message (le champ longueur de
// l'enveloppe, pas le total du tampon — voir kMaxBufferBytes plus bas).
//
// Pire cas raisonné : un « repaint » complet d'un terminal énorme — 500
// colonnes x 500 lignes, largement au-delà de tout affichage réel (un
// moniteur 8K avec une fonte minuscule plafonne autour de 480x120) — où
// CHAQUE cellule diffère de la précédente et porte un style truecolor
// complet. C'est le cas le plus défavorable pour le diffeur
// (render/diff.cpp, render/profile.cpp) :
//   - un CUP absolu avant chaque cellule (règle §4.1 : un graphème
//     non-ASCII invalide toujours la position implicite du curseur) :
//     jusqu'à "\033[65535;65535H" = 14 octets ;
//   - une transition SGR complète : reset (4) + 6 attributs (4 octets
//     chacun = 24) + fg 24 bits (20) + bg 24 bits (20) = 68 octets ;
//   - un caractère UTF-8 jusqu'à 4 octets.
// Soit ~86 octets/cellule, arrondi à 100 pour la marge.
// 250 000 cellules x 100 octets ≈ 23,8 Mio. 32 Mio laisse ~34% de marge
// au-dessus de ce calcul, tout en restant loin des gigaoctets qu'un `len`
// 32 bits non borné autoriserait. (cols/rows sont des uint16_t, donc le
// type autoriserait 65535x65535 ; ce n'est pas un « pire cas réel », et
// aucune borne sur cols/rows elles-mêmes n'existe encore ailleurs dans le
// jalon — voir le rapport de relecture.)
inline constexpr size_t kMaxMessageBytes = 32ull * 1024 * 1024;

// Plafond de l'extent NON CONSOMMÉ du tampon de réassemblage (buf_.size()
// - pos_ : les octets déjà consommés en tête ne comptent pas), pas de son
// allocation physique. Doit rester assez grand pour qu'un message légal
// de taille maximale puisse s'assembler même arrivé fragmenté sur de très
// nombreux feed() : l'en-tête (5 octets) + le corps maximal
// (kMaxMessageBytes), plus 1 Mio de marge pour ce qui peut être mis en
// file juste à côté (fin d'un message précédent pas encore consommé,
// début du suivant arrivé dans la même salve).
//
// Conséquence assumée : parce que ce plafond ignore pos_, et que
// compact() ne décale que lorsque pos_ * 2 >= buf_.size() (voir son
// commentaire), buf_.size() lui-même peut atteindre jusqu'à ~2x
// kMaxBufferBytes juste avant qu'un compact() ne se déclenche (pos_ tout
// juste sous la moitié du tampon, plus kMaxBufferBytes d'octets non
// consommés en plus). Ce facteur 2 n'est pas une fuite : c'est le prix
// délibéré d'une compaction à coût amorti linéaire plutôt que quadratique
// — voir le commentaire de compact() (proto.cpp) pour l'analyse complète
// de ce compromis.
inline constexpr size_t kMaxBufferBytes =
    kMaxMessageBytes + 5 + (1ull * 1024 * 1024);

// Décodeur incrémental : les messages arrivent découpés n'importe comment.
//
// C'est la seule frontière de ce jalon qui parse des octets non fiables
// (démon comme client y sont exposés) : next() ne fait jamais confiance à
// une longueur annoncée sans la vérifier, et feed() n'accumule jamais un
// tampon sans borne.
//
// Un tag connu dont le corps est incohérent, ou qui laisse des octets non
// consommés dans l'enveloppe qu'il a lui-même déclarée, est un pair qui
// viole le protocole qu'il prétend parler : la connexion est marquée en
// échec de façon définitive (failed()), à charge pour l'appelant de la
// fermer. Un tag INCONNU reste en revanche sauté-et-continué — ce n'est
// pas la même chose qu'un tag connu mal formé ; voir le commentaire à son
// traitement dans proto.cpp pour la distinction et pourquoi elle ne
// rouvre pas la compatibilité de version.
class Decoder {
 public:
  void feed(std::string_view bytes);
  std::optional<Msg> next();

  // Vrai dès que le pair a violé le protocole : longueur de message hors
  // plafond, tampon de réassemblage hors plafond, ou corps incohérent /
  // avec octets de trop sous un tag connu. Une fois vrai, next() ne
  // renverra plus jamais rien et feed() n'agrandira plus le tampon —
  // c'est à l'appelant de fermer la connexion, le décodeur n'a pas
  // d'autre moyen de le signaler.
  bool failed() const { return failed_; }

  // Diagnostic réservé aux tests : capacité physique actuelle du tampon
  // interne (buf_.capacity()), pour vérifier que la mémoire est bien
  // rendue après un épisode qui l'a fait grossir. N'existe que pour ça —
  // aucun code de production ne doit lire cette valeur.
  size_t buffer_capacity_for_tests() const { return buf_.capacity(); }

 private:
  void fail();
  void compact();

  std::string buf_;
  size_t pos_ = 0;      // octets déjà consommés en tête de buf_
  bool failed_ = false;
};

}  // namespace sshos
