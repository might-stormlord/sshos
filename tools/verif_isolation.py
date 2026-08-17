#!/usr/bin/env python3
"""Verifie que le bureau INSTALLE et l'arbre de DEVELOPPEMENT s'ignorent.

C'est le test qui juge la phase 1 entiere. Le scenario est celui qui a
motive tout le travail : l'utilisateur travaille sur le projet depuis un
terminal du bureau installe, et les outils de developpement ne doivent
jamais pouvoir l'atteindre.

Technique reprise de tools/sonde.py : pty.fork() + execv du LANCEUR, sans
--daemon. Lancer « sshos --daemon » directement bloquerait -- become_daemon()
ne forke pas, il execute la boucle du demon dans le processus appelant ; le
detachement vient de spawn_detached, cote client.

usage: verif_isolation.py <HOME de l'installation> [<racine de l'arbre de dev>]
"""
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

DEV_ROOT = "/home/storm/dev/ssh_os_2.0"


def daemon_pids():
    """Les demons de CET utilisateur, trouves par /proc. Jamais par un motif
    de nom : une sonde qui se cherche par motif se trouve elle-meme."""
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


def kill_pids(pids):
    for p in pids:
        try:
            os.kill(p, signal.SIGTERM)
        except OSError:
            pass
    for _ in range(50):
        if not [p for p in pids if os.path.exists("/proc/%d" % p)]:
            return True
        time.sleep(0.1)
    return False


def status(binary, env_extra=None, home=None):
    env = dict(os.environ)
    if home:
        env["HOME"] = home
    if env_extra:
        env.update(env_extra)
    r = subprocess.run([binary, "--status"], capture_output=True, text=True,
                       timeout=15, env=env)
    return r.stdout.strip()


def kill_via(binary, env_extra=None, home=None):
    env = dict(os.environ)
    if home:
        env["HOME"] = home
    if env_extra:
        env.update(env_extra)
    r = subprocess.run([binary, "--kill"], capture_output=True, text=True,
                       timeout=15, env=env)
    return r.stdout.strip()


def attach(launcher, home):
    env = dict(os.environ)
    env["HOME"] = home
    env["TERM"] = "xterm-256color"
    env["COLORTERM"] = "truecolor"
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.clear()
        os.environ.update(env)
        os.execv(launcher, [launcher])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    return pid, fd


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
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    home = sys.argv[1]
    dev_root = sys.argv[2] if len(sys.argv) > 2 else DEV_ROOT

    launcher = os.path.join(home, ".local/bin/sshos")
    dev_bin = os.path.join(dev_root, "build-release/sshos")
    for p in (launcher, dev_bin):
        if not os.path.exists(p):
            print("ECHEC : %s est absent" % p)
            return 1

    print("== Terrain net : on tue tout demon prealable")
    kill_pids(daemon_pids())
    if daemon_pids():
        print("ECHEC : un demon residuel refuse de mourir")
        return 1

    ok = True
    client_pid = None
    client_fd = None
    try:
        print("\n== 1. Ouvrir le bureau INSTALLE")
        client_pid, client_fd = attach(launcher, home)
        out = drain(client_fd, 3.0)
        print("   octets de bureau recus : %d" % len(out))
        before = daemon_pids()
        st_inst = status(launcher, home=home)
        print("   --status (installe) : %s" % st_inst)
        if len(out) < 200 or not st_inst.startswith("demon actif"):
            print("   ECHEC : le bureau installe n'a pas demarre")
            return 1
        installed_pid = int(st_inst.split("pid ")[1].rstrip(")"))
        print("   pid du demon installe : %d" % installed_pid)

        print("\n== 2. L'arbre de DEV le voit-il ? (il ne doit PAS)")
        st_dev = status(dev_bin)
        print("   --status (dev) : %s" % st_dev)
        if st_dev != "aucun demon":
            print("   ECHEC : le binaire de dev voit le bureau installe")
            ok = False

        print("\n== 3. Le --kill de DEV peut-il l'atteindre ? (il ne doit PAS)")
        out_kill = kill_via(dev_bin)
        print("   --kill (dev) : %s" % out_kill)
        if out_kill != "aucun demon":
            print("   ECHEC : le --kill de dev a trouve quelque chose")
            ok = False

        print("\n== 4. Le bureau installe est-il TOUJOURS vivant ?")
        time.sleep(0.5)
        st_after = status(launcher, home=home)
        print("   --status (installe) : %s" % st_after)
        if not st_after.startswith("demon actif"):
            print("   ECHEC : le bureau installe est mort -- l'isolation ne tient pas")
            ok = False
        elif os.path.exists("/proc/%d" % installed_pid):
            print("   OK : meme pid, toujours la")
        else:
            print("   ECHEC : le pid a change")
            ok = False

        print("\n== 5. Le lanceur pose-t-il bien l'identite ?")
        launcher_text = open(launcher).read()
        for needle in ("SSHOS_BOOT_ID", "SSHOS_EXE", "exec "):
            if needle not in launcher_text:
                print("   ECHEC : %s absent du lanceur" % needle)
                ok = False
        if ok:
            print("   OK : SSHOS_BOOT_ID, SSHOS_EXE et exec sont la")

        print("\n== 6. L'environnement du demon installe porte-t-il l'identite ?")
        try:
            env_raw = open("/proc/%d/environ" % installed_pid, "rb").read()
            env = dict(
                kv.split(b"=", 1) for kv in env_raw.split(b"\0")
                if b"=" in kv)
            boot = env.get(b"SSHOS_BOOT_ID", b"<absente>").decode()
            exe = env.get(b"SSHOS_EXE", b"<absente>").decode()
            print("   SSHOS_BOOT_ID = %s" % boot)
            print("   SSHOS_EXE     = %s" % exe)
            if boot == "<absente>":
                print("   ECHEC : le demon n'a pas herite de l'identite")
                ok = False
        except OSError as e:
            print("   (non lisible : %s)" % e)

    finally:
        if client_pid:
            try:
                os.kill(client_pid, signal.SIGKILL)
                os.waitpid(client_pid, 0)
            except OSError:
                pass
        if client_fd is not None:
            try:
                os.close(client_fd)
            except OSError:
                pass
        # Le demon SURVIT au client : c'est tout l'objet du projet. Il faut
        # donc le tuer explicitement, sinon l'essai suivant mesurerait celui-ci.
        kill_pids(daemon_pids())

    print("\n=== %s ===" % ("ISOLATION VERIFIEE" if ok else "L'ISOLATION NE TIENT PAS"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
