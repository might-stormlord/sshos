#include "vt/parser.hpp"

namespace sshos {
namespace {

// xterm en accepte trente ; au-delà c'est une séquence forgée, et un
// vecteur qui grandit sans borne sur l'entrée d'un invité est une fuite de
// mémoire à la demande.
constexpr size_t kMaxParams = 32;

// De quoi tenir 65535, ce que réclament les coordonnées les plus larges.
// Sans plafond, `\033[999999999999m` déborde un int -- comportement
// indéfini, donc UBSan en Debug et n'importe quoi en Release.
constexpr int kMaxParamValue = 65535;

// `\033(0` n'en a qu'un ; on en garde quatre pour les séquences exotiques,
// et on ignore au-delà plutôt que de grandir.
constexpr size_t kMaxIntermediates = 4;

// Un OSC plus long qu'un titre de fenêtre plausible est soit une attaque,
// soit un programme cassé. On tronque et on continue : jeter la séquence
// entière ferait perdre le titre à cause d'un octet de trop.
constexpr size_t kMaxOsc = 4096;

bool is_c0(uint8_t b) { return b <= 0x17 || b == 0x19 || (b >= 0x1C && b <= 0x1F); }
bool is_intermediate(uint8_t b) { return b >= 0x20 && b <= 0x2F; }
bool is_final(uint8_t b) { return b >= 0x40 && b <= 0x7E; }
bool is_digit(uint8_t b) { return b >= 0x30 && b <= 0x39; }
bool is_private(uint8_t b) { return b >= 0x3C && b <= 0x3F; }

}  // namespace

int param_or(const Params& p, size_t index, int fallback) {
  if (index >= p.size()) return fallback;
  return p[index].value < 0 ? fallback : p[index].value;
}

void Parser::reset() {
  state_ = VtState::Ground;
  clear_sequence();
  utf8_need_ = 0;
  utf8_seen_ = 0;
  utf8_acc_ = 0;
  utf8_min_ = 0;
}

void Parser::clear_sequence() {
  params_.clear();
  intermediates_.clear();
  osc_.clear();
}

bool Parser::collect(uint8_t b) {
  // Rend false quand la liste déborde, et l'appelant jette la séquence
  // entière. Tronquer silencieusement serait pire que jeter :
  // « \033[!!!!!!p » deviendrait « \033[!!!!p », qui est une séquence
  // valide et différente.
  if (intermediates_.size() >= kMaxIntermediates) return false;
  intermediates_.push_back(static_cast<char>(b));
  return true;
}

void Parser::next_param(bool sub) {
  if (params_.size() < kMaxParams) params_.push_back(Param{-1, sub});
}

void Parser::push_param_digit(uint8_t b) {
  if (params_.empty()) {
    if (params_.size() >= kMaxParams) return;
    params_.push_back(Param{});
  }
  Param& p = params_.back();
  const int digit = b - '0';
  if (p.value < 0) p.value = 0;
  // Le plafond s'applique EN COURS d'accumulation, pas après : sans ça le
  // débordement a déjà eu lieu au moment où on voudrait le corriger.
  if (p.value <= (kMaxParamValue - digit) / 10) {
    p.value = p.value * 10 + digit;
  } else {
    p.value = kMaxParamValue;
  }
}

void Parser::flush_utf8_as_replacement() {
  if (utf8_need_ == 0) return;
  utf8_need_ = 0;
  utf8_seen_ = 0;
  utf8_acc_ = 0;
  utf8_min_ = 0;
  sink_->print(0xFFFD);
}

void Parser::feed(std::string_view bytes) {
  for (const char c : bytes) step(static_cast<uint8_t>(c));
}

void Parser::utf8_byte(uint8_t b) {
  if (utf8_need_ > 0) {
    if ((b & 0xC0) != 0x80) {
      // Une séquence tronquée suivie d'autre chose : on rend le caractère
      // de remplacement pour ce qui manque, PUIS on retraite l'octet à
      // neuf. Le jeter avec la séquence ferait disparaître un caractère
      // parfaitement valide à chaque octet corrompu.
      flush_utf8_as_replacement();
      ground_byte(b);
      return;
    }
    utf8_acc_ = (utf8_acc_ << 6) | (b & 0x3F);
    if (++utf8_seen_ < utf8_need_) return;

    const uint32_t cp = utf8_acc_;
    const uint32_t min = utf8_min_;
    utf8_need_ = 0;
    utf8_seen_ = 0;
    utf8_acc_ = 0;
    utf8_min_ = 0;
    // Surlong, substitut UTF-16, ou au-delà du plan 16 : trois façons
    // d'encoder ce qui n'existe pas, et trois vecteurs d'évasion connus si
    // on les laisse passer.
    if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      sink_->print(0xFFFD);
      return;
    }
    sink_->print(static_cast<char32_t>(cp));
    return;
  }

