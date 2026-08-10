#include "common/proto.hpp"

#include <cstring>

namespace sshos {
namespace {

enum class Tag : uint8_t {
  Hello = 1, Welcome = 2, Incompatible = 3, Detached = 4,
  Input = 5, Resize = 6, Frame = 7,
};

void put_u8(std::string& o, uint8_t v) { o += static_cast<char>(v); }

void put_u16(std::string& o, uint16_t v) {
  put_u8(o, static_cast<uint8_t>(v >> 8));
  put_u8(o, static_cast<uint8_t>(v & 0xFF));
}

void put_u32(std::string& o, uint32_t v) {
  put_u16(o, static_cast<uint16_t>(v >> 16));
  put_u16(o, static_cast<uint16_t>(v & 0xFFFF));
}

void put_str(std::string& o, const std::string& s) {
  put_u32(o, static_cast<uint32_t>(s.size()));
  o += s;
}

struct Reader {
  std::string_view s;
  size_t i = 0;
  bool ok = true;

  uint8_t u8() {
    if (i + 1 > s.size()) { ok = false; return 0; }
    return static_cast<uint8_t>(s[i++]);
  }
  uint16_t u16() { const uint16_t hi = u8(); return static_cast<uint16_t>((hi << 8) | u8()); }
  uint32_t u32() { const uint32_t hi = u16(); return (hi << 16) | u16(); }
  std::string str() {
    const uint32_t n = u32();
    if (!ok || i + n > s.size()) { ok = false; return {}; }
    std::string out(s.substr(i, n));
    i += n;
    return out;
  }
};

// Taille de l'enveloppe fixe (1 octet de tag + 4 octets de longueur).
// Nommée pour que l'arithmétique de next() se fasse explicitement en
// size_t : additionner ce `5` littéral (int) à `len` (uint32_t) sans
// passer par size_t calculerait en 32 bits et boucle pour len proche de
// 0xFFFFFFFF — exactement le bogue corrigé dans cette révision.
inline constexpr size_t kHeaderSize = 5;

std::string body_of(const Msg& m, Tag& tag) {
  std::string b;
  if (const auto* h = std::get_if<Hello>(&m)) {
    tag = Tag::Hello;
    put_u32(b, h->build_id);
    put_u16(b, h->cols);
    put_u16(b, h->rows);
    put_str(b, h->term);
    put_str(b, h->colorterm);
    put_u8(b, h->utf8 ? 1 : 0);
    put_u32(b, static_cast<uint32_t>(h->env.size()));
    for (const auto& [k, v] : h->env) { put_str(b, k); put_str(b, v); }
  } else if (std::get_if<Welcome>(&m) != nullptr) {
    tag = Tag::Welcome;
  } else if (const auto* x = std::get_if<Incompatible>(&m)) {
    tag = Tag::Incompatible;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Detached>(&m)) {
    tag = Tag::Detached;
    put_str(b, x->reason);
  } else if (const auto* x = std::get_if<Input>(&m)) {
    tag = Tag::Input;
    put_str(b, x->bytes);
  } else if (const auto* x = std::get_if<Resize>(&m)) {
    tag = Tag::Resize;
    put_u16(b, x->cols);
    put_u16(b, x->rows);
  } else {
    tag = Tag::Frame;
    put_str(b, std::get<FrameMsg>(m).ansi);
  }
  return b;
}

}  // namespace

std::string encode(const Msg& m) {
  Tag tag = Tag::Welcome;
  const std::string body = body_of(m, tag);
  std::string out;
  put_u8(out, static_cast<uint8_t>(tag));
  put_u32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

void Decoder::fail() {
  // Le pair a violé le protocole : plus rien de ce qu'il enverra n'est
  // digne de confiance. On efface le tampon (rien à en tirer) plutôt que
  // de le laisser grossir en pure perte jusqu'à la fermeture par
  // l'appelant.
  failed_ = true;
  pos_ = 0;
  buf_.clear();
  buf_.shrink_to_fit();
}

void Decoder::compact() {
  if (pos_ == 0) return;
  // On ne décale que lorsqu'au moins la moitié du tampon est du
  // gaspillage déjà consommé. Le coût du décalage est proportionnel à ce
  // qui reste (<= pos_ dans ce cas), donc toujours borné par ce qui vient
  // d'être consommé : le coût amorti sur tout un drain est linéaire,
  // quel que soit son découpage en feed(). Un seuil fixe ne suffirait
  // pas : près du plafond du tampon (kMaxBufferBytes) il redonnerait un
  // comportement proche du quadratique de l'origine — voir le rapport de
  // relecture pour la mesure (128 000 messages, 3,4 s).
  // En dessous de quelques kilooctets, décaler le reste coûte moins que
  // l'intérêt de le faire : pas la peine de compacter des miettes.
  constexpr size_t kCompactMinSize = 4096;
  if (buf_.size() < kCompactMinSize || pos_ * 2 < buf_.size()) return;
  buf_.erase(0, pos_);
  pos_ = 0;
}

void Decoder::feed(std::string_view bytes) {
  if (failed_) return;  // pair déjà félon : ignorer, ne pas agrandir le tampon
  if (bytes.empty()) return;
  if (buf_.size() - pos_ + bytes.size() > kMaxBufferBytes) {
    fail();
    return;
  }
  buf_.append(bytes);
}

std::optional<Msg> Decoder::next() {
  if (failed_) return std::nullopt;

  for (;;) {
    if (buf_.size() - pos_ < kHeaderSize) return std::nullopt;

    Reader head{std::string_view(buf_).substr(pos_), 0, true};
    const auto tag = static_cast<Tag>(head.u8());
    const uint32_t len = head.u32();

    // Toute l'arithmétique qui suit se fait en size_t (64 bits ici) :
    // `len` est un uint32_t, jamais mélangé à un littéral `int` avant
    // d'avoir été élargi. Voir kHeaderSize et kMaxMessageBytes.
    const size_t declared = kHeaderSize + static_cast<size_t>(len);

    // Un pair qui annonce un message plus gros que ce que le pire cas
    // légitime exige (kMaxMessageBytes) ment ou attaque : on ne prend
    // même pas la peine d'accumuler les octets pour le découvrir plus
    // tard, on coupe tout de suite — sans attendre que le tampon entier
    // atteigne son propre plafond.
    if (len > kMaxMessageBytes) {
      fail();
      return std::nullopt;
    }

    if (buf_.size() - pos_ < declared) return std::nullopt;  // pas encore tout arrivé

    Reader r{std::string_view(buf_).substr(pos_ + kHeaderSize, len), 0, true};
    std::optional<Msg> out;
    bool known_tag = true;

    switch (tag) {
      case Tag::Hello: {
        Hello h;
        h.build_id = r.u32();
        h.cols = r.u16();
        h.rows = r.u16();
        h.term = r.str();
        h.colorterm = r.str();
        h.utf8 = r.u8() != 0;
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n && r.ok; ++k) {
          std::string key = r.str();
          h.env.emplace_back(std::move(key), r.str());
        }
        out = Msg{std::move(h)};
        break;
      }
      case Tag::Welcome: out = Msg{Welcome{}}; break;
      case Tag::Incompatible: out = Msg{Incompatible{r.str()}}; break;
      case Tag::Detached: out = Msg{Detached{r.str()}}; break;
      case Tag::Input: out = Msg{Input{r.str()}}; break;
      case Tag::Resize: { Resize z; z.cols = r.u16(); z.rows = r.u16(); out = Msg{z}; break; }
      case Tag::Frame: out = Msg{FrameMsg{r.str()}}; break;
      default:
        // Tag inconnu : sauté-et-continué, pas une violation. La
        // compatibilité ascendante est voulue (une version future peut
        // ajouter un message que ce décodeur ne connaît pas encore). La
        // tension symétrique — une version future ajoutant un CHAMP à un
        // message CONNU — n'est pas résolue ici : elle l'est déjà par
        // kBuildId au handshake (Hello/Incompatible), qui refuse
        // proprement une session trop récente au lieu de laisser un
        // vieux décodeur deviner ce qu'il ne comprend pas. Ce n'est donc
        // pas ce commentaire qu'il faut modifier pour « permettre » un
        // champ de plus sur un tag connu : c'est le tag connu qui doit
        // rester strict, et kBuildId qui absorbe l'évolution.
        known_tag = false;
        break;
    }

    pos_ += declared;

    if (!known_tag) {
      compact();
      continue;  // ne pas s'arrêter sur un message délibérément ignoré
    }

    // Un tag connu dont le corps ment sur ses propres longueurs internes
    // (r.ok == false), ou qui laisse des octets non consommés dans
    // l'enveloppe qu'il a lui-même déclarée (r.i != len — la « queue »
    // silencieusement avalée par l'ancien `buf_.erase`), est un pair qui
    // viole le protocole qu'il prétend parler. Ce n'est pas un trou de
    // compatibilité : on ne devine pas ce qu'il voulait dire, on ferme.
    if (!r.ok || r.i != len) {
      fail();
      return std::nullopt;
    }

    compact();
    return out;
  }
}

}  // namespace sshos
