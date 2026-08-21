#!/usr/bin/env python3
"""Reinstaller pendant qu'un bureau tourne.

Installer, c'est repartir a neuf. Mais l'installeur est AUSSI le chemin de
mise a jour de repli -- quand le prefixe est systeme, ou qu'il n'y a pas de
source distante -- donc il tourne parfois avec un demon vivant. Ce demon
continuerait sur l'ANCIEN binaire apres la pose, sans que rien ne le
signale : c'est le meme mensonge que RestartPending corrige du cote interne.

Trois cas, et aucun n'est atteignable par un test unitaire : il faut un vrai
demon, un vrai PTY et un vrai remplacement de binaire.

  1. aucun demon      -> l'installeur ne dit RIEN et installe
  2. un demon vivant  -> il previent, demande, arrete, puis installe
  3. on refuse        -> il abandonne et le bureau est INTACT

Aucune commande detachee. Le demon lance ici est tue avant de sortir : il
survit au client, c'est tout l'objet du projet.
"""
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = "/var/tmp/sshos-verif-bureau-ouvert"
# UNE INSTANCE A NOUS, ET SURTOUT PAS LE DEFAUT. « bureau01 » est le nom
# que l installeur donne au bureau reel de la machine : install.sh --kill
# viserait alors le socket termos/<uid>/bureau01, c est-a-dire le bureau
# vivant -- et la session de travail qui tourne dedans. Le filtre par HOME
# de daemons() ne protege pas de ca : c est install.sh qui tue, pas nous.
INSTANCE = "verif-bureau-ouvert"

ok_all = True


def check(label, cond, extra=""):
    global ok_all
    print("   %-52s %s%s" % (label, "OK" if cond else "ECHEC",
                             ("  " + extra) if extra and not cond else ""),
          flush=True)
    if not cond:
        ok_all = False
    return cond


def install(*args, stdin=None):
    env = dict(os.environ)
    env["HOME"] = HOME
    cmd = ["sh", os.path.join(ROOT, "tools", "install.sh"),
           "--instance", INSTANCE] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, env=env,
                          input=stdin, timeout=1800)


def daemons():
    """Les demons de CETTE installation, reconnus par leur environnement et
    non par un motif de nom : une sonde qui se cherche par motif se trouve
    elle-meme."""
    out = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            a = open("/proc/%s/cmdline" % e, "rb").read().split(b"\0")
            env = open("/proc/%s/environ" % e, "rb").read()
        except OSError:
            continue
        if (b"--daemon" in a and HOME.encode() in env
                and ("TERMOS_BOOT_ID=" + INSTANCE).encode() in env):
            out.append(int(e))
    return out


def open_desktop():
    launcher = os.path.join(HOME, ".local/bin/termos")
    env = dict(os.environ)
    env["HOME"] = HOME
    env["TERM"] = "xterm-256color"
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.clear()
        os.environ.update(env)
        os.execv(launcher, [launcher])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    t0 = time.time()
    while time.time() - t0 < 3:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                os.read(fd, 65536)
            except OSError:
                break
    return pid, fd


def close_client(pid, fd):
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    try:
        os.close(fd)
    except OSError:
        pass


def kill_daemons():
    for p in daemons():
        try:
            os.kill(p, signal.SIGTERM)
        except OSError:
            pass
    for _ in range(50):
        if not daemons():
            return
        time.sleep(0.1)


def main():
    shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME)
    try:
        print("== 1. Aucun demon : l'installeur ne dit rien")
        r = install("--yes", "--source", "local", "--path", "no")
        check("installation reussie", r.returncode == 0, r.stderr[-300:])
        check("aucun avertissement", "il sera ARRETE" not in r.stdout)

        print("\n== 2. Un demon vivant : prevenu, arrete, installe")
        pid, fd = open_desktop()
        check("le bureau a demarre", len(daemons()) == 1)
        r = install("--yes", "--source", "local", "--path", "no")
        check("l'installeur previent", "il sera ARRETE" in r.stdout)
        check("il annonce l'arret", "bureau arrete" in r.stdout)
        check("l'installation aboutit", r.returncode == 0, r.stderr[-300:])
        time.sleep(0.5)
        check("le demon est bien parti", daemons() == [])
        close_client(pid, fd)

        print("\n== 3. On refuse : le bureau est intact")
        pid, fd = open_desktop()
        before = daemons()
        check("le bureau a redemarre", len(before) == 1)
        r = install("--source", "local", "--path", "no", stdin="non\n")
        check("l'installeur abandonne", r.returncode != 0)
        check("il le dit clairement", "abandonnee" in r.stderr)
        check("le demon est intact", daemons() == before,
              "avant %s apres %s" % (before, daemons()))
        close_client(pid, fd)
    finally:
        kill_daemons()
        shutil.rmtree(HOME, ignore_errors=True)

    print("\n=== %s ===" % ("CONFORME" if ok_all else "UN CAS NE PASSE PAS"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
