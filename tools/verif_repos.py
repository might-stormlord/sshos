#!/usr/bin/env python3
"""Le demon DETACHE doit rester au repos.

L'echeance de mise a jour est repliee dans le timeout d'epoll_wait SANS la
garde `if (client)`. Un delai qui rendrait 0 en permanence transformerait la
boucle en scrutin actif -- exactement le defaut que le commentaire de
daemon.cpp met en garde contre pour EPOLLHUP.

On attache un client, on le tue, et on mesure le temps CPU du demon reste
seul pendant plusieurs secondes.
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
BOOT = "verif-repos"
BIN = sonde.BIN


def daemon_pids():
    return sonde.demons(BOOT)


def kill_all():
    return sonde.kill_daemon(BOOT)


def cpu_ticks(pid):
    f = open("/proc/%d/stat" % pid).read()
    # utime et stime sont les champs 14 et 15, apres le nom entre parentheses.
    tail = f[f.rindex(")") + 2:].split()
    return int(tail[11]) + int(tail[12])


def main():
    kill_all()
    pid, fd = sonde.spawn(BOOT)

    # Laisser le bureau s'ouvrir.
    t0 = time.time()
    while time.time() - t0 < 2.5:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                os.read(fd, 65536)
            except OSError:
                break

    dpids = daemon_pids()
    if len(dpids) != 1:
        print("ECHEC : %d demons trouves" % len(dpids))
        kill_all()
        return 1
    dpid = dpids[0]

    # On tue le CLIENT : le demon reste seul, detache. C'est l'etat normal
    # de ce projet, et celui qu'on mesure.
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    os.close(fd)
    time.sleep(0.5)

    if not os.path.exists("/proc/%d" % dpid):
        print("ECHEC : le demon n'a pas survecu a son client")
        return 1

    hz = os.sysconf("SC_CLK_TCK")
    before = cpu_ticks(dpid)
    seconds = 6
    time.sleep(seconds)
    after = cpu_ticks(dpid)

    used = (after - before) / hz
    pct = 100.0 * used / seconds
    print("demon detache pid %d" % dpid)
    print("temps CPU sur %d s : %.3f s  (%.2f %%)" % (seconds, used, pct))

    kill_all()
    # Un demon au repos ne doit rien consommer. On laisse une marge tres
    # large : au-dela de 1 %, c'est un scrutin actif.
    ok = pct < 1.0
    print("=== %s ===" % ("AU REPOS" if ok else "SCRUTIN ACTIF"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
