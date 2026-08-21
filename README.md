<h1 align="center">termos</h1>

<p align="center">
  <strong>Un bureau en mode texte qui survit à la déconnexion.</strong><br>
  <em>A text-mode desktop that survives disconnection.</em>
</p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="dépendances externes : 0" src="https://img.shields.io/badge/d%C3%A9pendances%20externes-0-2ea44f">
  <img alt="1318 tests" src="https://img.shields.io/badge/tests-1318%20%C2%B7%200%20%C3%A9chec-2ea44f">
  <img alt="licence AGPL-3.0" src="https://img.shields.io/badge/licence-AGPL--3.0-blue">
  <img alt="Linux / glibc" src="https://img.shields.io/badge/cible-Linux%20%C2%B7%20glibc-lightgrey">
</p>

```
┌─ Terminal (build) ────────────┐ ┌─ Fichiers ───────────────────┐
│ $ make -j8                    │ │ ~/dev/termos                 │
│ [100%] Built target termos    │ │  ..                          │
│ $ _                           │ │  src/            <REP>       │
│                               │ │  tests/          <REP>       │
└───────────────────────────────┘ └──────────────────────────────┘
 [TERMOS]  Terminal  Fichiers  Editeur              CPU 12%  14:32
```

<p align="center"><strong>Français</strong> · <a href="#english">English</a></p>

---

Fermer la fenêtre du terminal ne tue pas la session : on se rebranche et on retrouve son
écran intact — comme `tmux`, mais avec des fenêtres, un menu, une barre des tâches et
des applications.

Le geste qui montre l'intérêt du projet : lancer un Terminal, y poser
`MARQUE=persiste`, **fermer la fenêtre du terminal**, relancer `./build-release/termos` —
`echo $MARQUE` répond `persiste`.

## Ce qui le caractérise

- **C++20, zéro dépendance externe.** Pas de gtest, pas de ncurses, pas de fmt —
  uniquement la bibliothèque standard et l'API POSIX/Linux. Le harnais de test
  (`tests/harness.hpp`) est maison, comme le reste.
- **Un seul thread, un seul `epoll`, aucun mutex.** La contrainte est structurante :
  toute opération longue (copier une arborescence, supprimer récursivement) avance
  **par tranches**, une par réveil, pour ne jamais figer le bureau.
- **Client mince, démon épais.** Le démon détient tout l'état, compose une grille de
  cellules, calcule un diff et n'envoie que les séquences ANSI nécessaires. Le client
  met son terminal en mode brut et recopie ce qu'il reçoit.
- **La souris d'abord.** Toute fonction est atteignable sans raccourci clavier.
- **Plus de tests que de code** — 25 159 lignes dans `tests/` contre 16 736 dans
  `src/`, ce qui rend les campagnes de mutation possibles.

**Cible :** glibc/Linux (`epoll`, `signalfd`, `timerfd`, sockets UNIX abstraits).

## Compiler et lancer

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"

./build-release/termos            # lance le bureau (démarre le démon si besoin)
./build-release/termos --status   # « demon actif (pid N) » ou « aucun demon »
./build-release/termos --kill     # arrête le démon
./build-release/termos --daemon   # démarre le démon sans s'attacher
```

`Ctrl+A` puis `Espace` ouvre le menu. `Ctrl+Q` **détache** la session sans la détruire ;
la détruire pour de bon se demande par l'entrée « Fermer la session », qui pose une
confirmation.

## Tests

```bash
./build-release/termos_tests          # 1318 cas, ~36 s
./build-release/termos_tests files_   # filtre par sous-chaîne du nom
```

Attendu : `1318 cas, 0 en echec, 0 assertions echouees`, avec 0 avertissement
(`-Wall -Wextra -Wpedantic -Werror`). Le type `Debug` ajoute ASan et UBSan et fait
tourner la même suite en ~67 s :

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j"$(nproc)" && ./build-debug/termos_tests
```

## Ce que contient le bureau

| | |
|---|---|
| **Terminal** | À onglets, chacun avec son PTY, sa grille et son historique. Fait tourner `vim`, `htop`, `less` et un `tmux` imbriqué. Émulation VT écrite pour l'occasion. |
| **Fichiers** | Façon Dolphin : vue scindée, sélection multiple, colonnes triables, glisser-déposer, menu au bouton droit, copie et suppression récursive sans jamais bloquer le démon. |
| **Éditeur** | Ouvrir, modifier, enregistrer (`Ctrl+S`), rechercher (`Ctrl+F`), quitter avec confirmation si le tampon est sale (`Ctrl+X` — pas `Ctrl+Q`, que le bureau intercepte pour détacher). S'ouvre depuis Fichiers en validant un fichier. |
| **Moniteur** | Widget du fond d'écran : CPU, mémoire, réseau, charge, processus. |

