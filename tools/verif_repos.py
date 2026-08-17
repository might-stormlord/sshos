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
BIN = _os.environ.get("SSHOS_DEV_BIN", "/home/storm/dev/ssh_os_2.0/build-release/sshos")


def daemon_pids():
    out = []
    me = os.getpid()
    for e in os.listdir("/proc"):
        if not e.isdigit() or int(e) == me:
            continue
        try:
            a = open("/proc/%s/cmdline" % e, "rb").read().split(b"\0")
            u = [l for l in open("/proc/%s/status" % e)
                 if l.startswith("Uid:")][0].split()[1]
        except OSError:
            continue
        if b"--daemon" in a and int(u) == os.getuid():
            out.append(int(e))
    return out


def kill_all():
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


def cpu_ticks(pid):
    f = open("/proc/%d/stat" % pid).read()
    # utime et stime sont les champs 14 et 15, apres le nom entre parentheses.
    tail = f[f.rindex(")") + 2:].split()
    return int(tail[11]) + int(tail[12])


def main():
    kill_all()
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.execv(BIN, [BIN])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

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