  if (b >= 0xC2 && b <= 0xDF) {
    utf8_acc_ = b & 0x1F;
    utf8_need_ = 1;
    utf8_min_ = 0x80;
  } else if (b >= 0xE0 && b <= 0xEF) {
    utf8_acc_ = b & 0x0F;
    utf8_need_ = 2;
    utf8_min_ = 0x800;
  } else if (b >= 0xF0 && b <= 0xF4) {
    utf8_acc_ = b & 0x07;
    utf8_need_ = 3;
    utf8_min_ = 0x10000;
  } else {
    // 0x80-0xBF hors séquence, 0xC0/0xC1 (surlongs par construction),
    // 0xF5-0xFF (au-delà du plan 16).
    sink_->print(0xFFFD);
    return;
  }
  utf8_seen_ = 0;
}

void Parser::ground_byte(uint8_t b) {
  // Une séquence UTF-8 en cours a la priorité sur TOUT le reste, y compris
  // sur l'ASCII : sans ce renvoi, « \xc3A » -- un « é » tronqué suivi d'un
  // A -- imprimait le A et faisait disparaître le caractère tronqué en
  // silence, au lieu de rendre un caractère de remplacement. utf8_byte()
  // rappelle ground_byte() une fois le ménage fait, et une seule fois.
  if (utf8_need_ > 0) {
    utf8_byte(b);
    return;
  }
  if (b == 0x1B) {
    state_ = VtState::Escape;
    clear_sequence();
    return;
  }
  if (b == 0x18 || b == 0x1A) {  // CAN, SUB
    sink_->execute(b);
    return;
  }
  if (is_c0(b)) {
    sink_->execute(b);
    return;
  }
  if (b == 0x7F) return;  // DEL : ignoré, comme dans xterm
  if (b < 0x80) {
    sink_->print(static_cast<char32_t>(b));
    return;
  }
  utf8_byte(b);
}

