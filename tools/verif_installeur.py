#!/usr/bin/env python3
"""Pilote l'assistant de tools/install.sh a travers un vrai PTY.

CE QUE CA ATTRAPE, ET QU'AUCUN AUTRE TEST NE PEUT VOIR : le cadre se
desaligne des qu'un glyphe multi-octets entre dans le calcul de
remplissage. dash compte des OCTETS avec ${#s} ; « ╭ » en fait trois. Le
script est ecrit pour que le texte reste ASCII et que les glyphes soient
poses a cote, mais c'est une discipline, pas une garantie du langage --
donc on mesure.

On verifie aussi que les fleches deplacent la selection, que Entree avance,
que la barre de progression suit, et que le clic selectionne ET valide.

Aucune commande detachee. L'installeur est interrompu apres l'assistant : on
ne veut pas compiler pour verifier un dessin.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import shutil
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(ROOT, "tools", "install.sh")

COLS, ROWS = 100, 32

ANSI = re.compile(r"\033\[[0-9;?]*[a-zA-Z]")
BOX = "╭╰│╮╯"

ok_all = True


def check(label, cond, extra=""):
    global ok_all
    print("   %-56s %s%s" % (label, "OK" if cond else "ECHEC",
                             ("  " + extra) if extra and not cond else ""),
          flush=True)
    if not cond:
        ok_all = False
    return cond


def strip_ansi(s):
    return ANSI.sub("", s)


def last_frame(buf):
    """Le dernier ecran complet : tout ce qui suit le dernier effacement."""
    text = buf.decode("utf-8", "replace")
    i = text.rfind("\033[H\033[2J")
    if i >= 0:
        text = text[i + len("\033[H\033[2J"):]
    return strip_ansi(text)


def box_lines(frame):
    return [l for l in frame.split("\n") if l and l[0] in BOX]


class Pty:
    def __init__(self, home):
        env = dict(os.environ)
        env["HOME"] = home
        env["TERM"] = "xterm-256color"
        env["LANG"] = "en_US.UTF-8"
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.clear()
            os.environ.update(env)
            os.execv("/bin/sh", ["/bin/sh", INSTALL])
            os._exit(1)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, 0, 0))

    def drain(self, seconds=0.7):
        buf = b""
        import time
        t0 = time.time()
        while time.time() - t0 < seconds:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                c = os.read(self.fd, 65536)
            except OSError:
                break
            if not c:
                break
            buf += c
        return buf

    def send(self, data):
        os.write(self.fd, data)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass


UP = b"\033[A"
DOWN = b"\033[B"
LEFT = b"\033[D"
ENTER = b"\r"
CTRLC = b"\003"


def click(y, x=8):
    """Un appui de bouton gauche en SGR, a la ligne y (1-based)."""
    return ("\033[<0;%d;%dM" % (x, y)).encode()


def main():
    home = tempfile.mkdtemp(prefix="sshos-ui-", dir="/var/tmp")
    p = Pty(home)
    try:
        print("== 1. L'assistant s'ouvre et le cadre est droit")
        buf = p.drain(1.5)
        frame = last_frame(buf)
        lines = box_lines(frame)
        check("un cadre est dessine", len(lines) >= 8, "%d lignes" % len(lines))
        widths = sorted(set(len(l) for l in lines))
        check("toutes les lignes du cadre ont la meme largeur",
              len(widths) == 1, "largeurs vues : %s" % widths)
        check("le titre est la", "Installation de ssh_os" in frame)
        check("la question 1 est la", "Ou installer ?" in frame)
        check("les trois choix sont la",
              ".local" in frame and "/usr/local" in frame and "autre" in frame)
        check("la barre de progression est la", "0/4" in frame)
        check("l'aide mentionne le clic", "clic" in frame)

        print("\n== 2. Les fleches deplacent la selection")
        p.send(DOWN)
        f2 = last_frame(p.drain())
        sel2 = [l for l in f2.split("\n") if "▸" in l and "/usr/local" in l]
        check("Bas selectionne /usr/local", len(sel2) == 1)
        p.send(UP)
        f3 = last_frame(p.drain())
        sel3 = [l for l in f3.split("\n") if "▸" in l and ".local" in l]
        check("Haut revient sur ~/.local", len(sel3) >= 1)

        print("\n== 3. Entree avance, et la reponse reste affichee")
        p.send(ENTER)
        f4 = last_frame(p.drain())
        check("l'etape 1 est cochee", "✓" in f4 and "Ou installer" in f4)
        check("la progression a avance", "1/4" in f4)
        widths = sorted(set(len(l) for l in box_lines(f4)))
        check("le cadre reste droit avec une ligne cochee", len(widths) == 1,
              "largeurs : %s" % widths)

        print("\n== 4. Gauche revient en arriere")
        p.send(LEFT)
        f5 = last_frame(p.drain())
        check("on est revenu a la question 1",
              "Ou installer ?" in f5 and "0/4" in f5)

        print("\n== 5. Le clic selectionne ET valide")
        # On retrouve la ligne d'ecran du second choix et on clique dessus.
        rows = f5.split("\n")
        target = None
        for i, l in enumerate(rows):
            if "/usr/local" in l:
                target = i + 1  # les lignes d'ecran sont 1-based
                break
        if target is None:
            check("la ligne /usr/local est reperable", False)
        else:
            p.send(click(target))
            f6 = last_frame(p.drain())
            check("le clic a valide et avance", "2/4" in f6 or "1/4" in f6,
                  "ecran :\n" + f6[:400])
            check("c'est bien /usr/local qui est retenu", "/usr/local" in f6)

        print("\n== Dernier ecran rendu")
        for l in last_frame(p.drain(0.3)).split("\n"):
            if l.strip():
                print("   " + l)

        p.send(CTRLC)
        p.drain(0.5)
    finally:
        p.close()
        shutil.rmtree(home, ignore_errors=True)

    print("\n=== %s ===" % ("INTERFACE AU VERT" if ok_all
                            else "L'INTERFACE A UN DEFAUT"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
