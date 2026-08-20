#!/usr/bin/env python3
"""SONDE BOUT-EN-BOUT DE LA CHAINE DE MISE A JOUR.

Quatorze defauts de ce projet n'ont ete trouves ni par les tests unitaires ni
par la relecture, seulement par une sonde (docs/REPRISE.md §9 bis). Les
proprietes verifiees ici sont du meme genre : aucune n'est atteignable par un
test unitaire, parce qu'elles vivent dans des courses, des codes de retour de
processus et des remplacements de fichiers en cours d'execution.

Ce qui est verifie, dans l'ordre :
  1. --check detecte une version disponible ;
  2. --apply REFUSE d'installer quand la suite est rouge -- la propriete de
     surete centrale ;
  3. --apply installe quand la suite est verte, et conserve .previous ;
  4. deux --apply concurrents ne se marchent pas dessus : sshos.previous ne
     doit JAMAIS contenir le binaire neuf, sinon --rollback restaure la
     version cassee ;
  5. --rollback remet le binaire ET reecrit installed_commit ;
  6. un historique reecrit est reconnu comme tel, pas comme une mise a jour ;
  7. un binaire peut etre remplace PENDANT qu'un processus l'execute -- une
     ecriture en place rendrait ETXTBSY a tous les coups.

ISOLATION. La sonde tourne sur un faux depot git local, un prefixe et un
repertoire d'etat temporaires. SSHOS_BOOT_ID n'isole QUE le nom du socket :
les chemins, eux, se surchargent par SSHOS_PREFIX et SSHOS_STATE_DIR. Sans
ca elle ecraserait le vrai binaire de l'utilisateur.

Aucune commande detachee : deux agents s'y sont bloques definitivement sur ce
projet.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPDATE_SH = os.path.join(ROOT, "tools", "update.sh")

# Un faux projet minuscule : il produit un sshos et un sshos_tests, et rien
# d'autre. Compiler le vrai projet a chaque cas couterait des minutes par
# verdict.
CMAKELISTS = """cmake_minimum_required(VERSION 3.20)
project(sshos CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(sshos src/main.cpp)
add_executable(sshos_tests tests/main.cpp)
"""

MAIN_CPP = """#include <cstdio>
int main(int argc, char**) { std::printf("MARQUE=%s\\n", "@@VERSION@@"); return argc > 1 ? 2 : 0; }
"""

TESTS_GREEN = """#include <cstdio>
int main() { std::printf("0 en echec\\n"); return 0; }
"""

TESTS_RED = """#include <cstdio>
int main() { std::printf("1 en echec\\n"); return 1; }
"""

ok_all = True


def say(msg):
    print(msg, flush=True)


def check(label, condition):
    global ok_all
    print("   %-58s %s" % (label, "OK" if condition else "ECHEC"), flush=True)
    if not condition:
        ok_all = False
    return condition


def run(cmd, cwd=None, env=None, timeout=600):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                          timeout=timeout)


def git(repo, *args):
    return run(["git", "-C", repo] + list(args))


class Fixture:
    """Un faux depot, un prefixe et un etat, tous jetables."""

    def __init__(self, base):
        self.base = base
        self.repo = os.path.join(base, "depot")
        self.prefix = os.path.join(base, "prefixe")
        self.state_dir = os.path.join(base, "etat")
        os.makedirs(os.path.join(self.repo, "src"))
        os.makedirs(os.path.join(self.repo, "tests"))
        os.makedirs(os.path.join(self.repo, "tools"))
        os.makedirs(os.path.join(self.repo, "tests", "golden"))
        os.makedirs(os.path.join(self.prefix, "libexec"))
        os.makedirs(self.state_dir)

        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "sonde@example.invalid")
        git(self.repo, "config", "user.name", "sonde")
        self.write_project(version="v1", tests_ok=True)
        self.commit("premier")

    def write_project(self, version, tests_ok):
        w = lambda p, c: open(os.path.join(self.repo, p), "w").write(c)
        w("CMakeLists.txt", CMAKELISTS)
        w("src/main.cpp", MAIN_CPP.replace("@@VERSION@@", version))
        w("tests/main.cpp", TESTS_GREEN if tests_ok else TESTS_RED)
        w("tests/golden/reference.txt", "une reference\n")
        shutil.copy(UPDATE_SH, os.path.join(self.repo, "tools", "update.sh"))

    def commit(self, msg):
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", msg)
        return git(self.repo, "rev-parse", "HEAD").stdout.strip()

    def head(self):
        return git(self.repo, "rev-parse", "HEAD").stdout.strip()

    def env(self):
        e = dict(os.environ)
        e["SSHOS_PREFIX"] = self.prefix
        e["SSHOS_STATE_DIR"] = self.state_dir
        e["SSHOS_REPO_URL"] = self.repo
        return e

    def write_state(self, status, installed, previous="", source="git"):
        with open(os.path.join(self.state_dir, "state"), "w") as f:
            f.write("schema=1\nprefix=%s\nsource=%s\ninstalled_commit=%s\n"
                    "previous_commit=%s\nremote_commit=\nchecked_at=0\n"
                    "status=%s\npid=\nmessage=\n"
                    % (self.prefix, source, installed, previous, status))

    def get(self, key):
        p = os.path.join(self.state_dir, "state")
        if not os.path.exists(p):
            return ""
        for line in open(p):
            if line.startswith(key + "="):
                return line[len(key) + 1:].rstrip("\n")
        return ""

    def update(self, mode, timeout=600):
        return run(["sh", UPDATE_SH, mode], env=self.env(), timeout=timeout)

    def exe(self):
        return os.path.join(self.prefix, "libexec", "sshos")

    def previous(self):
        return os.path.join(self.prefix, "libexec", "sshos.previous")


def marker(path):
    """Ce que le binaire pose imprime : MARQUE=v1 ou MARQUE=v2."""
    if not os.path.exists(path):
        return "<absent>"
    r = run([path], timeout=30)
    return r.stdout.strip()


def etape1_detection(fix):
    say("\n== 1. --check detecte une version disponible")
    first = fix.head()
    fix.write_project(version="v2", tests_ok=True)
    second = fix.commit("deuxieme")
    fix.write_state("up-to-date", installed=first)

    r = fix.update("--check")
    check("code de retour nul", r.returncode == 0)
    check("status=available", fix.get("status") == "available")
    check("remote_commit renseigne", fix.get("remote_commit") == second)
    return first, second


def etape2_refus_sur_rouge(fix, installed):
    say("\n== 2. --apply REFUSE d'installer quand la suite est rouge")
    fix.write_project(version="v3", tests_ok=False)
    fix.commit("suite cassee")
    fix.write_state("available", installed=installed)
    shutil.copy(os.path.join(ROOT, "tools", "update.sh"), fix.exe())
    avant = open(fix.exe(), "rb").read()

    r = fix.update("--apply")
    check("code de retour non nul", r.returncode != 0)
    check("status=apply-failed", fix.get("status") == "apply-failed")
    check("le binaire en place n'a PAS bouge",
          open(fix.exe(), "rb").read() == avant)
    check("installed_commit inchange", fix.get("installed_commit") == installed)


def etape3_application(fix, installed):
    say("\n== 3. --apply installe quand la suite est verte")
    fix.write_project(version="v4", tests_ok=True)
    neuf = fix.commit("suite reparee")
    fix.write_state("available", installed=installed)

    r = fix.update("--apply")
    check("code de retour nul", r.returncode == 0)
    check("status=restart-pending", fix.get("status") == "restart-pending")
    check("installed_commit = le commit neuf", fix.get("installed_commit") == neuf)
    check("previous_commit = l'ancien", fix.get("previous_commit") == installed)
    check("le binaire pose est le neuf", marker(fix.exe()) == "MARQUE=v4")
    check("sshos.previous existe", os.path.exists(fix.previous()))
    return neuf


def etape4_concurrence(fix, installed):
    say("\n== 4. deux --apply concurrents ne detruisent pas .previous")
    fix.write_project(version="v5", tests_ok=True)
    fix.commit("cinquieme")
    fix.write_state("available", installed=installed)
    # On repose une version connue, pour savoir ce que .previous DOIT contenir.
    shutil.copy(fix.exe(), fix.previous())
    avant_previous = marker(fix.previous())

    # Deux processus lances coup sur coup. Le verrou doit en faire echouer un
    # proprement, ou les serialiser -- jamais les laisser s'entrelacer.
    a = subprocess.Popen(["sh", UPDATE_SH, "--apply"], env=fix.env(),
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    b = subprocess.Popen(["sh", UPDATE_SH, "--apply"], env=fix.env(),
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    a.communicate(timeout=900)
    b.communicate(timeout=900)

    apres_previous = marker(fix.previous())
    check("un seul a travaille (l'autre refuse ou attend)",
          a.returncode == 0 or b.returncode == 0)
    check("sshos.previous ne contient PAS le binaire neuf",
          apres_previous != marker(fix.exe()) or apres_previous == avant_previous)


def etape5_rollback(fix):
    say("\n== 5. --rollback remet le binaire ET reecrit l'etat")
    avant_exe = marker(fix.exe())
    avant_prev = marker(fix.previous())
    prev_commit = fix.get("previous_commit")

    r = fix.update("--rollback")
    check("code de retour nul", r.returncode == 0)
    check("le binaire est redevenu le precedent", marker(fix.exe()) == avant_prev)
    check("installed_commit = l'ancien commit",
          fix.get("installed_commit") == (prev_commit or "unknown"))
    check("previous_commit vide", fix.get("previous_commit") == "")
    check("status=available, on peut reappliquer",
          fix.get("status") == "available")
    check("le binaire neuf n'est plus en place", marker(fix.exe()) != avant_exe
          or avant_exe == avant_prev)


def etape6_historique_reecrit(fix):
    say("\n== 6. un historique reecrit n'est pas une mise a jour")
    # On reecrit le sommet du faux depot : le commit installe n'est plus un
    # ancetre de rien. C'est exactement ce que ce projet a fait deux fois.
    installed = fix.head()
    fix.write_state("up-to-date", installed=installed)
    git(fix.repo, "commit", "-q", "--amend", "-m", "sommet reecrit")

    r = fix.update("--check")
    check("code de retour nul", r.returncode == 0)
    check("status=history-rewritten",
          fix.get("status") == "history-rewritten")
    check("le message le dit", "reecrit" in fix.get("message"))


def etape7_etxtbsy(fix):
    say("\n== 7. remplacer un binaire PENDANT qu'un processus l'execute")
    # IL FAUT UN VRAI BINAIRE ELF. Avec un script shell, le noyau execute
    # /bin/sh et le fichier n'est PAS l'image en cours d'execution : aucun
    # ETXTBSY, et le cas ne prouverait rien. La sonde s'est fait prendre a ce
    # piege au premier essai.
    src = os.path.join(fix.base, "dormeur.cpp")
    with open(src, "w") as f:
        f.write("#include <unistd.h>\nint main(){ ::sleep(5); return 0; }\n")
    cible = os.path.join(fix.prefix, "libexec", "cible")
    c = run(["c++", "-O0", "-o", cible, src], timeout=120)
    if c.returncode != 0:
        check("compilation du dormeur", False)
        return

    p = subprocess.Popen([cible])
    try:
        # L'ecriture en place : elle DOIT echouer, c'est le piege.
        en_place_a_echoue = False
        try:
            with open(cible, "r+b") as f:
                f.write(b"\x7fELF")
        except OSError as e:
            en_place_a_echoue = True
            print("      (%s -- c'est ce qu'on attend)" % e.strerror)
        check("l'ecriture en place echoue bien (ETXTBSY)", en_place_a_echoue)

        # La sequence du script : copie dans .new, puis rename.
        neuf = cible + ".new"
        shutil.copy(cible, neuf)
        os.chmod(neuf, 0o755)
        renomme_ok = True
        try:
            os.rename(neuf, cible)
        except OSError:
            renomme_ok = False
        check("le rename passe, lui", renomme_ok)
        check("le processus qui l'executait vit toujours", p.poll() is None)
    finally:
        p.kill()
        p.wait()


def etape8_restart_pending_survit(fix):
    say("\n== 8. --check n'efface JAMAIS un redemarrage en attente")
    # RESTART-PENDING N'EST PAS UNE CONCLUSION, C'EST UN FAIT : un binaire est
    # pose et ce n'est pas celui qui tourne. Une verification ne regarde que
    # le depot distant -- elle n'observe pas ce fait, donc elle ne peut pas le
    # dementir. Seuls un --apply (qui repose un binaire) et le demon (qui
    # compare les inodes) le peuvent.
    #
    # Sans cette regle : la verification automatique tombe une fois par jour,
    # ecrit « up-to-date » par-dessus, la pastille s'eteint, l'entree redevient
    # « Verifier les mises a jour » -- et plus RIEN ne propose le redemarrage
    # alors que le binaire pose n'est toujours pas celui qui tourne. Le trou
    # se refermait tout seul, en silence.
    installed = fix.head()

    # a. rien de neuf en face : le fait survit
    fix.write_state("restart-pending", installed=installed)
    r = fix.update("--check")
    check("code de retour nul", r.returncode == 0)
    check("status TOUJOURS restart-pending (et non up-to-date)",
          fix.get("status") == "restart-pending")
    check("remote_commit quand meme mis a jour",
          fix.get("remote_commit") == installed)

    # b. le reseau tombe : le fait survit aussi. C'est le cas le plus
    #    probable des deux, et il ne compare meme pas.
    fix.write_state("restart-pending", installed=installed)
    env = fix.env()
    env["SSHOS_REPO_URL"] = os.path.join(fix.base, "depot-qui-n-existe-pas")
    r = run(["sh", UPDATE_SH, "--check"], env=env, timeout=120)
    check("code de retour non nul", r.returncode != 0)
    check("status TOUJOURS restart-pending (et non check-failed)",
          fix.get("status") == "restart-pending")

    # c. une version VRAIMENT plus recente, elle, a le droit de gagner : elle
    #    PROPOSE quelque chose, donc l'entree reste actionnable et
    #    l'application qui suit reposera un binaire de toute facon.
    fix.write_project(version="v9", tests_ok=True)
    plus_neuf = fix.commit("encore une")
    fix.write_state("restart-pending", installed=installed)
    r = fix.update("--check")
    check("code de retour nul", r.returncode == 0)
    check("status=available (une nouveaute prime)",
          fix.get("status") == "available")
    check("remote_commit = le commit neuf",
          fix.get("remote_commit") == plus_neuf)


def main():
    say("SONDE DE LA CHAINE DE MISE A JOUR")
    base = tempfile.mkdtemp(prefix="sshos-sonde-", dir="/var/tmp")
    say("terrain : %s" % base)
    try:
        fix = Fixture(base)
        first, second = etape1_detection(fix)
        etape2_refus_sur_rouge(fix, installed=second)
        neuf = etape3_application(fix, installed=second)
        etape4_concurrence(fix, installed=neuf)
        etape5_rollback(fix)
        etape6_historique_reecrit(fix)
        etape7_etxtbsy(fix)
        etape8_restart_pending_survit(fix)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    say("\n=== %s ===" % ("SONDE AU VERT" if ok_all else "LA SONDE A TROUVE UN DEFAUT"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
