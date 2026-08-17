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
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.execv(BIN, [BIN])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
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
