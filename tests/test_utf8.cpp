#include <string>

#include "common/utf8.hpp"
#include "harness.hpp"

// Les deux moitiés du codec UTF-8 vivent dans common/utf8.*, et leurs tests
// vivent ici, ensemble. Avant ce regroupement les cas du décodeur étaient dans
// test_surface.cpp et ceux de l'encodeur dans test_profile.cpp, parce que les
// deux fonctions y avaient été écrites — deux fichiers qui nomment des modules
// dont le codec n'est plus un détail. Qui modifie common/utf8.cpp doit pouvoir
// trouver sa couverture sans la chercher.

TEST(utf8_decode_handles_truncated_input) {
  char32_t cp = 0;
  const std::string truncated = "\xe6\x97";  // moitie de 日
  const size_t used = sshos::utf8_decode(truncated, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(2));
  CHECK_EQ(cp, U'�');
}

// Valider que les surrogates sont rejetées.
// ED A0 80 encode U+D800 : rejeter et substituer U+FFFD.
TEST(utf8_decode_rejects_surrogates) {
  char32_t cp = 0;
  const std::string surrogate_d800 = "\xed\xa0\x80";
  const size_t used = sshos::utf8_decode(surrogate_d800, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(3));
  CHECK_EQ(cp, U'�');
}

// Valider que les séquences trop longues sont rejetées.
// C0 80 encode U+0000 : rejeté car U+0000 devrait être un seul octet.
TEST(utf8_decode_rejects_overlong_2byte) {
  char32_t cp = 0;
  const std::string overlong = "\xc0\x80";
  const size_t used = sshos::utf8_decode(overlong, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(2));
  CHECK_EQ(cp, U'�');
}

// Valider le rejet des surséquences 3-octet trop longues.
// E0 80 80 encode U+0000 : rejeté.
TEST(utf8_decode_rejects_overlong_3byte) {
  char32_t cp = 0;
  const std::string overlong = "\xe0\x80\x80";
  const size_t used = sshos::utf8_decode(overlong, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(3));
  CHECK_EQ(cp, U'�');
}

// Valider le rejet des surséquences 4-octet trop longues.
// F0 80 80 80 encode U+0000 : rejeté.
TEST(utf8_decode_rejects_overlong_4byte) {
  char32_t cp = 0;
  const std::string overlong = "\xf0\x80\x80\x80";
  const size_t used = sshos::utf8_decode(overlong, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(4));
  CHECK_EQ(cp, U'�');
}

// Valider que U+D7FF (avant surrogates) est accepté.
TEST(utf8_decode_accepts_boundary_d7ff) {
  char32_t cp = 0;
  const std::string valid = "\xed\x9f\xbf";  // U+D7FF
  const size_t used = sshos::utf8_decode(valid, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(3));
  CHECK_EQ(cp, U'퟿');
}

// Valider que U+E000 (après surrogates) est accepté.
TEST(utf8_decode_accepts_boundary_e000) {
  char32_t cp = 0;
  const std::string valid = "\xee\x80\x80";  // U+E000
  const size_t used = sshos::utf8_decode(valid, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(3));
  CHECK_EQ(cp, U'');
}

// Valider que U+10FFFF (maximum valide) est accepté.
TEST(utf8_decode_accepts_boundary_10ffff) {
  char32_t cp = 0;
  const std::string valid = "\xf4\x8f\xbf\xbf";  // U+10FFFF
  const size_t used = sshos::utf8_decode(valid, 0, cp);
  CHECK_EQ(used, static_cast<size_t>(4));
  CHECK_EQ(cp, U'\U0010ffff');
}

TEST(encode_utf8_roundtrip) {
  CHECK_EQ(sshos::encode_utf8(U'a'), std::string("a"));
  CHECK_EQ(sshos::encode_utf8(U'日'), std::string("\xe6\x97\xa5"));
  CHECK_EQ(sshos::encode_utf8(U'\U0001f600'), std::string("\xf0\x9f\x98\x80"));
}

// Valider que les surrogates sont substitués par U+FFFD.
TEST(encode_utf8_rejects_surrogates) {
  // Première surrogate (D800)
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0xD800)), std::string("\xef\xbf\xbd"));
  // Dernière surrogate (DFFF)
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0xDFFF)), std::string("\xef\xbf\xbd"));
}

// Valider que les valeurs > U+10FFFF sont substituées.
TEST(encode_utf8_rejects_out_of_range) {
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0x110000)), std::string("\xef\xbf\xbd"));
  CHECK_EQ(sshos::encode_utf8(static_cast<char32_t>(0x200000)), std::string("\xef\xbf\xbd"));
}

// Valider les limites valides du scalaire Unicode.
TEST(encode_utf8_accepts_scalar_boundaries) {
  // U+D7FF : avant la première surrogate
  CHECK_EQ(sshos::encode_utf8(U'퟿'), std::string("\xed\x9f\xbf"));
  // U+E000 : après la dernière surrogate
  CHECK_EQ(sshos::encode_utf8(U''), std::string("\xee\x80\x80"));
  // U+10FFFF : maximum valide
  CHECK_EQ(sshos::encode_utf8(U'\U0010FFFF'), std::string("\xf4\x8f\xbf\xbf"));
}

// --------------------------------------------------------------------------
// Le repli des accents : la seule façon d'écrire les libellés du bureau en
// français correct une seule fois, et de les rendre lisibles au client qui
// n'annonce pas l'UTF-8.
// --------------------------------------------------------------------------

TEST(fold_to_ascii_keeps_french_readable_without_utf8) {
  CHECK(sshos::fold_to_ascii("Déplacer la fenêtre") == "Deplacer la fenetre");
  CHECK(sshos::fold_to_ascii("Précédente, à côté") == "Precedente, a cote");
  CHECK(sshos::fold_to_ascii("abc") == "abc");
  CHECK(sshos::fold_to_ascii("") == "");
  // Ce que la table ne connaît pas tombe sur '?', jamais en octets bruts :
  // un client sans UTF-8 afficherait sinon du charabia.
  CHECK(sshos::fold_to_ascii("日") == "?");
  // Et le résultat est bien de l'ASCII pur.
  const std::string out = sshos::fold_to_ascii("œuvre — « ça »");
  for (const char c : out) {
    CHECK((static_cast<unsigned char>(c) & 0x80) == 0);
  }
}
