#!/usr/bin/env python3
"""LE HARNAIS DE CAMPAGNE DE MUTATION de ssh_os 2.0.

Une mutation remplace une ligne du code de production par une variante
FAUSSE, recompile, relance la suite filtree, restaure, recommence. Une
mutation « morte » est une mutation qu'un test a mordue ; une
« survivante » est presque toujours UN TROU DE TEST, pas une equivalence --
2 sur 246 seulement l'etaient au jalon 3.

Usage :
    cp tools/mutation.py /tmp/ma_campagne.py   # puis remplir FILES et M
    DRY=1 python3 /tmp/ma_campagne.py          # verifie que chaque motif
                                               # existe EXACTEMENT une fois
    python3 -u /tmp/ma_campagne.py > camp.log  # jamais derriere un tube
    python3 -u /tmp/ma_campagne.py M4 M7       # rejoue une selection

CINQ REGLES, toutes payees comptant :

1. COMMITER AVANT. La sauvegarde n'est fiable que sur un arbre propre.
2. SAUVEGARDE FRAICHE ET COMPLETE : `rm -rf <bak>` avant toute campagne
   qui suit une modification. Une sauvegarde de la campagne d'avant
   restaure du code perime et rend des verdicts inattribuables.
3. RESTAURER PAR copyfile + utime, JAMAIS copy2. `copy2` preserve la
   mtime, `make` ne recompile pas, et le binaire teste RESTE MUTE.
   Symptome a connaitre : `[100%] Built target` sans ligne de compilation.
4. NE RIEN LIRE DANS src/ PENDANT LA CAMPAGNE : le fichier y est faux.
   Lire `<bak>/`. Apres tout arret anormal : `diff -q <bak>/<f> src/<f>`
   AVANT de croire quoi que ce soit.
5. VERIFIER LE FILTRE. Une campagne dont FILTERS ne couvre pas les cas qui
   mordent rend « 8 survivantes » qui n'en sont pas. Arrive le 14 aout 2026
   avec un filtre ["files_"] qui ne voyait pas les cas "copy_".

Et : une mutation qui NE COMPILE PAS n'est pas une survivante. `-Werror`
refuse une variable devenue inutilisee ; c'est une mutation INVALIDE, a
compter comme telle. « Toutes les mutations ne compilent pas » veut dire
que c'est la BASE qui ne compile pas.

Enfin : tuer les campagnes orphelines apres tout redemarrage. Un travail de
fond survit a une coupure et continue de muter src/ sous les doigts.
    pgrep -af mutation
"""
import io, os, shutil, subprocess, sys
ROOT = "/home/storm/dev/ssh_os_2.0"
BAK = "/tmp/sshos-mutation-bak"
# Les fichiers mutes. METTRE LES .hpp AUSSI s'ils sont touches.
FILES = ["src/apps/files/files.cpp"]
# Les prefixes de TEST( a relancer. Trop etroit = fausses survivantes.
FILTERS = ["files_"]
if not os.path.isdir(BAK):
    os.makedirs(BAK)
    for f in FILES:
        shutil.copyfile(os.path.join(ROOT, f), os.path.join(BAK, f.replace("/", "_")))
    print("sauvegarde fraiche creee dans", BAK)
def restore():
    for f in FILES:
        dst = os.path.join(ROOT, f)
        shutil.copyfile(os.path.join(BAK, f.replace("/", "_")), dst)
        os.utime(dst, None)

S = "src/daemon/session.cpp"
D = "src/daemon/daemon.cpp"
P = "src/pty/pty.cpp"
S = "src/daemon/session.cpp"
A = "src/shell/snapassist.cpp"
F = "src/apps/files/files.cpp"
C = "src/apps/files/copy.cpp"
F = "src/apps/files/files.cpp"

# (nom, fichier, motif EXACT a remplacer, remplacement)
# Le motif doit exister exactement une fois : DRY=1 le verifie.
M = [
 ("M1 exemple : une garde retiree", F,
  "  if (pane().visible.empty()) return false;\n", ""),
]

only = sys.argv[1:]
if only: M = [m for m in M if m[0].split()[0] in only]
if os.environ.get("DRY"):
    bad = 0
    for name, rel, old, new in M:
        s = io.open(os.path.join(ROOT, rel), encoding="utf-8").read()
        n = s.count(old)
        if n != 1:
            bad += 1; print("!! %-50s MOTIF (%d)" % (name, n))
    print("=== %d a corriger sur %d ===" % (bad, len(M))); sys.exit(0)
survivors, broken = [], []
for name, rel, old, new in M:
    restore()
    full = os.path.join(ROOT, rel)
    s = io.open(full, encoding="utf-8").read()
    if s.count(old) != 1:
        broken.append((name, "motif")); print("!! %-50s MOTIF (%d)" % (name, s.count(old)), flush=True); continue
    io.open(full, "w", encoding="utf-8").write(s.replace(old, new))
    b = subprocess.run(["cmake", "--build", "build-release", "-j%d" % os.cpu_count()],
                       cwd=ROOT, capture_output=True, text=True, errors="replace")
    if " error" in b.stdout + b.stderr:
        broken.append((name, "ne compile pas")); print("!! %-50s NE COMPILE PAS" % name, flush=True); continue
    dead, line = False, ""
    for f in FILTERS:
        try:
            r = subprocess.run(["./build-release/sshos_tests", f], cwd=ROOT,
                               capture_output=True, text=True, timeout=300, errors="replace")
            t = [l for l in r.stdout.splitlines() if "en echec" in l]
            cur = t[-1] if t else "PAS DE BILAN"
            if ("0 en echec" not in cur) or r.returncode != 0:
                dead, line = True, "%s: %s" % (f, cur); break
        except subprocess.TimeoutExpired:
            dead, line = True, "%s: DELAI DEPASSE" % f; break
    print("%-4s %-50s %s" % ("mort" if dead else "VIT", name, line), flush=True)
    if not dead: survivors.append(name)
restore()
subprocess.run(["cmake", "--build", "build-release", "-j%d" % os.cpu_count()], cwd=ROOT, capture_output=True)
print("\n=== %d survivants, %d cassees ===" % (len(survivors), len(broken)))
for s_ in survivors: print("  VIT :", s_)
for n, w in broken: print("  CASSEE :", n, "--", w)
