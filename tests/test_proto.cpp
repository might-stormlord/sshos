#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "common/proto.hpp"
#include "harness.hpp"

using namespace sshos;

// Défaut #5 corrigé : l'ancienne version faisait CHECK(out.has_value()) puis
// return out.value() sans condition. CHECK enregistre l'échec et CONTINUE —
// contrairement à un assert, il ne revient jamais. Donc sur un optional vide
// .value() jette bad_optional_access, non rattrapé, et tout le binaire de
// tests s'arrête net (les cas proto sont aux positions 33-36 : tout ce qui
// suit, y compris le résumé, disparaît silencieusement). roundtrip() renvoie
// donc l'optional lui-même ; chaque appelant doit le garder (CHECK) puis
// revenir tôt s'il est vide avant tout déballage. C'est l'habitude la plus
// importante de ce fichier : aucun .value()/*m/std::get sur un optional sans
// un `if (!has_value()) return;` juste avant.
static std::optional<Msg> roundtrip(const Msg& m) {
  Decoder d;
  d.feed(encode(m));
  return d.next();
}

// -- Constructeurs bruts pour fabriquer des entrées hostiles qu'encode() ne
// produirait jamais. Les valeurs de tag reflètent l'enum privée de
// proto.cpp (Hello=1 .. Frame=7) ; à resynchroniser si celle-ci change.
namespace {

constexpr uint8_t kTagWelcome = 2;
constexpr uint8_t kTagInput = 5;
constexpr uint8_t kTagResize = 6;
constexpr uint8_t kTagUnknown = 200;  // hors de 1..7

std::string raw_u32(uint32_t v) {
  std::string o;
  o += static_cast<char>((v >> 24) & 0xFF);
  o += static_cast<char>((v >> 16) & 0xFF);
  o += static_cast<char>((v >> 8) & 0xFF);
  o += static_cast<char>(v & 0xFF);
  return o;
}

std::string raw_header(uint8_t tag, uint32_t len) {
  std::string o;
  o += static_cast<char>(tag);
  o += raw_u32(len);
  return o;
}

std::string raw_msg(uint8_t tag, std::string_view body) {
  return raw_header(tag, static_cast<uint32_t>(body.size())) + std::string(body);
}

}  // namespace

TEST(proto_roundtrips_hello) {
  Hello h;
  h.build_id = 42;
  h.cols = 200;
  h.rows = 50;
  h.term = "xterm-256color";
  h.colorterm = "truecolor";
  h.utf8 = true;
  h.env.emplace_back("SSH_AUTH_SOCK", "/tmp/agent.1");
  const auto out = roundtrip(h);
  CHECK(out.has_value());
  if (!out.has_value()) return;
  const auto got = std::get<Hello>(*out);
  CHECK_EQ(got.build_id, 42u);
  CHECK_EQ(static_cast<int>(got.cols), 200);
  CHECK_EQ(got.term, std::string("xterm-256color"));
  CHECK(got.utf8);
  CHECK_EQ(got.env.size(), static_cast<size_t>(1));
  CHECK_EQ(got.env[0].second, std::string("/tmp/agent.1"));
}

TEST(proto_roundtrips_simple_messages) {
  {
    const auto out = roundtrip(Incompatible{"vieux demon"});
    CHECK(out.has_value());
    if (out.has_value()) {
      const auto got = std::get<Incompatible>(*out);
      CHECK_EQ(got.reason, std::string("vieux demon"));
    }
  }
  {
    const auto out = roundtrip(Input{"\033[A"});
    CHECK(out.has_value());
    if (out.has_value()) {
      const auto got = std::get<Input>(*out);
      CHECK_EQ(got.bytes, std::string("\033[A"));
    }
  }
  {
    const auto out = roundtrip(Resize{80, 24});
    CHECK(out.has_value());
    if (out.has_value()) {
      const auto got = std::get<Resize>(*out);
      CHECK_EQ(static_cast<int>(got.rows), 24);
    }
  }
  {
    const auto out = roundtrip(FrameMsg{"\033[1;1Hx"});
    CHECK(out.has_value());
    if (out.has_value()) {
      const auto got = std::get<FrameMsg>(*out);
      CHECK_EQ(got.ansi, std::string("\033[1;1Hx"));
    }
  }
  {
    const auto out = roundtrip(Welcome{});
    CHECK(out.has_value());
    if (out.has_value()) CHECK(std::holds_alternative<Welcome>(*out));
  }
}