## Pour aller plus loin

- **[`docs/REPRISE.md`](docs/REPRISE.md)** — le dossier de reprise. Écrit pour quelqu'un
  qui ne connaît rien au projet : l'architecture, les contraintes non négociables, les
  pièges d'environnement, la méthode de travail et ce qui reste à faire. C'est le
  document à lire avant de toucher au code.
- **`docs/superpowers/specs/`** — la conception d'origine.
- **`docs/superpowers/plans/`** — les plans des sept jalons, tâche par tâche, avec le
  nombre de tests et de mutations de chacune.
- **`tools/`** — la sonde bout-en-bout (`sonde.py`) et le harnais de campagne de
  mutation (`mutation.py`).

> La documentation du projet est **en français**. Le code, lui, se lit sans.

## Licence

**GNU AGPL-3.0** — voir [`LICENSE`](LICENSE). Forkez, modifiez, redistribuez ; mais toute
version dérivée reste sous la même licence et doit publier ses sources, y compris si
elle n'est **exploitée qu'en service** sans jamais être distribuée. C'est le cas d'usage
qui compte ici : `termos` est un démon que l'on joint à distance.

Copyright © 2026 [might-stormlord](https://github.com/might-stormlord).

---

<h2 id="english">English</h2>

Closing the terminal window does not kill the session: reconnect and your screen is
exactly where you left it — like `tmux`, but with windows, a menu, a taskbar and
applications.

The gesture that makes the point: open a Terminal, set `MARK=persists`, **close the
terminal window**, run `./build-release/termos` again — `echo $MARK` answers `persists`.

### What makes it what it is

- **C++20, zero external dependencies.** No gtest, no ncurses, no fmt — the standard
  library and the POSIX/Linux API, nothing else. The test harness
  (`tests/harness.hpp`) is home-grown too.
- **One thread, one `epoll`, no mutex.** The constraint shapes the design: every long
  operation (copying a tree, deleting recursively) advances **in slices**, one per
  wake-up, so the desktop never freezes.
- **Thin client, thick daemon.** The daemon holds all state, composes a cell grid,
  diffs it and sends only the ANSI sequences that changed. The client puts its terminal
  in raw mode and echoes back what it receives.
- **Mouse first.** Every function is reachable without knowing a keyboard shortcut.
- **More test code than code** — 25,159 lines in `tests/` against 16,736 in `src/`,
  which is what makes mutation campaigns viable.

**Target:** glibc/Linux (`epoll`, `signalfd`, `timerfd`, abstract UNIX sockets).

### Build and run

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"

./build-release/termos            # start the desktop (spawns the daemon if needed)
./build-release/termos --status   # daemon running (pid N), or none
./build-release/termos --kill     # stop the daemon
./build-release/termos --daemon   # start the daemon without attaching
```

`Ctrl+A` then `Space` opens the menu. `Ctrl+Q` **detaches** without destroying the
session; destroying it for good goes through the "Close session" entry, which asks for
confirmation.

### Tests

```bash
./build-release/termos_tests          # 1318 cases, ~36 s
./build-release/termos_tests files_   # filter by substring of the test name
```

Expected: `1318 cas, 0 en echec, 0 assertions echouees`, with zero compiler warnings
(`-Wall -Wextra -Wpedantic -Werror`). The `Debug` type adds ASan and UBSan and runs the
same suite in ~67 s.

### What the desktop ships with

| | |
|---|---|
| **Terminal** | Tabbed, each tab with its own PTY, grid and scrollback. Runs `vim`, `htop`, `less` and a nested `tmux`. VT emulation written for the occasion. |
| **Files** | Dolphin-style: split view, multi-selection, sortable columns, drag and drop, right-click menu, recursive copy and delete that never block the daemon. |
| **Editor** | Open, edit, save (`Ctrl+S`), search (`Ctrl+F`), quit with confirmation on a dirty buffer (`Ctrl+X` — not `Ctrl+Q`, which the desktop intercepts to detach). Opens from Files by activating a file. |
| **Monitor** | Wallpaper widget: CPU, memory, network, load, processes. |

### Documentation

The project's documentation is **in French**, starting with
[`docs/REPRISE.md`](docs/REPRISE.md) — a hand-over dossier written for someone who knows
nothing about the project: architecture, non-negotiable constraints, environment
pitfalls, working method, and what is left to do. Read it before touching the code.

### License

**GNU AGPL-3.0** — see [`LICENSE`](LICENSE). Fork it, modify it, redistribute it; but any
derivative stays under the same license and must publish its sources, **including when
it is only ever run as a service** and never distributed. That is the case that matters
here: `termos` is a daemon you reach over the network.

Copyright © 2026 [might-stormlord](https://github.com/might-stormlord).
