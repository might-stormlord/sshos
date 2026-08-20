#include "shell/modal.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "render/gauge.hpp"
#include "render/width.hpp"

namespace sshos {
namespace {

// Les libelles NUS : button() pose les crochets, une seule fois.
constexpr char kCancel[] = "Annuler";
constexpr char kConfirm[] = "Confirmer";
constexpr char kAcknowledge[] = "[ OK ]";
constexpr int kCancelW = sizeof(kCancel) + 3;   // « [ Annuler ] »
constexpr int kConfirmW = sizeof(kConfirm) + 3;

// Un libelle est encadre de crochets et d'espaces : « Redemarrer » devient
// « [ Redemarrer ] ». Le faire ICI plutot qu'a l'appel evite qu'un appelant
// oublie les crochets et casse l'alignement des deux boutons.
std::string button(const std::string& label) { return "[ " + label + " ]"; }

// Cadre, LES LIGNES DE TEXTE, ligne vide, boutons, ligne vide, cadre. Le
// corps peut tenir sur plusieurs lignes : « il existe 7 mises a jour » et
// « cce9d11 -> 3512ffe » ne se lisent pas serrees sur une seule.
constexpr int kChromeHeight = 5;
// DEUX MARGES DE DEUX COLONNES, ET UN ESPACE ENTRE LES BOUTONS : c'est ce
// que la pose de cancel_rect()/confirm_rect() consomme, et le plancher n'est
// que cette somme, calculee sur les libelles qu'on a REELLEMENT recus.
constexpr int kButtonsChrome = 5;
constexpr int kMinWidth = kCancelW + kConfirmW + kButtonsChrome;

// Decoupe le corps sur les retours a la ligne. Rendre un vecteur plutot que
// d'indexer a la volee : rect(), draw() et les boutons ont tous besoin du
// MEME compte de lignes, et le recalculer trois fois invite a la divergence.
std::vector<std::string> body_lines(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t nl = s.find('\n', start);
    if (nl == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  if (out.empty()) out.push_back(std::string());
  return out;
}

}  // namespace

void Modal::ask(std::string question, WindowId target) {
  ask(std::move(question), target, kCancel, kConfirm);
}

void Modal::ask(std::string question, WindowId target, std::string cancel_label,
                std::string confirm_label) {
  // La seconde demande est ignorée, pas mise en file : c'est la première
  // question que l'utilisateur a sous les yeux.
  //
  // Sauf si une PROGRESSION occupe la place : elle n'attend aucune reponse,
  // et c'est precisement elle qui doit se transformer en question quand le
  // travail se termine.
  if (open_ && style_ != ModalStyle::Progress) return;
  open_ = true;
  style_ = ModalStyle::Question;
  cancel_label_ = std::move(cancel_label);
  confirm_label_ = std::move(confirm_label);
  question_ = std::move(question);
  target_ = target;
  confirm_ = false;
  percent_ = -1;
}

void Modal::progress(std::string message) {
  if (open_ && style_ != ModalStyle::Progress) return;
  open_ = true;
  style_ = ModalStyle::Progress;
  question_ = std::move(message);
  target_ = 0;
  confirm_ = false;
  percent_ = -1;
}

void Modal::set_body(std::string message) {
  if (!open_) return;
  question_ = std::move(message);
}

void Modal::inform(std::string message) {
  // Même règle que ask() : la seconde demande est ignorée -- sauf face a une
  // progression, qui a vocation a ceder la place a son resultat.
  if (open_ && style_ != ModalStyle::Progress) return;
  open_ = true;
  style_ = ModalStyle::Info;
  question_ = std::move(message);
  target_ = 0;
  confirm_ = true;  // le seul bouton a le focus
  percent_ = -1;
}

void Modal::dismiss() {
  open_ = false;
  style_ = ModalStyle::Question;
  cancel_label_.clear();
  confirm_label_.clear();
  question_.clear();
  target_ = 0;
  confirm_ = false;
  percent_ = -1;
}

// LE PLANCHER SUIT LES LIBELLES QU'ON A RECUS, PAS CEUX PAR DEFAUT.
//
// Il etait fige sur « Annuler / Confirmer » alors que cancel_rect() et
// confirm_rect() se posent, elles, a partir des libelles REELS. La session
// pose « Plus tard / Mettre a jour » et « Plus tard / Reinstaller depuis
// GitHub » : sur un corps court, le bouton de gauche sortait du cadre par la
// gauche -- peint sur le bureau, et INCLIQUABLE puisque hit() exige d'abord
// que le point soit dans rect_. C'est le defaut de 3512ffe revenu par la
// porte des libelles au lieu du corps.
int Modal::min_width() const {
  if (style_ != ModalStyle::Question) return kMinWidth;
  const int want = static_cast<int>(button(cancel_label_).size() +
                                    button(confirm_label_).size()) +
                   kButtonsChrome;
  return std::max(kMinWidth, want);
}

Rect Modal::rect(int cols, int rows) const {
  const std::vector<std::string> lines = body_lines(question_);
  std::size_t longest = 0;
  for (const std::string& l : lines) longest = std::max(longest, l.size());

  const int floor = min_width();
  const int want = static_cast<int>(longest) + 4;
  int w = std::max(floor, want);
  w = std::min(w, std::max(floor, cols - 4));
  w = std::min(w, cols);
  const int h = std::min(kChromeHeight + static_cast<int>(lines.size()), rows);
  return Rect{(cols - w) / 2, (rows - h) / 2, w, h};
}

// Les boutons descendent avec le corps : une ligne de plus les pousse d'une
// ligne. Sans ce calcul commun, hit() cliquerait ailleurs que ce que draw()
// a peint -- la discipline du panneau, appliquee ici.
int Modal::buttons_y() const {
  return rect_.y + static_cast<int>(body_lines(question_).size()) + 2;
}

void Modal::layout(int cols, int rows) { rect_ = rect(cols, rows); }

// LES DEUX BOUTONS SONT BORNES PAR LE CADRE, TOUJOURS. min_width() suffit
// des que l'ecran peut le porter -- et alors ces bornes ne rognent rien --
// mais un terminal plus etroit que la somme des deux libelles ferait
// autrement ressortir le bouton de gauche. Un bouton peint hors du cadre est
// perdu deux fois : il salit le bureau, et hit() ne le rend jamais.
Rect Modal::confirm_rect() const {
  const int w = std::min(static_cast<int>(button(confirm_label_).size()),
                         std::max(0, rect_.w - 4));
  return Rect{rect_.x + rect_.w - 2 - w, buttons_y(), w, 1};
}

Rect Modal::cancel_rect() const {
  const Rect k = confirm_rect();
  // Ce qui reste entre la marge gauche du cadre et le bouton de droite.
  const int room = std::max(0, k.x - 1 - (rect_.x + 2));
  const int w = std::min(static_cast<int>(button(cancel_label_).size()), room);
  return Rect{k.x - 1 - w, buttons_y(), w, 1};
}

void Modal::draw(View v, const Theme& th, Border b) const {
  if (!open_) return;

  Style st;
  st.bg = th.modal_bg;
  st.fg = th.modal_fg;
  v.fill(rect_, st);
  v.box(rect_, b, st);

  Style hot = st;
  hot.bg = th.accent;
  hot.fg = th.modal_bg;

  // ÉLIDÉ À LA LARGEUR DE LA BOÎTE. rect() borne bien le cadre à l'écran,
  // mais draw() reçoit une View pleine largeur : sans cette coupe, une ligne
  // plus longue que la boîte débordait par-dessus le bureau jusqu'au bord
  // droit. Le tilde plutôt qu'une ellipse Unicode : Modal ne sait pas si le
  // client accepte l'UTF-8.
  const std::vector<std::string> lines = body_lines(question_);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    v.text(rect_.x + 2, rect_.y + 1 + static_cast<int>(i),
           elide_to_cells(lines[i], rect_.w - 4, "~"), st);
  }
  if (style_ == ModalStyle::Progress) {
    // LA BARRE PREND LA PLACE DES BOUTONS. Une progression n'en a aucun, et
    // se servir de leur ligne evite a la boite de changer de hauteur entre
    // « je ne sais pas ou j'en suis » et « j'en suis a 47% » -- un cadre qui
    // grandit d'une ligne en cours de travail clignote.
    if (percent_ >= 0) {
      const std::string pc = std::to_string(percent_) + "%";
      // Deux colonnes de respiration entre la barre et le chiffre, et le
      // chiffre calibre sur « 100% » pour que la barre ne se decale pas en
      // passant de 9 a 10 pour cent.
      const int chiffre = 4;
      const int barre = rect_.w - 4 - chiffre - 2;
      if (barre >= 4) {
        Style g = st;
        g.fg = th.accent;
        v.text(rect_.x + 2, buttons_y(), gauge_bar(percent_, barre, b), g);
        v.text(rect_.x + 2 + barre + 2 + (chiffre - static_cast<int>(pc.size())),
               buttons_y(), pc, st);
      }
    }
    return;  // aucun bouton : ca travaille
  }
  if (style_ == ModalStyle::Info) {
    // Un seul bouton, centré : il n'y a rien à décider.
    const int w = static_cast<int>(sizeof(kAcknowledge) - 1);
    v.text(rect_.x + (rect_.w - w) / 2, confirm_rect().y, kAcknowledge, hot);
    return;
  }
  const Rect c = cancel_rect();
  const Rect k = confirm_rect();
  // ELIDES A LA LARGEUR QUE LA GEOMETRIE LEUR DONNE : draw() et hit() lisent
  // le meme rectangle, faute de quoi on cliquerait a cote de ce qui est
  // peint. Sur un ecran capable de porter min_width() -- le cas normal -- la
  // coupe ne retire rien.
  v.text(c.x, c.y, elide_to_cells(button(cancel_label_), c.w, "~"),
         confirm_ ? st : hot);
  v.text(k.x, k.y, elide_to_cells(button(confirm_label_), k.w, "~"),
         confirm_ ? hot : st);
}

ModalHit Modal::hit(int x, int y) const {
  if (!open_ || !rect_.contains(x, y)) return ModalHit::None;
  // En mode information il n'y a qu'un bouton, et cliquer n'importe ou sur
  // sa ligne l'atteint : c'est une reconnaissance, pas un choix.
  if (style_ == ModalStyle::Progress) return ModalHit::Body;  // rien a cliquer
  if (style_ == ModalStyle::Info) {
    if (confirm_rect().y == y) return ModalHit::Confirm;
    return ModalHit::Body;
  }
  if (cancel_rect().contains(x, y)) return ModalHit::Cancel;
  if (confirm_rect().contains(x, y)) return ModalHit::Confirm;
  return ModalHit::Body;
}

}  // namespace sshos