// Le décodeur est nourri de morceaux arbitraires venant de read().
TEST(proto_decoder_survives_byte_by_byte_feeding) {
  const std::string wire = encode(Msg{Input{"hello"}}) + encode(Msg{Resize{80, 24}});
  Decoder d;
  int produced = 0;
  for (char c : wire) {
    d.feed(std::string_view(&c, 1));
    while (auto m = d.next()) ++produced;
  }
  CHECK_EQ(produced, 2);
  CHECK(!d.failed());
}

TEST(proto_decoder_yields_nothing_on_partial_message) {
  const std::string wire = encode(Msg{Input{"hello"}});
  Decoder d;
  d.feed(std::string_view(wire).substr(0, wire.size() - 1));
  CHECK(!d.next().has_value());
  d.feed(std::string_view(wire).substr(wire.size() - 1));
  CHECK(d.next().has_value());
}

// Défaut #1 : `5 + len` calculé en 32 bits boucle pour len proche de
// 0xFFFFFFFF. Ces valeurs sont maintenant interceptées par le plafond par
// message bien avant d'atteindre la moindre addition dangereuse — mais le
// test vérifie le symptôme décrit dans la relecture : aucune de ces
// longueurs ne doit jamais laisser le décodeur croire qu'un message tient
// dans quelques octets. Chaque valeur est testée sur un décodeur neuf, muni
// d'un seul en-tête (pas de corps) : la boucle de next() est bornée (pos_
// avance strictement à chaque itération, ou la fonction retourne), donc ce
// test ne peut pas bloquer — il documente juste qu'il n'y a pas de faux
// message ni de plantage.
TEST(proto_decoder_rejects_wraparound_and_adversarial_lengths) {
  const uint32_t adversarial[] = {
      0xFFFFFFFFu,  // 5 + len déborde à 4 en 32 bits
      0xFFFFFFFBu,  // 5 + len déborde exactement à 0 en 32 bits
      0xFFFFFFFCu,
      0x80000000u,
      static_cast<uint32_t>(kMaxMessageBytes) + 1,
  };
  for (uint32_t len : adversarial) {
    Decoder d;
    d.feed(raw_header(kTagInput, len));
    const auto out = d.next();
    CHECK(!out.has_value());
    CHECK(d.failed());
    // Une fois en échec, le décodeur reste silencieux pour de bon, même si
    // on continue à le nourrir avec un message par ailleurs valide.
    d.feed(encode(Msg{Welcome{}}));
    CHECK(!d.next().has_value());
    CHECK(d.failed());
  }
}

// La limite exacte : kMaxMessageBytes est accepté comme longueur annoncée
// (le décodeur attend simplement le corps, qui n'arrive jamais ici), mais
// kMaxMessageBytes + 1 est un échec immédiat — sans attendre un seul octet
// de corps.
TEST(proto_decoder_accepts_len_at_ceiling_rejects_just_above) {
  Decoder at_ceiling;
  at_ceiling.feed(raw_header(kTagWelcome, static_cast<uint32_t>(kMaxMessageBytes)));
  CHECK(!at_ceiling.next().has_value());
  CHECK(!at_ceiling.failed());

  Decoder above_ceiling;
  above_ceiling.feed(raw_header(kTagWelcome, static_cast<uint32_t>(kMaxMessageBytes) + 1));
  CHECK(!above_ceiling.next().has_value());
  CHECK(above_ceiling.failed());
}

// Défaut #3 : un tag CONNU dont un champ interne ment sur sa propre longueur
// (proto.cpp, Reader::str()) est un pair qui viole le protocole : la
// connexion échoue définitivement (voir le commentaire à Decoder::next()).
// Le message valide est placé AVANT le message menteur dans le flux : cela
// prouve qu'un message déjà entièrement tamponné est délivré sans attendre
// un feed() supplémentaire (l'esprit du défaut #3 — ne pas laisser un
// message valide en souffrance derrière un autre), tout en restant
// cohérent avec la décision "échec de connexion" : rien de valide *après*
// le message menteur ne saurait être récupéré, puisque le pair est déjà
// félon à ce moment-là.
TEST(proto_decoder_delivers_valid_message_before_lying_inner_length_then_fails) {
  std::string lying;
  lying += raw_header(kTagInput, 7);  // enveloppe cohérente : 4 (préfixe) + 3 (contenu) = 7
  lying += raw_u32(100);              // le préfixe interne ment : réclame 100 octets
  lying += "abc";                     // seulement 3 sont réellement présents

  Decoder d;
  d.feed(encode(Msg{Input{"ok"}}) + lying);

  const auto first = d.next();
  CHECK(first.has_value());
  if (first.has_value()) {
    const auto* in = std::get_if<Input>(&*first);
    CHECK(in != nullptr);
    if (in != nullptr) CHECK_EQ(in->bytes, std::string("ok"));
  }
  CHECK(!d.failed());

  CHECK(!d.next().has_value());
  CHECK(d.failed());
}

