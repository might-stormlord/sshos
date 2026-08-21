#!/usr/bin/env python3
"""La barre de progression doit AVANCER pendant une vraie mise a jour.

Un test unitaire peut prouver que le C++ lit un pourcentage et le dessine.
Il ne peut pas prouver que quelqu'un lui en donne un : c'est le defaut
signature de ce projet -- du code ne sans appelant, quinze fois -- et ici
l'appelant est un SCRIPT SH qui surveille les journaux de cmake et de la
suite de tests en tache de fond.

On fait donc tourner un vrai `--apply` sur un faux depot, et on echantillonne
le fichier d'etat pendant qu'il travaille. Ce qui est verifie :

  - la progression prend PLUSIEURS valeurs distinctes (elle bouge) ;
  - elle ne RECULE jamais ;
  - elle reste dans [0, 100] -- le C++ refuse tout le reste, et une valeur
    refusee efface la barre au lieu de la faire mentir ;
  - l'etape et le pourcentage s'accordent : « compilation » ne rend pas un
    chiffre du domaine de la suite de tests ;
  - l'etat TERMINAL ne porte ni etape ni pourcentage -- le travail est fini.

Le faux projet est rendu volontairement lent : plusieurs unites de
compilation et une suite qui egrene ses cas, sans quoi tout serait fini
avant le premier echantillon.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sonde_update as su  # noqa: E402

# Assez d'unites pour que cmake egrene des pourcentages, et une suite qui
# prend son temps : la surveillance echantillonne toutes les 0,5 s.
UNITES = 12
CAS = 40

CMAKELISTS = """cmake_minimum_required(VERSION 3.20)
project(termos CXX)
set(CMAKE_CXX_STANDARD 20)
file(GLOB LENTS ${CMAKE_SOURCE_DIR}/src/lent_*.cpp)
add_executable(termos src/main.cpp ${LENTS})
add_executable(termos_tests tests/test_faux.cpp)
"""

# Une unite volontairement couteuse a compiler : des gabarits recursifs.
LENT = """#include <cstddef>
template <std::size_t N> struct F { static constexpr std::size_t v = N * F<N - 1>::v; };
template <> struct F<0> { static constexpr std::size_t v = 1; };
template <std::size_t N> struct S { static constexpr std::size_t v = F<N % 20>::v + S<N - 1>::v; };
template <> struct S<0> { static constexpr std::size_t v = 0; };
std::size_t bruit_@@N@@() { return S<220>::v; }
"""

# La suite egrene ses cas comme la vraie : une ligne « - <nom> » par cas.
#
# ET SES CAS SE COMPTENT COMME LES VRAIS. `total_des_cas` fait
# `grep -c '^TEST('` sur tests/test_*.cpp : un faux projet dont la suite ne
# declarerait pas ses cas ainsi rendrait un total de zero, donc AUCUN
# pourcentage -- ce qui est le comportement juste, mais laisserait ce chemin
# sans sonde. D'ou un TEST( par cas, en tete de ligne, et compile pour de
# bon.
TESTS_TETE = """#include <cstdio>
#include <ctime>
static void egrener(const char* nom) {
  std::printf("- %s\\n", nom);
  std::fflush(stdout);
  timespec t{0, 60 * 1000 * 1000};
  nanosleep(&t, nullptr);
}
#define TEST(nom) static void nom() { egrener(#nom); }
"""

TESTS_PIED = """int main() {
@@APPELS@@
  std::printf("\\n@@CAS@@ cas, 0 en echec, 0 assertions echouees\\n");
  return 0;
}
"""


def source_des_tests(cas):
    corps = "".join("TEST(cas_%d)\n" % i for i in range(cas))
    appels = "".join("  cas_%d();\n" % i for i in range(cas))
    return (TESTS_TETE + corps +
            TESTS_PIED.replace("@@APPELS@@", appels).replace("@@CAS@@", str(cas)))


def ecrire_projet_lent(fix):
    """Remplace le faux projet minuscule par un faux projet LENT."""
    w = lambda p, c: open(os.path.join(fix.repo, p), "w").write(c)
    w("CMakeLists.txt", CMAKELISTS)
    w("src/main.cpp", su.MAIN_CPP.replace("@@VERSION@@", "lent"))
    for i in range(UNITES):
        w("src/lent_%d.cpp" % i, LENT.replace("@@N@@", str(i)))
    w("tests/test_faux.cpp", source_des_tests(CAS))
    w("tests/golden/reference.txt", "une reference\n")
    shutil.copy(su.UPDATE_SH, os.path.join(fix.repo, "tools", "update.sh"))


class Echantillonneur(threading.Thread):
    """Relit le fichier d'etat sans relache et retient chaque changement."""

    def __init__(self, fix):
        super().__init__(daemon=True)
        self.fix = fix
        self.vus = []          # (stage, progress) dans l'ordre
        self.stop = False

    def run(self):
        dernier = None
        while not self.stop:
            couple = (self.fix.get("stage"), self.fix.get("progress"),
                      self.fix.get("status"))
            if couple != dernier:
                self.vus.append(couple)
                dernier = couple
            time.sleep(0.05)


def main():
    base = tempfile.mkdtemp(prefix="sshos-verif-progression-", dir="/var/tmp")
    try:
        fix = su.Fixture(base)
        ecrire_projet_lent(fix)
        neuf = fix.commit("un projet lent")
        fix.write_state("available", installed=fix.head())
        # On repart d'un commit anterieur pour qu'il y ait quelque chose a
        # appliquer.
        su.git(fix.repo, "checkout", "-q", "HEAD")
        fix.write_state("available", installed="0" * 40)

        ech = Echantillonneur(fix)
        ech.start()
        t0 = time.time()
        r = fix.update("--apply", timeout=900)
        ech.stop = True
        ech.join()
        duree = time.time() - t0

        print("application terminee en %.1f s, code %d" % (duree, r.returncode))
        if r.returncode != 0:
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])

        print("\n=== ce que l'etat a montre, dans l'ordre ===")
        for stage, prog, status in ech.vus:
            print("   %-24s progress=%-5s status=%s" % (stage or "-", prog or "-", status))

        chiffres = []
        for stage, prog, _ in ech.vus:
            if prog:
                chiffres.append((stage, int(prog)))

        ok = True

        def check(libelle, cond):
            nonlocal ok
            if not cond:
                ok = False
            print("   %-56s %s" % (libelle, "OK" if cond else "ECHEC"))

        print("\n=== verdicts ===")
        check("l'application a reussi", r.returncode == 0)
        check("status=restart-pending", fix.get("status") == "restart-pending")
        check("la progression a pris au moins 3 valeurs distinctes",
              len({v for _, v in chiffres}) >= 3)
        check("elle ne recule jamais",
              all(chiffres[i][1] <= chiffres[i + 1][1] for i in range(len(chiffres) - 1)))
        check("elle reste dans [0, 100]", all(0 <= v <= 100 for _, v in chiffres))
        compil = [v for s, v in chiffres if s == "compilation"]
        tests = [v for s, v in chiffres if s == "suite de tests"]
        check("la compilation a bouge (plusieurs valeurs)", len(set(compil)) >= 2)
        check("la suite de tests a bouge (plusieurs valeurs)", len(set(tests)) >= 2)
        check("compilation et suite ne se chevauchent pas",
              not compil or not tests or max(compil) <= min(tests))
        check("l'etat terminal ne porte pas d'etape", fix.get("stage") == "")
        check("l'etat terminal ne porte pas de pourcentage", fix.get("progress") == "")

        print("\n=== %s ===" % ("PROGRESSION AU VERT" if ok else "ECHEC"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
