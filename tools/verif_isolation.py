#!/usr/bin/env python3
"""Verifie que le bureau INSTALLE et l'arbre de DEVELOPPEMENT s'ignorent.

C'est le test qui juge la phase 1 entiere. Le scenario est celui qui a
motive tout le travail : l'utilisateur travaille sur le projet depuis un
terminal du bureau installe, et les outils de developpement ne doivent
jamais pouvoir l'atteindre.

Technique reprise de tools/sonde.py : pty.fork() + execv du LANCEUR, sans
--daemon. Lancer « termos --daemon » directement bloquerait -- become_daemon()
ne forke pas, il execute la boucle du demon dans le processus appelant ; le
detachement vient de spawn_detached, cote client.

usage: verif_isolation.py <HOME de l'installation> [<racine de l'arbre de dev>]
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

# La racine se deduit du fichier, jamais du chemin de la machine de
# l'auteur : le depot est public et personne d'autre n'a /home/storm.
DEV_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def boot_du_lanceur(launcher):
    """L'instance que ce lanceur vise : install.sh y grave
    TERMOS_BOOT_ID="${TERMOS_BOOT_ID:-<instance>}". C'est la SEULE chose qui
    separe deux bureaux (docs/REPRISE.md §2 ter)."""
    m = re.search(r'TERMOS_BOOT_ID="\$\{TERMOS_BOOT_ID:-([^}]*)\}"',
                  open(launcher).read())
    return m.group(1) if m else None


def daemon_pids(boot):
    """Les demons de L'INSTANCE TESTEE, reconnus par leur TERMOS_BOOT_ID.

    JAMAIS par « --daemon dans cmdline + uid » : le bureau installe de la
    machine porte exactement ces deux marques, et la session de travail
    tourne dedans. Cette sonde le tuait a sa premiere instruction.
    """
    if not boot:
        return []
    marque = ("TERMOS_BOOT_ID=" + boot).encode()
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
        if b"--daemon" in a and marque in env:
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

    launcher = os.path.join(home, ".local/bin/termos")
    dev_bin = os.path.join(dev_root, "build-release/termos")
    for p in (launcher, dev_bin):
        if not os.path.exists(p):
            print("ECHEC : %s est absent" % p)
            return 1

    boot = boot_du_lanceur(launcher)
    if not boot:
        print("ECHEC : le lanceur %s ne grave aucun TERMOS_BOOT_ID" % launcher)
        return 1
    print("== Instance visee : %s" % boot)

    # ON NE TUE PAS UN BUREAU QU'ON N'A PAS LEVE. Si l'instance est deja
    # vivante, ce n'est pas la notre : c'est celle de l'utilisateur, et la
    # session de travail tourne peut-etre dedans. Refuser bruyamment vaut
    # infiniment mieux que balayer en silence -- installer avec l'instance
    # par defaut « bureau01 » suffit a viser le vrai bureau.
    deja = daemon_pids(boot)
    if deja:
        print("REFUS : un demon de l'instance « %s » tourne deja (pid %s)."
              % (boot, ", ".join(str(p) for p in deja)))
        print("        Reinstallez le HOME d'essai avec une autre instance :")
        print("        sh tools/install.sh --instance verif-isolation ...")
        return 1

    ok = True
    client_pid = None
    client_fd = None
    try:
        print("\n== 1. Ouvrir le bureau INSTALLE")
        client_pid, client_fd = attach(launcher, home)
        out = drain(client_fd, 3.0)
        print("   octets de bureau recus : %d" % len(out))
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
        for needle in ("TERMOS_BOOT_ID", "TERMOS_EXE", "exec "):
            if needle not in launcher_text:
                print("   ECHEC : %s absent du lanceur" % needle)
                ok = False
        if ok:
            print("   OK : TERMOS_BOOT_ID, TERMOS_EXE et exec sont la")

        print("\n== 6. L'environnement du demon installe porte-t-il l'identite ?")
        try:
            env_raw = open("/proc/%d/environ" % installed_pid, "rb").read()
            env = dict(
                kv.split(b"=", 1) for kv in env_raw.split(b"\0")
                if b"=" in kv)
            boot = env.get(b"TERMOS_BOOT_ID", b"<absente>").decode()
            exe = env.get(b"TERMOS_EXE", b"<absente>").decode()
            print("   TERMOS_BOOT_ID = %s" % boot)
            print("   TERMOS_EXE     = %s" % exe)
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
        kill_pids(daemon_pids(boot))

    print("\n=== %s ===" % ("ISOLATION VERIFIEE" if ok else "L'ISOLATION NE TIENT PAS"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
