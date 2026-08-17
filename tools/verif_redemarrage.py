#!/usr/bin/env python3
"""« Redemarrer pour terminer » doit vraiment redemarrer.

Trois choses sont verifiees d'un coup, et aucune n'est atteignable par un
test unitaire :
  - le demon ferme son ecouteur AVANT d'annoncer, sinon le client se
    reconnecte au cadavre et sort sur un message trompeur ;
  - le client reconnait la raison et rejoue son chemin de demarrage au lieu
    de rendre la main au shell ;
  - un demon NEUF prend la place, avec un pid different.
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
DEV = _os.environ.get("SSHOS_DEV_BIN", "/home/storm/dev/ssh_os_2.0/build-release/sshos")
BOOT = "verif-redemarrage"


def daemon_pids():
    """Les demons de CETTE instance, reconnus par leur SSHOS_BOOT_ID et non
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
        if ("SSHOS_BOOT_ID=" + BOOT).encode() in env:
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
    env["SSHOS_BOOT_ID"] = BOOT
    env["SSHOS_EXE"] = DEV
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

    # Ctrl+A, Espace, puis filtrer sur « redem » et valider.
    os.write(fd, b"\x01")
    time.sleep(0.2)
    os.write(fd, b" ")
    drain(fd, 0.6)
    os.write(fd, b"redem")
    out = drain(fd, 0.8)
    if b"Redemarrer" not in out:
        print("ATTENTION : l'entree n'apparait pas dans le flux (differentiel)")
    os.write(fd, b"\r")
    drain(fd, 0.6)

    # ALLER CHERCHER la confirmation : le focus est sur Annuler.
    os.write(fd, b"\t")
    time.sleep(0.2)
    os.write(fd, b"\r")

    # L'ancien demon doit mourir...
    gone = False
    for _ in range(80):
        if not os.path.exists("/proc/%d" % old_pid):
            gone = True
            break
        time.sleep(0.1)
    print("ancien demon sorti : %s" % ("oui" if gone else "NON"))

    # ...et un NEUF doit prendre sa place, sans que l'utilisateur tape quoi
    # que ce soit. C'est le client qui rejoue son chemin de demarrage.
    new_pid = None
    for _ in range(100):
        drain(fd, 0.1)
        ps = [p for p in daemon_pids() if p != old_pid]
        if ps:
            new_pid = ps[0]
            break
    print("nouveau demon : %s" % (new_pid if new_pid else "AUCUN"))

    tail = drain(fd, 1.5)
    reattached = new_pid is not None and len(tail) > 100
    print("octets de bureau recus apres redemarrage : %d" % len(tail))

    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    os.close(fd)
    kill_ours()
    os.system("rm -rf %s" % root)

    ok = gone and new_pid is not None and new_pid != old_pid and reattached
    print("=== %s ===" % ("REDEMARRAGE COMPLET" if ok else "ECHEC"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
