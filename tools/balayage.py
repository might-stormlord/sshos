#!/usr/bin/env python3
"""LE BALAYAGE DES FONCTIONS SANS APPELANT DE PRODUCTION.

Le defaut signature de ce projet : du code qui existe sans que personne ne
l'appelle. Vingt fois a ce jour (docs/REPRISE.md §9 bis). Aucune suite de
tests ne le signale, parce que ce qui manque n'est pas la couverture, c'est
l'APPEL -- un test unitaire ne peut pas le voir, et une campagne de mutation
non plus : muter du code mort ne casse rien, et la mutation se declare
« equivalente ».

    python3 tools/balayage.py            # la liste des candidats
    python3 tools/balayage.py --strict   # sort non nul si un candidat n'est
                                         # pas dans EXEMPTES

⚠️ LA SORTIE N'EST JAMAIS UNE CONCLUSION. Un candidat n'est un defaut
qu'apres un `grep -rn "\\bnom\\b" src/ tests/` SANS TRONCATURE, lu a la main.
Un audit adversarial a un jour declare quatre orphelines inexistantes -- il
les avait cherchees APRES leur retrait. Le rapport etait affirmatif, source,
et faux.

POURQUOI CE FICHIER EXISTE. Ce script vivait dans un bloc de markdown du
dossier de reprise, comme y vivaient sonde.py et mutation.py avant le §8 ter.
Un outil de verification qu'on ne peut ni lancer ni eprouver derive : la
version publiee jusqu'au 15 aout 2026 etait incapable de trouver ce qu'on lui
attribuait, et personne ne pouvait s'en apercevoir.

Et un outil de verification doit lui-meme etre verifie contre un cas dont on
connait la reponse :

    git archive <commit>^ src tests | tar x -C /var/tmp/essai
    python3 tools/balayage.py --racine /var/tmp/essai

sur le commit qui a retire une orpheline connue doit la faire ressortir.

LES CINQ PIEGES, ET LES CINQ PARADES SONT DANS decl() :
  1. `return foo(x);` ressemble a une declaration -- `return` passe pour un
     type de retour -- et masque un vrai appel. Parade : la liste KW.
  2. Une definition EN LIGNE, `T nom() const { return n_; }`, n'etait pas
     capturee du tout : l'ancienne version n'enregistrait que les lignes
     finissant par `;`. C'est la forme exacte des quatre orphelines du
     15 aout. Parade : accepter aussi `{` et `}` en fin de ligne.
  3. Une signature etalee sur plusieurs lignes echappe au meme filtre.
     Parade : parenthese restee ouverte = signature valide.
  4. Un commentaire en fin de ligne masque le `;` : `void set_tab();  // HTS`
     n'etait pas capture -- c'est-a-dire, precisement, le defaut n° 8 du
     tableau du §9 bis. Parade : couper la ligne au `//` avant de tester sa
     fin.
  5. UN APPEL PRECEDE D'UN OPERATEUR passe pour une declaration, et l'appel
     est alors JETE -- donc une fonction bel et bien appelee ressort comme
     orpheline. Deux formes reelles :
         out << render_config(c);                  (config.cpp:89)
         name, sshos::daemon_exe_path(), [] {      (main.cpp:45)
     Parade : une fois les chevrons apparies retires du prefixe, il ne doit
     plus rester ni virgule, ni chevron, ni signe d'egalite.

LES CONSTRUCTEURS NE SONT PAS ANALYSES, et c'est delibere : le groupe de
capture exige une initiale minuscule, or tous les types du projet sont
capitalises. Elargir aux majuscules ferait remonter les constructions
locales, `Type x{args};` etant syntaxiquement identique a une declaration.
"""
import argparse
import io
import os
import re
import sys

KW = ("return", "case", "else", "throw", "if", "for", "while", "switch",
      "do", "delete", "new", "goto")          # parade 1
DECL = re.compile(r"^([A-Za-z_][\w:<>,&\*\s]*?)\s[\*&]*(?:\w+::)*([a-z_][a-z0-9_]*)\s*\(")

