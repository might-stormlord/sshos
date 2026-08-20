#!/usr/bin/env python3
"""« Fermer la session » doit encore tuer le demon.

wants_quit() etait lu dans la branche EPOLLIN du client ; il est desormais
lu en fin de corps de boucle. Aucun test de la suite ne couvre ce chemin --
il n'est verifie qu'au niveau de la Session, jamais a travers la boucle du
demon. C'est exactement le defaut « ne sans appelant » du projet, alors on
le verifie pour de vrai.
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
import sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import sonde  # noqa: E402

# NOTRE ETIQUETTE, ET RIEN D'AUTRE NE SERA TUE. L'enumeration par
# « --daemon dans cmdline + uid » qui vivait ici trouvait le bureau INSTALLE
# de la machine -- il porte exactement ces deux marques -- et le tuait, avec
# la session de travail qui tourne dedans. sonde.spawn(BOOT) pose
# SSHOS_BOOT_ID dans l'enfant, sonde.demons(BOOT) le relit dans /proc.
BOOT = "verif-sortie"
BIN = sonde.BIN


def daemon_pids():
    return sonde.demons(BOOT)


def kill_all():
    return sonde.kill_daemon(BOOT)


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
    kill_all()
    pid, fd = sonde.spawn(BOOT)
    drain(fd, 2.5)

    dpids = daemon_pids()
    if len(dpids) != 1:
        print("ECHEC : %d demons" % len(dpids))
        kill_all()
        return 1
    dpid = dpids[0]
    print("demon %d en place" % dpid)

    # Ctrl+A puis Espace ouvre le menu, « fermer » le filtre, Entree valide.
    os.write(fd, b"\x01")
    time.sleep(0.2)
    os.write(fd, b" ")
    drain(fd, 0.6)
    os.write(fd, b"fermer")
    drain(fd, 0.6)
    os.write(fd, b"\r")
    out = drain(fd, 0.8)
    if b"Fermer la session" not in out and b"perdues" not in out:
        print("(la modale n'est pas visible dans le flux differentiel, on continue)")

    # ALLER CHERCHER la confirmation : Tab puis Entree. Entree seule ne
    # detruit rien, le focus est sur Annuler.
    os.write(fd, b"\t")
    time.sleep(0.2)
    os.write(fd, b"\r")

    # Le demon doit mourir de lui-meme, sans qu'on tape autre chose.
    dead = False
    for _ in range(60):
        if not os.path.exists("/proc/%d" % dpid):
            dead = True
            break
        time.sleep(0.1)

    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    os.close(fd)
    kill_all()

    print("=== %s ===" % ("LE DEMON EST SORTI" if dead
                          else "ECHEC : le demon ne sort pas"))
    return 0 if dead else 1


if __name__ == "__main__":
    sys.exit(main())
