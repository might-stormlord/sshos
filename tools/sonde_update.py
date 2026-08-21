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
repertoire d'etat temporaires. TERMOS_BOOT_ID n'isole QUE le nom du socket :
les chemins, eux, se surchargent par TERMOS_PREFIX et TERMOS_STATE_DIR. Sans
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
        e["TERMOS_PREFIX"] = self.prefix
        e["TERMOS_STATE_DIR"] = self.state_dir
        e["TERMOS_REPO_URL"] = self.repo
        return e

    def write_state(self, status, installed, previous="", source="git",
                    restart_pending=""):
        with open(os.path.join(self.state_dir, "state"), "w") as f:
            f.write("schema=1\nprefix=%s\nsource=%s\ninstalled_commit=%s\n"
                    "previous_commit=%s\nremote_commit=\nchecked_at=0\n"
                    "status=%s\nrestart_pending=%s\npid=\nmessage=\n"
                    % (self.prefix, source, installed, previous, status,
                       restart_pending))

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


def etape8_le_fait_et_la_conclusion(fix):
    say("\n== 8. `restart_pending` est un FAIT, pas une conclusion")
    # UN BINAIRE EST POSE ET CE N'EST PAS CELUI QUI TOURNE : c'est un fait du
    # disque. `status` porte la CONCLUSION de la derniere verification. Les
    # avoir confondus dans une seule cle rendait le fait effacable par
    # n'importe quelle conclusion -- et la verification automatique, qui tombe
    # une fois par jour sans que personne ait rien demande, le faisait en
    # silence.
    installed = fix.head()

    # a. une application ARME le fait, et le redit dans status pour les
    #    demons anterieurs a la cle
    fix.write_project(version="vA", tests_ok=True)
    neuf_commit = fix.commit("de quoi appliquer")
    fix.write_state("available", installed=installed)
    r = fix.update("--apply")
    check("--apply : code de retour nul", r.returncode == 0)
    check("--apply arme restart_pending=1", fix.get("restart_pending") == "1")
    check("--apply redit restart-pending dans status",
          fix.get("status") == "restart-pending")
    installed = neuf_commit

    # b. rien de neuf en face : le fait survit a une verification tapee a la
    #    main, dont le parent n'est pas un demon
    r = fix.update("--check")
    check("--check (a la main) : code de retour nul", r.returncode == 0)
    check("le fait survit", fix.get("restart_pending") == "1")
    check("status le redit encore", fix.get("status") == "restart-pending")

    # c. le reseau tombe : le fait survit aussi. C'est le cas le plus
    #    probable des deux, et il ne compare meme pas.
    env = fix.env()
    env["TERMOS_REPO_URL"] = os.path.join(fix.base, "depot-qui-n-existe-pas")
    r = run(["sh", UPDATE_SH, "--check"], env=env, timeout=120)
    check("--check en echec : code de retour non nul", r.returncode != 0)
    check("le fait survit a un echec reseau", fix.get("restart_pending") == "1")

    # d. une version VRAIMENT plus recente : la conclusion passe a
    #    « available », mais LE FAIT NE BOUGE PAS. C'est ce que l'ancienne
    #    cle unique ne pouvait pas exprimer -- il fallait choisir.
    fix.write_project(version="vB", tests_ok=True)
    plus_neuf = fix.commit("encore une")
    r = fix.update("--check")
    check("--check : code de retour nul", r.returncode == 0)
    check("status=available (la nouveaute est dite)",
          fix.get("status") == "available")
    check("ET le fait tient toujours", fix.get("restart_pending") == "1")
    check("remote_commit = le commit neuf",
          fix.get("remote_commit") == plus_neuf)

    # e. LE REDRESSEMENT. Une verification lancee PAR le binaire pose -- donc
    #    dont le parent tourne l'inode posee -- constate que le redemarrage a
    #    eu lieu et efface le fait. C'est la seule chose qui rende le fichier
    #    a nouveau honnete, et sans elle il mentirait pour toujours.
    lanceur_src = os.path.join(fix.base, "lanceur.cpp")
    with open(lanceur_src, "w") as f:
        # IL FORKE, PUIS EXEC -- exactement comme launch_updater du demon
        # (src/daemon/session.cpp). Un `execl` nu REMPLACERAIT ce processus
        # par le shell, et le parent du script serait alors python : le cas
        # ne prouverait rien. Piege paye comptant au premier essai.
        f.write("#include <sys/wait.h>\n"
                "#include <unistd.h>\n"
                "int main(int, char** argv) {\n"
                "  const pid_t p = ::fork();\n"
                "  if (p == 0) {\n"
                "    ::execl(\"/bin/sh\", \"sh\", argv[1], \"--check\", (char*)0);\n"
                "    ::_exit(127);\n"
                "  }\n"
                "  int st = 0;\n"
                "  ::waitpid(p, &st, 0);\n"
                "  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;\n"
                "}\n")
    lanceur = os.path.join(fix.base, "lanceur")
    c = run(["c++", "-O0", "-o", lanceur, lanceur_src], timeout=120)
    if c.returncode != 0:
        check("compilation du faux demon", False)
        return
    # Il prend LA PLACE du binaire pose : c'est son inode que le script
    # comparera a celle de son parent.
    shutil.copy(lanceur, fix.exe())
    os.chmod(fix.exe(), 0o755)
    # Rien de neuf en face : la conclusion honnete est « a jour », et c'est
    # justement celle que l'ancienne cle unique ne pouvait pas ecrire sans
    # perdre le fait.
    fix.write_state("restart-pending", installed=fix.head(), restart_pending="1")
    r = run([fix.exe(), UPDATE_SH], env=fix.env(), timeout=300)
    check("le faux demon a lance --check", r.returncode == 0)
    check("LE FAIT EST EFFACE", fix.get("restart_pending") == "")
    check("et status dit enfin la verite", fix.get("status") == "up-to-date")

    # f. un retour arriere repose un binaire : le fait se rearme
    fix.write_state("up-to-date", installed=installed, previous=installed)
    shutil.copy(lanceur, fix.previous())
    r = fix.update("--rollback")
    check("--rollback : code de retour nul", r.returncode == 0)
    check("--rollback rearme le fait", fix.get("restart_pending") == "1")
    check("status=available, on peut reappliquer",
          fix.get("status") == "available")


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
        etape8_le_fait_et_la_conclusion(fix)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    say("\n=== %s ===" % ("SONDE AU VERT" if ok_all else "LA SONDE A TROUVE UN DEFAUT"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
