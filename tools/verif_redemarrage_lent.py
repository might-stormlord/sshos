#!/usr/bin/env python3
"""Un demon LENT a demarrer ne doit pas couter le bureau.

La sonde tools/verif_redemarrage.py passe parce qu'elle fait tourner le meme
binaire du debut a la fin. Le cas reel est different, et c'est la difference
qui compte :

  - le client et le demon tournent l'ANCIEN binaire ;
  - la mise a jour pose un binaire NEUF sous eux, a la place de l'ancien ;
  - c'est le client ancien qui doit relancer le demon neuf, PAR CHEMIN ;
  - et le demon neuf met du TEMPS a se mettre a ecouter (RETARD secondes).

Le 19 aout 2026, ce dernier point a coute un bureau : le client accordait
au demon une seconde pile, puis rendait la main au shell. Le journal ne
portait qu'un trou de treize secondes. RETARD=3 rejoue exactement ca.

On rejoue donc le parcours entier depuis le bureau : « Mettre a jour »,
puis « Redemarrer », et on regarde ce que le client ecrit sur son terminal.

Isole par SSHOS_BOOT_ID : ne touche jamais au bureau vivant.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

BOOT = "verif-redemarrage-lent"
ROOT = "/var/tmp/sshos-verif-redemarrage-lent"
DEPOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# L'ancien binaire installe (celui que la mise a jour a remplace) et le neuf.
# Par defaut, le binaire de l'arbre des deux cotes : la sonde juge le code
# COURANT. SSHOS_ANCIEN permet de la rejouer contre une version anterieure.
ANCIEN = os.environ.get("SSHOS_ANCIEN",
                        os.path.join(DEPOT, "build-release/sshos"))
NEUF = os.environ.get("SSHOS_NEUF", os.path.join(DEPOT, "build-release/sshos"))

T0 = time.time()


def dit(msg):
    print("[%6.2fs] %s" % (time.time() - T0, msg), flush=True)


def demons():
    """Les demons de CETTE instance, reconnus par leur SSHOS_BOOT_ID -- jamais
    par un motif de nom : pgrep -f se trouve lui-meme."""
    out = []
    me = os.getpid()
    for e in os.listdir("/proc"):
        if not e.isdigit() or int(e) == me:
            continue
        try:
            cmd = open("/proc/%s/cmdline" % e, "rb").read().split(b"\0")
            env = open("/proc/%s/environ" % e, "rb").read().split(b"\0")
        except OSError:
            continue
        if b"--daemon" not in cmd:
            continue
        if ("SSHOS_BOOT_ID=" + BOOT).encode() in env:
            out.append(int(e))
    return sorted(out)


def tuer_les_notres():
    for p in demons():
        try:
            os.kill(p, signal.SIGTERM)
        except OSError:
            pass
    for _ in range(50):
        if not demons():
            return
        time.sleep(0.1)


def vider(fd, secondes):
    buf = b""
    t = time.time()
    while time.time() - t < secondes:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            c = os.read(fd, 65536)
        except OSError:
            break
        if not c:
            break
        buf += c
    return buf


def poser_le_decor():
    os.system("rm -rf %s" % ROOT)
    for d in ("prefix/bin", "prefix/libexec", "data/sshos"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    prefix = ROOT + "/prefix"

    # Le lanceur, tel que tools/install.sh l'ecrit.
    with open(prefix + "/bin/sshos", "w") as f:
        f.write(
            "#!/bin/sh\n"
            'SSHOS_BOOT_ID="${SSHOS_BOOT_ID:-%s}"\n'
            'SSHOS_EXE="%s/libexec/sshos-lent"\n'
            "export SSHOS_BOOT_ID SSHOS_EXE\n"
            'exec "$SSHOS_EXE" "$@"\n' % (BOOT, prefix)
        )
    os.chmod(prefix + "/bin/sshos", 0o755)

    # L'ENVELOPPEUR : il dort RETARD secondes puis exec le vrai binaire.
    # Le demon met donc ce temps-la a ecouter, et rien d'autre ne change.
    with open(prefix + "/libexec/sshos-lent", "w") as f:
        f.write('#!/bin/sh\n[ -f "%s/lent" ] && sleep %s\n'
                'exec "%s/libexec/sshos" "$@"\n'
                % (ROOT, os.environ.get("RETARD", "3"), prefix))
    os.chmod(prefix + "/libexec/sshos-lent", 0o755)

    # Le binaire installe : l'ANCIEN.
    os.system("cp %s %s/libexec/sshos" % (ANCIEN, prefix))

    # Le faux updater : il fait EXACTEMENT ce que fait tools/update.sh --apply
    # dans sa derniere etape -- deposer le binaire neuf et ecrire l'etat --
    # sans compiler quoi que ce soit.
    with open(prefix + "/libexec/sshos-update", "w") as f:
        f.write(
            "#!/bin/sh\n"
            "set -e\n"
            'ETAT="%s/data/sshos/state"\n'
            'PREFIX="%s"\n'
            'if [ "$1" = "--apply" ]; then\n'
            "  sleep 1\n"
            '  mv "$PREFIX/libexec/sshos" "$PREFIX/libexec/sshos.previous"\n'
            '  cp %s "$PREFIX/libexec/sshos"\n'
            '  printf \'schema=1\\nprefix=%%s\\nsource=git\\nstatus=restart-pending\\n'
            "installed_commit=neufneuf\\nprevious_commit=vieuxvieux\\n"
            "installed_version=9.9\\nremote_version=9.9\\n' \"$PREFIX\" > \"$ETAT.tmp\"\n"
            '  mv "$ETAT.tmp" "$ETAT"\n  : > "%s/lent"\n'
            "fi\n" % (ROOT, prefix, NEUF, ROOT)
        )
    os.chmod(prefix + "/libexec/sshos-update", 0o755)

    # L'etat de depart : une mise a jour est disponible.
    with open(ROOT + "/data/sshos/state", "w") as f:
        f.write(
            "schema=1\nprefix=%s\nsource=git\nstatus=available\n"
            "installed_commit=vieuxvieux\nremote_commit=neufneuf\n"
            "installed_version=9.8\nremote_version=9.9\n" % prefix
        )
    return prefix


def main():
    for b in (ANCIEN, NEUF):
        if not os.path.exists(b):
            print("ECHEC : binaire absent : %s" % b)
            return 2
    prefix = poser_le_decor()
    tuer_les_notres()

    env = dict(os.environ)
    env["XDG_DATA_HOME"] = ROOT + "/data"
    env["SSHOS_BOOT_ID"] = BOOT
    env["TERM"] = "xterm-256color"
    env["COLORTERM"] = "truecolor"
    env["HOME"] = ROOT
    for cle in ("SSHOS_EXE",):
        env.pop(cle, None)

    lanceur = prefix + "/bin/sshos"
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.clear()
        os.environ.update(env)
        os.execv("/bin/sh", ["/bin/sh", lanceur])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

    # ATTENDRE QUE LE BUREAU SOIT LA, pas un delai : le demon peut mettre
    # du temps a ecouter, et c'est justement la variable qu'on fait bouger.
    tout = b""
    t = time.time()
    while time.time() - t < 30.0:
        tout += vider(fd, 0.2)
        jr = ROOT + "/data/sshos/journal.log"
        if os.path.exists(jr) and "demarrage" in open(jr).read():
            break
    tout += vider(fd, 2.0)
    d = demons()
    if len(d) != 1:
        dit("ECHEC : %d demons au depart" % len(d))
        os.kill(pid, signal.SIGKILL)
        tuer_les_notres()
        return 1
    demon_ancien = d[0]
    inode_ancien = os.stat("/proc/%d/exe" % demon_ancien).st_ino
    dit("demon initial %d (inode %d)" % (demon_ancien, inode_ancien))

    # « Mettre a jour » : Ctrl+A, Espace, filtrer, Entree, puis confirmer.
    os.write(fd, b"\x01")
    time.sleep(0.2)
    os.write(fd, b" ")
    vider(fd, 0.6)
    os.write(fd, b"jour")
    out = vider(fd, 0.8)
    tout += out
    if b"jour" not in out and b"Mettre" not in out:
        dit("ATTENTION : l'entree ne se voit pas dans le flux differentiel")
    os.write(fd, b"\r")
    vider(fd, 0.6)
    os.write(fd, b"\t")
    time.sleep(0.2)
    os.write(fd, b"\r")
    dit("« Mettre a jour » confirme")

    # ATTENDRE LA CONDITION, pas un delai : le faux updater pose le binaire
    # neuf puis ecrit restart-pending. Un delai fixe fait deriver toute la
    # suite, et la sonde ment alors sur ce qu'elle a mesure.
    t = time.time()
    while time.time() - t < 20.0:
        tout += vider(fd, 0.2)
        try:
            if "status=restart-pending" in open(ROOT + "/data/sshos/state").read():
                break
        except OSError:
            pass
    etat = open(ROOT + "/data/sshos/state").read()
    if "status=restart-pending" not in etat:
        dit("ECHEC DE MONTAGE : l'application n'a pas eu lieu (status=%s)"
            % (re.search(r"status=(\S+)", etat).group(1) if "status=" in etat else "?"))
        for l in re.findall(r"sshos: [^\r\n\x1b]+", tout.decode("utf-8", "replace")):
            dit("   client : %s" % l.strip())
        os.kill(pid, signal.SIGKILL)
        tuer_les_notres()
        return 2
    inode_pose = os.stat(prefix + "/libexec/sshos").st_ino
    dit("applique : binaire pose inode %d (l'ancien tournait sur %d)"
        % (inode_pose, inode_ancien))
    # Laisser le demon recolter l'enfant et changer sa modale en question.
    tout += vider(fd, 1.5)

    # La modale doit maintenant proposer « Plus tard / Redemarrer ».
    # Le focus est sur « Plus tard » : Tab puis Entree.
    os.write(fd, b"\t")
    time.sleep(0.2)
    os.write(fd, b"\r")
    dit("« Redemarrer » confirme")

    # L'ancien demon doit sortir.
    sorti = False
    for _ in range(150):
        if not os.path.exists("/proc/%d" % demon_ancien):
            sorti = True
            break
        vider(fd, 0.1)
    dit("ancien demon sorti : %s" % ("oui" if sorti else "NON"))

    # Un demon NEUF doit prendre la place, sans que l'utilisateur tape rien.
    neuf = None
    t_attente = time.time()
    while time.time() - t_attente < 20.0:
        tout += vider(fd, 0.1)
        autres = [p for p in demons() if p != demon_ancien]
        if autres:
            neuf = autres[0]
            break
    if neuf:
        dit("nouveau demon %d apres %.1f s (inode %d)"
            % (neuf, time.time() - t_attente, os.stat("/proc/%d/exe" % neuf).st_ino))
    else:
        dit("AUCUN nouveau demon apres 20 s")

    tout += vider(fd, 3.0)

    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    os.close(fd)

    print("\n=== CE QUE LE CLIENT A ECRIT (lignes sshos:) ===")
    texte = tout.decode("utf-8", "replace")
    # UNE TRAME N'A PAS DE RETOUR A LA LIGNE : elle positionne le curseur.
    # Decouper en lignes rend donc tout le bureau dans une seule « ligne »
    # qui contient le message. On extrait le message lui-meme.
    vues = re.findall(r"sshos: [^\r\n\x1b]+", texte)
    for l in vues:
        print("   %s" % l.strip())
    if not vues:
        print("   (aucune)")

    print("\n=== JOURNAL DU DEMON ===")
    jr = ROOT + "/data/sshos/journal.log"
    print(open(jr).read().rstrip() if os.path.exists(jr) else "   (aucun)")

    tuer_les_notres()

    ok = sorti and neuf is not None and not any(
        "n'a pas abouti" in l or "pas repondu" in l for l in vues)
    print("\n=== %s ===" % ("REDEMARRAGE COMPLET" if ok else "REPRODUIT : ECHEC"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