void Parser::step(uint8_t b) {
  // CAN et SUB annulent n'importe quelle séquence en cours, où qu'on en
  // soit. C'est la seule porte de sortie d'un état bloqué, et un `tmux`
  // imbriqué s'en sert pour se resynchroniser.
  if ((b == 0x18 || b == 0x1A) && state_ != VtState::Ground) {
    if (state_ == VtState::DcsPassthrough) sink_->dcs_end();
    state_ = VtState::Ground;
    clear_sequence();
    sink_->execute(b);
    return;
  }
  // ESC repart d'une séquence neuve depuis n'importe où : un ESC au milieu
  // d'un CSI n'est jamais un paramètre.
  if (b == 0x1B && state_ != VtState::Ground) {
    // Un OSC terminé par ESC \ (ST) sort ICI : sa chaîne est complète, seul
    // son terminateur diffère du BEL de xterm. Un DCS, lui, se referme.
    if (state_ == VtState::OscString) sink_->osc(osc_);
    if (state_ == VtState::DcsPassthrough) sink_->dcs_end();
    state_ = VtState::Escape;
    clear_sequence();
    return;
  }

  switch (state_) {
    case VtState::Ground:
      // ESC est traité ici et non au-dessus, pour que le décodeur UTF-8
      // rende son caractère de remplacement avant de changer d'état.
      if (b == 0x1B) flush_utf8_as_replacement();
      ground_byte(b);
      return;

    case VtState::Escape:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      if (is_intermediate(b)) {
        collect(b);
        state_ = VtState::EscapeIntermediate;
        return;
      }
      if (b == 0x7F) return;
      switch (b) {
        case '[':
          state_ = VtState::CsiEntry;
          clear_sequence();
          return;
        case ']':
          state_ = VtState::OscString;
          osc_.clear();
          return;
        case 'P':
          state_ = VtState::DcsEntry;
          clear_sequence();
          return;
        case 'X':
        case '^':
        case '_':
          state_ = VtState::SosPmApcString;
          return;
        default:
          break;
      }
      sink_->esc(intermediates_, b);
      state_ = VtState::Ground;
      return;

    case VtState::EscapeIntermediate:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      if (is_intermediate(b)) {
        // Pas d'état « EscapeIgnore » dans la machine DEC : on retombe au
        // sol sans rien émettre, ce qui revient au même.
        if (!collect(b)) state_ = VtState::Ground;
        return;
      }
      if (b == 0x7F) return;
      sink_->esc(intermediates_, b);
      state_ = VtState::Ground;
      return;

    case VtState::CsiEntry:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      if (b == 0x7F) return;
      if (is_final(b)) {
        sink_->csi(params_, intermediates_, b);
        state_ = VtState::Ground;
        return;
      }
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::CsiIgnore;
        else state_ = VtState::CsiIntermediate;
        return;
      }
      // Note : en CsiEntry la liste d'intermédiaires est forcément vide --
      // on y arrive après clear_sequence() -- si bien que le débordement
      // testé juste au-dessus ne peut pas s'y produire. La garde est là
      // pour la symétrie avec CsiParam, où elle est bien atteignable.
      //
      // Le marqueur privé (`?`, `>`, `<`, `=`) n'est légal qu'ICI, en tête.
      // Il voyage avec les intermédiaires : le puits reçoit `?` et `h`, et
      // sait donc lire `\033[?25h` sans qu'on invente un troisième champ.
      if (is_private(b)) {
        collect(b);
        state_ = VtState::CsiParam;
        return;
      }
      if (is_digit(b)) {
        push_param_digit(b);
        state_ = VtState::CsiParam;
        return;
      }
      if (b == ';' || b == ':') {
        if (params_.empty()) params_.push_back(Param{});
        next_param(b == ':');
        state_ = VtState::CsiParam;
        return;
      }
      state_ = VtState::CsiIgnore;
      return;

    case VtState::CsiParam:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      if (b == 0x7F) return;
      if (is_digit(b)) {
        push_param_digit(b);
        return;
      }
      if (b == ';' || b == ':') {
        if (params_.empty()) params_.push_back(Param{});
        next_param(b == ':');
        return;
      }
      if (is_final(b)) {
        sink_->csi(params_, intermediates_, b);
        state_ = VtState::Ground;
        return;
      }
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::CsiIgnore;
        else state_ = VtState::CsiIntermediate;
        return;
      }
      // Un marqueur privé APRÈS des paramètres est illégal : la séquence
      // entière part à la poubelle plutôt que d'être devinée.
      state_ = VtState::CsiIgnore;
      return;

    case VtState::CsiIntermediate:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      if (b == 0x7F) return;
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::CsiIgnore;
        return;
      }
      if (is_final(b)) {
        sink_->csi(params_, intermediates_, b);
        state_ = VtState::Ground;
        return;
      }
      state_ = VtState::CsiIgnore;
      return;

    case VtState::CsiIgnore:
      if (is_c0(b)) {
        sink_->execute(b);
        return;
      }
      // On avale jusqu'au final, qu'on jette avec le reste.
      if (is_final(b)) state_ = VtState::Ground;
      return;

    case VtState::DcsEntry:
      if (b == 0x7F || is_c0(b)) return;  // pas d'execute dans un DCS
      if (is_final(b)) {
        sink_->dcs_start(params_, intermediates_, b);
        state_ = VtState::DcsPassthrough;
        return;
      }
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::DcsIgnore;
        else state_ = VtState::DcsIntermediate;
        return;
      }
      if (is_private(b)) {
        collect(b);
        state_ = VtState::DcsParam;
        return;
      }
      if (is_digit(b)) {
        push_param_digit(b);
        state_ = VtState::DcsParam;
        return;
      }
      if (b == ';' || b == ':') {
        if (params_.empty()) params_.push_back(Param{});
        next_param(b == ':');
        state_ = VtState::DcsParam;
        return;
      }
      state_ = VtState::DcsIgnore;
      return;

    case VtState::DcsParam:
      if (b == 0x7F || is_c0(b)) return;
      if (is_digit(b)) {
        push_param_digit(b);
        return;
      }
      if (b == ';' || b == ':') {
        if (params_.empty()) params_.push_back(Param{});
        next_param(b == ':');
        return;
      }
      if (is_final(b)) {
        sink_->dcs_start(params_, intermediates_, b);
        state_ = VtState::DcsPassthrough;
        return;
      }
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::DcsIgnore;
        else state_ = VtState::DcsIntermediate;
        return;
      }
      state_ = VtState::DcsIgnore;
      return;

    case VtState::DcsIntermediate:
      if (b == 0x7F || is_c0(b)) return;
      if (is_intermediate(b)) {
        if (!collect(b)) state_ = VtState::DcsIgnore;
        return;
      }
      if (is_final(b)) {
        sink_->dcs_start(params_, intermediates_, b);
        state_ = VtState::DcsPassthrough;
        return;
      }
      state_ = VtState::DcsIgnore;
      return;

    case VtState::DcsPassthrough: {
      // Les données passent telles quelles, octet par octet. Le liant n'en
      // interprète aucune, mais les traverser proprement évite qu'un DCS
      // non terminé -- ce qu'un tmux imbriqué produit à chaque requête de
      // capacité -- se mette à manger l'écran caractère par caractère.
      if (b == 0x7F) return;
      const char one = static_cast<char>(b);
      sink_->dcs_data(std::string_view(&one, 1));
      return;
    }

    case VtState::DcsIgnore:
      return;

    case VtState::OscString:
      if (b == 0x07) {  // BEL : le terminateur de xterm
        sink_->osc(osc_);
        state_ = VtState::Ground;
        clear_sequence();
        return;
      }
      if (is_c0(b)) return;
      // On tronque au plafond plutôt que de jeter la séquence entière :
      // perdre le titre à cause d'un octet de trop serait pire.
      if (osc_.size() < kMaxOsc) osc_.push_back(static_cast<char>(b));
      return;

    case VtState::SosPmApcString:
      return;  // avalé jusqu'à ESC ou CAN/SUB, traités plus haut
  }
}

}  // namespace sshos
