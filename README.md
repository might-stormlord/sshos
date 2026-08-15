# ssh_os 2.0

Un environnement de bureau **en mode texte**, utilisable à travers SSH, qui **survit à
la déconnexion**. Fermer la fenêtre du terminal ne tue pas la session : on se rebranche
et on retrouve son écran intact — comme `tmux`, mais avec des fenêtres, un menu, une
barre des tâches et des applications.

```
┌─ Terminal (build) ────────────┐ ┌─ Fichiers ───────────────────┐
│ $ make -j8                    │ │ ~/dev/ssh_os_2.0             │
│ [100%] Built target sshos     │ │  ..                          │
│ $ _                           │ │  src/            <REP>       │
│                               │ │  tests/          <REP>       │
└───────────────────────────────┘ └──────────────────────────────┘
 [SSH OS]  Terminal  Fichiers  Editeur              CPU 12%  14:32
```

## Ce qui le caractérise

- **C++20, zéro dépendance externe.** Pas de gtest, pas de ncurses, pas de fmt —
  uniquement la bibliothèque standard et l'API POSIX/Linux.
- **Un seul thread, un seul `epoll`, aucun mutex.** La contrainte est structurante :
  toute opération longue (copier une arborescence, supprimer récursivement) avance
  **par tranches**, une par réveil, pour ne jamais figer le bureau.
- **Client mince, démon épais.** Le démon détient tout l'état, compose une grille de
  cellules, calcule un diff et n'envoie que les séquences ANSI nécessaires. Le client
  met son terminal en mode brut et recopie ce qu'il reçoit.
- **La souris d'abord.** Toute fonction est atteignable sans raccourci clavier.
- **41 500 lignes, dont 24 800 de tests** — plus de tests que de code, ce qui rend les
  campagnes de mutation possibles.

**Cible :** glibc/Linux (`epoll`, `signalfd`, `timerfd`, sockets UNIX abstraits).

## Compiler et lancer

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"

./build-release/sshos            # lance le bureau (démarre le démon si besoin)
./build-release/sshos --status   # « demon actif (pid N) » ou « aucun demon »
./build-release/sshos --kill     # arrête le démon
./build-release/sshos --daemon   # démarre le démon sans s'attacher
```

`Ctrl+A` puis `Espace` ouvre le menu. `Ctrl+Q` **détache** la session sans la détruire ;
la détruire pour de bon se demande par l'entrée « Fermer la session », qui pose une
confirmation.

Le geste qui montre l'intérêt du projet : lancer un Terminal, y poser `MARQUE=persiste`,
**fermer la fenêtre du terminal**, relancer `./build-release/sshos` — `echo $MARQUE`
répond `persiste`.

## Tests

```bash
./build-release/sshos_tests          # 1146 cas, ~20 s
./build-release/sshos_tests files_   # filtre par sous-chaîne du nom
```

Attendu : `1146 cas, 0 en echec, 0 assertions echouees`, avec 0 avertissement
(`-Wall -Wextra -Wpedantic -Werror`). Le type `Debug` ajoute ASan et UBSan et fait
tourner la même suite en ~47 s :

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j"$(nproc)" && ./build-debug/sshos_tests
```

Le harnais de test (`tests/harness.hpp`) est maison, comme le reste.

## Ce que contient le bureau

| | |
|---|---|
| **Terminal** | À onglets, chacun avec son PTY, sa grille et son historique. Fait tourner `vim`, `htop`, `less` et un `tmux` imbriqué. Émulation VT écrite pour l'occasion. |
| **Fichiers** | Façon Dolphin : vue scindée, sélection multiple, colonnes triables, glisser-déposer, menu au bouton droit, copie et suppression récursive sans jamais bloquer le démon. |
| **Éditeur** | Ouvrir, modifier, enregistrer (`Ctrl+S`), rechercher (`Ctrl+F`), quitter avec confirmation si le tampon est sale (`Ctrl+X` — pas `Ctrl+Q`, que le bureau intercepte pour détacher). S'ouvre depuis Fichiers en validant un fichier. |
| **Moniteur** | Widget du fond d'écran : CPU, mémoire, réseau, charge, processus. |

## Pour aller plus loin

- **[`docs/REPRISE.md`](docs/REPRISE.md)** — le dossier de reprise. Écrit pour
  quelqu'un qui ne connaît rien au projet : l'architecture, les contraintes non
  négociables, les pièges d'environnement, la méthode de travail et ce qui reste à
  faire. C'est le document à lire avant de toucher au code.
- **`docs/superpowers/specs/`** — la conception d'origine.
- **`docs/superpowers/plans/`** — les plans des sept jalons, tâche par tâche, avec le
  nombre de tests et de mutations de chacune.
- **`tools/`** — la sonde bout-en-bout (`sonde.py`) et le harnais de campagne de
  mutation (`mutation.py`).