// Défaut #3 : un tag INCONNU reste sauté-et-continué, avec ou sans corps —
// ce chemin est documenté comme supporté mais n'avait aucun test. Le
// message valide qui suit doit être délivré dans la même passe de drain,
// sans que le décodeur entre en échec.
TEST(proto_decoder_skips_unknown_tags_with_and_without_body_then_continues) {
  Decoder d;
  d.feed(raw_msg(kTagUnknown, ""));     // tag inconnu, corps vide
  d.feed(raw_msg(kTagUnknown, "xyz"));  // tag inconnu, avec corps
  d.feed(encode(Msg{Welcome{}}));

  int produced = 0;
  std::optional<Msg> last;
  while (auto m = d.next()) {
    ++produced;
    last = m;
  }
  CHECK_EQ(produced, 1);
  CHECK(!d.failed());
  CHECK(last.has_value());
  if (!last.has_value()) return;
  CHECK(std::holds_alternative<Welcome>(*last));
}

// Défaut #6 : des octets en trop DANS l'enveloppe déclarée d'un message
// connu (ici Resize, qui ne consomme que 4 octets) ne doivent plus être
// avalés en silence par l'ancien `buf_.erase` global : c'est désormais un
// échec de connexion, au même titre qu'un corps incohérent.
TEST(proto_decoder_rejects_trailing_bytes_inside_known_message) {
  std::string body;
  body += static_cast<char>(0);
  body += static_cast<char>(80);    // cols = 80
  body += static_cast<char>(0);
  body += static_cast<char>(24);    // rows = 24
  body += static_cast<char>(0xAA);  // octet de trop, jamais lu par Resize

  Decoder d;
  d.feed(raw_msg(kTagResize, body));
  CHECK(!d.next().has_value());
  CHECK(d.failed());
}

// Défaut #2 (O(N²)) : des milliers de petits messages concaténés dans un
// seul tampon doivent tous ressortir, en nombre exact — l'ancien
// `buf_.erase(0, ...)` par message était correct mais ruineux en CPU ; ce
// test vérifie la correction (le nombre drainé), pas le chronométrage
// (fragile en CI). Le mélange de tailles de message exerce aussi
// Decoder::compact() plusieurs fois pendant le drain (le seuil « moitié
// gaspillée » est franchi plusieurs fois avant l'épuisement du tampon).
TEST(proto_decoder_drains_many_thousands_of_small_messages_exactly) {
  constexpr int kCount = 6000;
  std::string wire;
  for (int i = 0; i < kCount; ++i) {
    if (i % 2 == 0) {
      wire += encode(Msg{Welcome{}});
    } else {
      wire += encode(Msg{Resize{static_cast<uint16_t>(i), static_cast<uint16_t>(i)}});
    }
  }
  Decoder d;
  d.feed(wire);
  int produced = 0;
  while (d.next().has_value()) ++produced;
  CHECK_EQ(produced, kCount);
  CHECK(!d.failed());
}

// Un flux de pur bruit ne doit jamais faire sortir un message. Les octets
// sont générés déterministement dans [1, 250] : jamais 0x00, donc le champ
// longueur (4 octets, gros-boutien) d'un en-tête pris n'importe où dans ce
// bruit vaut toujours au moins 0x01000000 (16 777 216) — bien plus que les
// quelques centaines d'octets réellement disponibles, et pour la plupart
// des positions au-delà même de kMaxMessageBytes. Dans tous les cas, le
// premier en-tête rencontré fait soit échouer le décodeur (longueur hors
// plafond) soit lui fait réclamer plus d'octets qui ne viendront jamais :
// aucun message ne peut se former par coïncidence.
TEST(proto_decoder_never_produces_a_message_from_pure_garbage) {
  std::string garbage;
  for (int i = 0; i < 997; ++i) {
    garbage += static_cast<char>(1 + (i * 167 + 13) % 250);
  }
  Decoder d;
  d.feed(garbage);
  int produced = 0;
  while (d.next().has_value()) ++produced;
  CHECK_EQ(produced, 0);
}

