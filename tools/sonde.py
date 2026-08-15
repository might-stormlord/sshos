#!/usr/bin/env python3
"""LA BOITE A OUTILS DES SONDES BOUT-EN-BOUT de ssh_os 2.0.

Aucun test unitaire ne monte le chemin complet : client -> demon -> fenetre
-> PTY -> shell -> parseur -> grille -> diff -> client. Une sonde le monte
en entier, avec de VRAIS programmes dedans. QUATRE des dix defauts « ne
sans appelant » du projet n'ont ete vus que comme ca -- voir §9 bis du
dossier de reprise.

Usage : copier ce fichier, ou l'importer, et ecrire son scenario.
    python3 -u tools/sonde.py            # sonde de fumee
    python3 -u ma_sonde.py > sortie.log  # JAMAIS derriere un tube

LES TROIS REGLES DURES, chacune payee comptant :

1. UN DEMON NEUF PAR SCENARIO. Le demon SURVIT au client -- c'est la
   promesse du projet -- donc un essai precedent laisse derriere lui un
   bureau dans un etat quelconque, et la sonde suivante mesure cet etat-la.
   `spawn()` appelle `kill_daemon()` pour cette raison. Une demi-heure
   perdue a diagnostiquer un « defaut » qui n'etait que le bureau d'avant.

2. UNE TRAME N'A PAS DE RETOURS A LA LIGNE. Elle positionne le curseur par
   `\033[l;cH` puis ecrit. Chercher un motif dans le flux brut donne des
   faux negatifs, et un `split("\n")` rend une seule ligne geante contenant
   tout l'ecran. Rejouer avec `screen()` d'abord, toujours.

3. JAMAIS DERRIERE UN TUBE. `| tail` bufferise : un travail tue ne laisse
   aucune trace. `python3 -u` vers un fichier.

Et une quatrieme, apprise en chemin : NE PAS FORCER DE REPEINT pour
observer. `\x01r` est une frappe, et une frappe referme le transitoire.
Voir `suivre()`.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time

BIN = "/home/storm/dev/ssh_os_2.0/build-release/sshos"


def daemon_pid():
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            a = open("/proc/%s/cmdline" % e, "rb").read().split(b"\0")
            u = [l for l in open("/proc/%s/status" % e) if l.startswith("Uid:")][0].split()[1]
        except OSError:
            continue
        if b"--daemon" in a and int(u) == os.getuid():
            return int(e)
    return None


def kill_daemon():
    p = daemon_pid()
    if p is None:
        return
    try:
        os.kill(p, signal.SIGTERM)
    except OSError:
        return
    for _ in range(50):
        if daemon_pid() is None:
            return
        time.sleep(0.1)


def spawn():
    kill_daemon()
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.execv(BIN, [BIN])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    return pid, fd


def read_for(fd, seconds):
    buf = b""
    first = None
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
        if first is None:
            first = time.time() - t0
        buf += c
    return buf.decode("utf-8", "replace"), first


def jiffies(pid):
    try:
        with open("/proc/%d/stat" % pid) as fh:
            f = fh.read().rsplit(") ", 1)[1].split()
        return int(f[11]) + int(f[12])
    except (OSError, IndexError, TypeError):
        return -1


def close(pid, fd):
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    os.close(fd)
    kill_daemon()



def screen(text, cols=80, rows=24):
    """Rejoue une trame en grille. UNE TRAME N'A PAS DE RETOURS A LA LIGNE :
    elle positionne le curseur par \033[l;cH et ecrit. Chercher un motif dans
    le flux brut donne des faux negatifs, et un `split("\n")` rend une seule
    ligne geante contenant tout l'ecran."""
    grid = [[" "] * cols for _ in range(rows)]
    y = x = 0
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "\033" and i + 1 < n and text[i + 1] == "[":
            j = i + 2
            while j < n and text[j] not in "@ABCDEFGHJKSTfhlmnrst":
                j += 1
            if j < n:
                params, final = text[i + 2:j], text[j]
                if final in "Hf":
                    bits = params.split(";")
                    y = (int(bits[0]) - 1) if bits and bits[0].isdigit() else 0
                    x = (int(bits[1]) - 1) if len(bits) > 1 and bits[1].isdigit() else 0
                i = j + 1
                continue
            i = n
            continue
        if c == "\r":
            x = 0
        elif c == "\n":
            y += 1
        elif c == "\033":
            i += 2
            continue
        elif 0 <= y < rows and 0 <= x < cols:
            grid[y][x] = c
            x += 1
        i += 1
    return ["".join(r) for r in grid]


def title_row(text):
    """Ligne et colonne du titre « Bloc » de la FENETRE (pas de la barre)."""
    rows = screen(text)
    for i, r in enumerate(rows[:-1]):   # la derniere ligne est le panneau
        at = r.find("Bloc")
        if at >= 0:
            return i, at
    return -1, -1


def repaint(fd):
    os.write(fd, b"\x01r")
    t, _ = read_for(fd, 1.0)
    return t



def open_terminal(fd):
    """Ctrl+A, Espace, on filtre sur « term », Entree."""
    os.write(fd, b"\x01 ")
    read_for(fd, 0.6)
    os.write(fd, b"term")
    read_for(fd, 0.6)
    os.write(fd, b"\r")
    read_for(fd, 1.5)


def grid_has(fd, needle, seconds=3.0):
    """Cherche dans la GRILLE rejouee, pas dans le flux brut : une trame
    positionne le curseur, elle n'a pas de retours a la ligne."""
    t0 = time.time()
    seen = ""
    while time.time() - t0 < seconds:
        os.write(fd, b"\x01r")            # repeint complet
        txt, _ = read_for(fd, 0.7)
        seen = "\n".join(screen(txt))
        if needle in seen:
            return True, seen
    return False, seen



# ---------------------------------------------------------------------------
# La boite a outils d'une sonde. Les trois regles dures sont dans le
# docstring en tete de fichier et dans docs/REPRISE.md, §9 ter.
# ---------------------------------------------------------------------------

FLUX = [""]


def suivre(fd, secondes):
    """Accumule TOUT ce que le demon envoie depuis le debut, et rejoue le
    flux entier dans une grille.

    NE PAS forcer un repeint pour regarder : `\\x01r` est une FRAPPE, et une
    frappe referme les choses transitoires -- proposition d'ancrage, menu,
    saisie en cours. Une sonde qui repeint pour observer mesure sa propre
    interference. Defaut rencontre pour de vrai le 14 aout 2026.
    """
    t, _ = read_for(fd, secondes)
    FLUX[0] += t
    return screen(FLUX[0])


def clic(fd, x, y, bouton=0):
    """Un clic SGR 1006, tel que le VRAI terminal le rapporterait.
    Colonnes et lignes 1-indexees ; la grille rejouee part de zero.
    `bouton` : 0 gauche, 2 droit, 16 = Ctrl+gauche, 32 = mouvement.
    """
    os.write(fd, ("\033[<%d;%d;%dM" % (bouton, x + 1, y + 1)).encode())
    time.sleep(0.12)
    os.write(fd, ("\033[<%d;%d;%dm" % (bouton, x + 1, y + 1)).encode())


def glisser(fd, x0, y0, x1, y1):
    """Un glisser-deposer complet. Le bureau ne livre les mouvements a une
    application que si l'appui a eu lieu dans SON corps (Session::mouse_grab_).
    """
    os.write(fd, ("\033[<0;%d;%dM" % (x0 + 1, y0 + 1)).encode())
    time.sleep(0.15)
    for i in range(1, 4):
        mx = x0 + (x1 - x0) * i // 3
        my = y0 + (y1 - y0) * i // 3
        os.write(fd, ("\033[<32;%d;%dM" % (mx + 1, my + 1)).encode())
        time.sleep(0.1)
    os.write(fd, ("\033[<0;%d;%dm" % (x1 + 1, y1 + 1)).encode())


def trouve(lignes, mot, depuis=0):
    """La position d'un motif dans la grille rejouee. Rend (-1, -1) sinon."""
    for y in range(depuis, len(lignes)):
        x = lignes[y].find(mot)
        if x >= 0:
            return x, y
    return -1, -1


def montre(lignes, quoi):
    print("  --- %s" % quoi)
    for r in lignes:
        if r.strip():
            print("   |" + r.rstrip())


def ouvrir(fd, mot):
    """Ctrl+A, Espace, on filtre sur `mot`, Entree."""
    os.write(fd, b"\x01 ")
    suivre(fd, 0.6)
    os.write(fd, mot.encode())
    suivre(fd, 0.6)
    os.write(fd, b"\r")
    suivre(fd, 1.5)


if __name__ == "__main__":
    # Sonde de fumee : le bureau se leve, une application s'ouvre, et le
    # demon ne consomme rien au repos.
    print("=== sonde de fumee ===")
    pid, fd = spawn()
    suivre(fd, 1.5)
    ouvrir(fd, "fich")
    lignes = suivre(fd, 1.0)
    montre(lignes, "le gestionnaire de fichiers")
    d = daemon_pid()
    avant = jiffies(d)
    time.sleep(2.0)
    print("  jiffies du demon : %d / 2 s" % (jiffies(d) - avant))
    close(pid, fd)