# Les candidats qu'on garde EXPRES, chacun documente sur place dans le code.
EXEMPTES = {
    # Sous Linux, un maitre dont le dernier esclave s'est ferme rend EIO et
    # non 0 : note_eof() n'est atteinte que dans des cas de bord.
    "saw_eof",
    # Chaque descripteur nait deja CLOEXEC en un seul appel systeme, ce qui
    # est le motif SUR. La fonction reste pour le jour ou il faudra poser le
    # drapeau apres coup.
    "set_cloexec",
    # Un getter que la production court-circuite : width.cpp lit directement
    # le global g_ambiguous_wide.
    "ambiguous_wide",
    # Le vocabulaire d'un type RAII d'usage general, contrepartie de get() et
    # reset(). La production leve a la construction plutot que de rendre un Fd
    # invalide, d'ou l'absence d'appelant ; la suffixer serait laid et se
    # retournerait contre le premier site de production qui en aura besoin.
    "valid",
}


def _prefixe_est_un_type(p):
    """Parade 5 : `out << f(x)` et `name, f(), [] {` ne sont pas des
    declarations. Les chevrons apparies d'un type generique, eux, ont le
    droit de porter des virgules -- `std::pair<int, int> f(`."""
    sans_generique = re.sub(r"<[^<>]*>", "", p)
    return not any(c in sans_generique for c in (",", "<", ">", "="))


def decl(t):
    """Rend le nom declare par cette ligne, ou None."""
    t = t.split("//")[0].rstrip()              # parade 4
    m = DECL.match(t)
    if not m or m.group(1).split()[0] in KW:   # parade 1
        return None
    if not _prefixe_est_un_type(m.group(1)):   # parade 5
        return None
    # parade 2 : definition en ligne ; parade 3 : signature etalee
    if t.rstrip().endswith((";", "{", "}")) or t.count("(") > t.count(")"):
        return m.group(2)
    return None


def lignes(rac):
    out = []
    for root, _, fs in os.walk(rac):
        for f in sorted(fs):
            if f.endswith((".cpp", ".hpp")):   # les .hpp AUSSI : beaucoup
                p = os.path.join(root, f)      # d'appels sont en ligne
                for i, l in enumerate(io.open(p, encoding="utf-8"), 1):
                    out.append((p, i, l.rstrip("\n")))
    return out


def compte(ls, name):
    pat = re.compile(r"(?<![\w])%s\s*\(" % re.escape(name))
    n = 0
    for _p, _i, l in ls:
        t = l.strip()
        if t.startswith(("//", "*")) or decl(t) == name:
            continue
        n += len(pat.findall(t.split("//")[0]))
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--racine", default=".", help="racine du depot a balayer")
    ap.add_argument("--strict", action="store_true",
                    help="sort non nul si un candidat n'est pas exempte")
    a = ap.parse_args()

    src = lignes(os.path.join(a.racine, "src"))
    tst = lignes(os.path.join(a.racine, "tests"))
    decls = {}
    for p, i, l in src:
        if not p.endswith(".hpp"):
            continue
        t = l.strip()
        if t.startswith(("//", "*", "/*")):
            continue
        n = decl(t)
        if n:
            decls.setdefault(n, "%s:%d" % (p, i))

    print("%d noms declares" % len(decls))
    candidats, non_exemptes = [], []
    for name in sorted(decls):
        if name.endswith("_for_tests"):
            continue
        if compte(src, name) == 0:             # zero appelant de PRODUCTION
            t = compte(tst, name)
            candidats.append(name)
            marque = "" if name in EXEMPTES else "  <-- a trancher a la main"
            print("  %-26s src=0 tests=%-4d %s%s" % (name, t, decls[name], marque))
            if name not in EXEMPTES:
                non_exemptes.append(name)

    # LE SECOND PASSAGE, QUE LA BOUCLE CI-DESSUS NE PEUT PAS FAIRE : elle
    # saute les `_for_tests` par construction, donc une API de test que plus
    # aucun test n'appelle lui est invisible. Deux s'y cachaient.
    morts = [n for n in sorted(decls)
             if n.endswith("_for_tests") and compte(tst, n) == 0]
    if morts:
        print("-- API de test que plus aucun test n'appelle --")
        for n in morts:
            print("  %-26s tests=0 %s" % (n, decls[n]))
        non_exemptes += morts

    print("=== %d candidats, dont %d a trancher ===" % (len(candidats) + len(morts),
                                                        len(non_exemptes)))
    if a.strict and non_exemptes:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