// Une coupure en plein milieu du champ longueur (4 octets) ne doit produire
// aucun message tant que les 4 octets, puis le corps, ne sont pas tous
// arrivés.
TEST(proto_decoder_survives_split_inside_length_field) {
  const std::string wire = encode(Msg{Resize{80, 24}});
  Decoder d;
  d.feed(wire.substr(0, 1));  // le tag seul
  CHECK(!d.next().has_value());
  d.feed(wire.substr(1, 2));  // 2 des 4 octets de longueur
  CHECK(!d.next().has_value());
  d.feed(wire.substr(3, 1));  // 3e octet de longueur
  CHECK(!d.next().has_value());
  d.feed(wire.substr(4, 1));  // 4e et dernier octet de longueur : l'en-tête est complet
  CHECK(!d.next().has_value());  // mais le corps n'est pas encore là
  d.feed(wire.substr(5));
  const auto out = d.next();
  CHECK(out.has_value());
  if (!out.has_value()) return;
  CHECK(std::holds_alternative<Resize>(*out));
  CHECK(!d.failed());
}

// Le plafond global du tampon (kMaxBufferBytes) doit couper la connexion
// plutôt que de laisser feed() accumuler indéfiniment — sans jamais avoir
// vu un en-tête cohérent. Un seul feed() surdimensionné suffit : comme
// feed() refuse d'agrandir le tampon au-delà du plafond, l'octet en trop
// n'est jamais copié dans le décodeur.
TEST(proto_decoder_rejects_buffer_over_capacity) {
  Decoder d;
  const std::string huge(kMaxBufferBytes + 1, 'x');
  d.feed(huge);
  CHECK(d.failed());
  CHECK(!d.next().has_value());
}

// Un message légal de taille maximale (kMaxMessageBytes) doit pouvoir
// s'assembler même arrivé fragmenté sur de très nombreux feed() : c'est
// exactement ce que kMaxBufferBytes est dimensionné pour permettre.
TEST(proto_decoder_assembles_max_size_message_across_many_feeds) {
  const uint32_t inner_len = static_cast<uint32_t>(kMaxMessageBytes) - 4;  // Input : préfixe (4) + contenu
  Decoder d;
  d.feed(raw_header(kTagInput, static_cast<uint32_t>(kMaxMessageBytes)));
  d.feed(raw_u32(inner_len));

  const std::string filler(inner_len, 'z');
  constexpr size_t kChunk = 4ull * 1024 * 1024;
  size_t off = 0;
  while (off < filler.size()) {
    const size_t n = std::min(kChunk, filler.size() - off);
    d.feed(std::string_view(filler).substr(off, n));
    off += n;
    // Le dernier morceau complète le message : ne pas vérifier
    // l'incomplétude sur cette itération-là, sous peine de consommer (et
    // perdre) le message qu'on s'apprête à aller chercher juste après.
    if (off < filler.size()) CHECK(!d.next().has_value());
  }

  const auto out = d.next();
  CHECK(out.has_value());
  if (!out.has_value()) return;
  const auto* in = std::get_if<Input>(&*out);
  CHECK(in != nullptr);
  if (in == nullptr) return;
  CHECK_EQ(in->bytes.size(), static_cast<size_t>(inner_len));
  CHECK(!d.failed());
}

