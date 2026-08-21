#!/usr/bin/env python3
"""« Redemarrer pour terminer » doit vraiment redemarrer -- DEUX FOIS.

Quatre choses sont verifiees d'un coup, et aucune n'est atteignable par un
test unitaire :
  - le demon ferme son ecouteur AVANT d'annoncer, sinon le client se
    reconnecte au cadavre et sort sur un message trompeur ;
  - le client reconnait la raison et rejoue son chemin de demarrage au lieu
    de rendre la main au shell ;
  - un demon NEUF prend la place, avec un pid different ;
  - ET LE DEUXIEME REDEMARRAGE PASSE AUSSI.

Ce quatrieme point est la raison d'etre de ce fichier depuis le 21 aout
2026. `src/main.cpp` bornait la boucle a deux tours -- `for (int attempt =
0; attempt < 2; ++attempt)` -- en croyant empecher un emballement. Elle
comptait en realite les redemarrages de TOUTE LA VIE DU CLIENT : le
deuxieme etait refuse sans meme essayer de relancer un demon, sur
« sshos: le redemarrage n'a pas abouti », et l'utilisateur perdait son
bureau alors que rien n'etait casse. Un `sshos` retape repartait avec un
compteur neuf : d'ou le « une fois sur deux » exact que l'utilisateur
rapportait. Un seul redemarrage verifie ne voit RIEN de ce defaut -- c'est
precisement ce qui l'a laisse passer.

Le compte vit desormais dans `src/client/restart.hpp` (RestartBudget), a
portee de la suite ; ce script garde l'autre moitie, son cablage dans
main.cpp, que CMakeLists retire de sshos_core.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time

import os as _os
# La racine se deduit du fichier, jamais du chemin de la machine de
# l'auteur : le depot est public et personne d'autre n'a /home/storm.
_RACINE = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
DEV = _os.environ.get("TERMOS_DEV_BIN",
                      _os.path.join(_RACINE, "build-release", "sshos"))
BOOT = "verif-redemarrage"


def daemon_pids():
    """Les demons de CETTE instance, reconnus par leur TERMOS_BOOT_ID et non
    par un motif de nom."""
    out = []
    me = os.getpid()
    for e in os.listdir("/proc"):
        if not e.isdigit() or int(e) == me:
            continue
        try:
            a = open("/proc/%s/cmdline" % e, "rb").read().split(b"\0")
            env = open("/proc/%s/environ" % e, "rb").read().split(b"\0")
        except OSError:
            continue
        if b"--daemon" not in a:
            continue
        if ("TERMOS_BOOT_ID=" + BOOT).encode() in env:
            out.append(int(e))
    return out


def kill_ours():
    for p in daemon_pids():
        try:
            os.kill(p, signal.SIGTERM)
        except OSError:
            pass
    for _ in range(50):
        if not daemon_pids():
            return True
        time.sleep(0.1)
    return False


def drain(fd, seconds):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < seconds:
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


def main():
    root = "/var/tmp/sshos-verif-redemarrage"
    os.system("rm -rf %s && mkdir -p %s/sshos %s/libexec" % (root, root, root))
    with open(root + "/sshos/state", "w") as f:
        f.write("schema=1\nprefix=%s\nsource=git\nstatus=restart-pending\n" % root)

    env = dict(os.environ)
    env["XDG_DATA_HOME"] = root
    env["TERMOS_BOOT_ID"] = BOOT
    env["TERMOS_EXE"] = DEV
    env["TERM"] = "xterm-256color"
    env["COLORTERM"] = "truecolor"

    kill_ours()

    pid, fd = pty.fork()
    if pid == 0:
        os.environ.clear()
        os.environ.update(env)
        os.execv(DEV, [DEV])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    drain(fd, 2.5)

    first = daemon_pids()
    if len(first) != 1:
        print("ECHEC : %d demons au depart" % len(first))
        kill_ours()
        return 1
    old_pid = first[0]
    print("demon initial : %d" % old_pid)

    # DEUX TOURS, et le second est celui qui compte. Le fichier d'etat garde
    # `restart-pending` et le prefixe ne porte aucun binaire pose : l'entree
    # reste donc proposee apres le premier redemarrage, ce qui permet de la
    # recliquer exactement comme le ferait un utilisateur qui enchaine deux
    # mises a jour dans la meme session.
    sortie = b""
    courant = old_pid
    ok = True
    for tour in (1, 2):
        # Ctrl+A, Espace, puis filtrer sur « redem » et valider.
        os.write(fd, b"\x01")
        time.sleep(0.2)
        os.write(fd, b" ")
        sortie += drain(fd, 0.6)
        os.write(fd, b"redem")
        out = drain(fd, 0.8)
        sortie += out
        if b"Redemarrer" not in out:
            print("tour %d : ATTENTION, l'entree n'apparait pas dans le flux"
                  % tour)
        os.write(fd, b"\r")
        sortie += drain(fd, 0.6)

        # ALLER CHERCHER la confirmation : le focus est sur Annuler.
        os.write(fd, b"\t")
        time.sleep(0.2)
        os.write(fd, b"\r")

        # L'ancien demon doit mourir...
        gone = False
        for _ in range(80):
            if not os.path.exists("/proc/%d" % courant):
                gone = True
                break
            sortie += drain(fd, 0.1)
        print("tour %d : ancien demon sorti : %s"
              % (tour, "oui" if gone else "NON"))

        # ...et un NEUF doit prendre sa place, sans que l'utilisateur tape
        # quoi que ce soit. C'est le client qui rejoue son chemin de
        # demarrage.
        new_pid = None
        for _ in range(150):
            sortie += drain(fd, 0.1)
            ps = [p for p in daemon_pids() if p != courant]
            if ps:
                new_pid = ps[0]
                break
        print("tour %d : nouveau demon : %s"
              % (tour, new_pid if new_pid else "AUCUN"))

        tail = drain(fd, 1.5)
        sortie += tail
        reattached = new_pid is not None and len(tail) > 100
        print("tour %d : octets de bureau recus apres redemarrage : %d"
              % (tour, len(tail)))

        if not (gone and new_pid is not None and new_pid != courant
                and reattached):
            ok = False
            break
        courant = new_pid
        time.sleep(0.5)

    # LE MESSAGE QUI SIGNE LE DEFAUT. Il ne doit apparaitre nulle part : le
    # voir, c'est un client qui a rendu la main au shell sans meme essayer.
    if b"n'a pas abouti" in sortie:
        print("ECHEC : « le redemarrage n'a pas abouti » est apparu")
        ok = False

    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    os.close(fd)
    kill_ours()
    os.system("rm -rf %s" % root)

    print("=== %s ===" % ("DEUX REDEMARRAGES COMPLETS" if ok else "ECHEC"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