// Durcissement : le tampon physique peut grossir jusqu'à ~2x
// kMaxBufferBytes (voir son commentaire, proto.hpp) puis, sans ce
// correctif, rester figé à cette taille pour le reste de la vie du
// Decoder — compact() ne fait qu'erase(), qui ne rend jamais la
// capacité. Ce test construit le scénario que le plafond documente
// lui-même : un message légal de taille maximale (kMaxMessageBytes)
// suivi d'un second message qui occupe pile la marge de 1 Mio que
// kMaxBufferBytes réserve au-delà d'un message maximal, de sorte que le
// tampon logique atteigne EXACTEMENT kMaxBufferBytes avant le moindre
// drain. Cela garantit buf_.capacity() >= kMaxBufferBytes par la seule
// garantie de la norme (capacity() >= size()), sans dépendre d'une
// marge de croissance propre à l'implémentation. On vide ensuite
// entièrement le tampon (les deux messages) et on vérifie que la
// capacité est bien redescendue — pas seulement que size() a diminué.
// buffer_capacity_for_tests() est l'accesseur de diagnostic ajouté pour
// ce test (voir son commentaire dans proto.hpp).
TEST(proto_decoder_releases_capacity_once_fully_drained_past_threshold) {
  // Message 1 : un Input légal de taille maximale, comme dans
  // proto_decoder_assembles_max_size_message_across_many_feeds.
  const uint32_t content1 = static_cast<uint32_t>(kMaxMessageBytes) - 4;
  std::string msg1 = raw_header(kTagInput, static_cast<uint32_t>(kMaxMessageBytes));
  msg1 += raw_u32(content1);
  msg1 += std::string(content1, 'z');

  // Message 2 : un second Input dont la taille totale sur le fil occupe
  // exactement la marge de 1 Mio réservée par kMaxBufferBytes au-delà
  // d'un message maximal.
  constexpr size_t kMargin = 1ull * 1024 * 1024;
  const uint32_t content2 = static_cast<uint32_t>(kMargin) - 9;  // 5 (en-tête) + 4 (préfixe interne)
  std::string msg2 = raw_header(kTagInput, content2 + 4);
  msg2 += raw_u32(content2);
  msg2 += std::string(content2, 'y');

  CHECK_EQ(msg1.size() + msg2.size(), kMaxBufferBytes);

  Decoder d;
  const std::string wire = msg1 + msg2;
  constexpr size_t kChunk = 4ull * 1024 * 1024;
  size_t off = 0;
  while (off < wire.size()) {
    const size_t n = std::min(kChunk, wire.size() - off);
    d.feed(std::string_view(wire).substr(off, n));
    off += n;
  }
  CHECK(!d.failed());

  const size_t grown_capacity = d.buffer_capacity_for_tests();
  CHECK(grown_capacity >= kMaxBufferBytes);

  int produced = 0;
  while (d.next().has_value()) ++produced;
  CHECK_EQ(produced, 2);
  CHECK(!d.failed());

  const size_t drained_capacity = d.buffer_capacity_for_tests();
  // La preuve recherchée : la capacité est redescendue à quelque chose
  // de proche de zéro (la taille d'un std::string frais), pas
  // simplement en dessous de grown_capacity — un simple erase() aurait
  // laissé drained_capacity == grown_capacity (seul size() aurait
  // changé), ce qui est exactement le comportement de l'ancien code.
  CHECK(drained_capacity < grown_capacity);
  CHECK(drained_capacity < 1024u);
}

// Vérifie aussi le cas Hello complet contre un tag inconnu bâti à la main
// (et non pas seulement contre un Welcome) : la reprise après un tag
// inconnu doit fonctionner quel que soit le message qui suit.
TEST(proto_decoder_unknown_tag_then_hello_is_delivered) {
  Hello h;
  h.term = "xterm";
  h.cols = 80;
  h.rows = 24;

  Decoder d;
  d.feed(raw_msg(kTagUnknown, "n'importe quoi"));
  d.feed(encode(Msg{h}));

  const auto out = d.next();
  CHECK(out.has_value());
  if (!out.has_value()) return;
  const auto* got = std::get_if<Hello>(&*out);
  CHECK(got != nullptr);
  if (got == nullptr) return;
  CHECK_EQ(got->term, std::string("xterm"));
  CHECK(!d.failed());
}

// LA RAISON DE MISE A JOUR EST UNE CONSTANTE, PAS UNE PHRASE. Les quatre
// autres raisons sont des litteraux francais que le client se contente
// d'imprimer ; faire dependre un COMPORTEMENT d'une comparaison de texte
// libre casserait a la premiere reformulation, en silence. Ce cas fige la
// valeur et verifie qu'elle traverse l'encodage intacte.
TEST(proto_carries_a_stable_update_detach_reason) {
  const std::string wire = sshos::encode(sshos::Msg{
      sshos::Detached{sshos::kDetachReasonUpdate}});

  sshos::Decoder d;
  d.feed(wire);
  const auto got = d.next();
  REQUIRE(got.has_value());
  const auto* det = std::get_if<sshos::Detached>(&*got);
  REQUIRE(det != nullptr);
  CHECK_EQ(det->reason, std::string(sshos::kDetachReasonUpdate));

  // Et elle ne se confond avec aucune des raisons ordinaires du demon.
  CHECK(std::string(sshos::kDetachReasonUpdate) != "le demon s'arrete");
  CHECK(std::string(sshos::kDetachReasonUpdate) != "detache, la session continue");
  CHECK(std::string(sshos::kDetachReasonUpdate) != "un autre client a pris la main");
}
