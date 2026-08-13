# ssh_os 2.0 — Jalon 3 : le Terminal

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mettre un vrai shell dans une fenêtre. À la fin de ce jalon, `vim`, `htop` et `less` fonctionnent, un `tmux` imbriqué tient, une compilation survit à un redimensionnement, et la déconnexion SSH ne tue rien. C'est le jalon où le projet cesse d'être une démonstration.

**Architecture:** Trois couches qui ne se connaissent que par en dessous.

`src/pty/` ouvre un pseudo-terminal et lance un processus dedans. Il ne sait rien du rendu ni de l'`epoll` : il rend un descripteur et un `pid`, et c'est l'application qui les confie à son `Host`.

`src/vt/parser.*` est une machine à états pure : elle reçoit des octets et appelle un `ParserSink`. Elle n'a **aucune** notion de grille, de couleur ni de curseur — ce qui la rend testable par un mouchard qui enregistre les appels, et fuzzable sans rien monter autour.

`src/vt/screen.*` est la grille : curseur, attributs, régions, scrollback. Elle implémente `ParserSink` et ne lit jamais un octet.

`src/apps/terminal.*` est le liant, et c'est tout ce que le bureau voit : une `App` de plus au catalogue.

**La propriété centrale : aucun octet invité n'est jamais relayé tel quel.** Le démon interprète tout, met à jour une grille, et re-synthétise sa propre sortie. Un `vim` dans une fenêtre ne peut pas plus perturber le bureau qu'un programme ne peut perturber le compositeur d'un vrai OS. Le `DECSET 1049` d'un invité bascule le tampon **à l'intérieur de sa fenêtre** ; l'écran alterné du client, lui, sert une seule fois, pour le bureau entier.

**Tech Stack:** C++20, CMake, glibc/Linux. Aucune bibliothèque externe — pas de `libvterm`, pas de `terminfo`. Deux appels système nouveaux par rapport au jalon 2 : `posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r` et `TIOCSCTTY`/`TIOCSWINSZ`. Le `signalfd` existe depuis le jalon 1 ; il gagne `SIGCHLD`.

**Spec de référence :** `docs/superpowers/specs/2026-08-10-ssh-os-design.md` §9.1 (le terminal, dans le détail), §7.5 (collage entre crochets et rapport de focus), §8 (contrat applicatif), §13.6 (liste de vérification manuelle). Les plans des jalons 1 et 2 décrivent le socle.

**Point de départ :** HEAD `a10822f`, **377 cas, 0 en echec**, en `Release` comme en `Debug`.

## Global Constraints

Ces contraintes valent pour **toutes** les tâches et ne sont pas répétées dans chacune.

- **C++20**, `set(CMAKE_CXX_EXTENSIONS OFF)`. Options : `-Wall -Wextra -Wpedantic -Werror`. `Debug` ajoute `-fsanitize=address,undefined -g -O0` ; `Release` compile en `-O2`.
- **`\e` est une extension GCC et ne compile pas** sous `-Wpedantic -Werror`. Écrire `\033` dans tous les littéraux, tests compris. Une seule occurrence casse la compilation du projet entier.
- **Zéro dépendance externe.** Uniquement la bibliothèque standard et l'API POSIX/Linux.
- **Ne pas modifier `CMakeLists.txt`.** Il globe `src/*.cpp` en récursif et `tests/test_*.cpp` : tout fichier créé aux bons endroits est pris automatiquement. `src/pty/` et `src/vt/` ne demandent aucune déclaration.
- **Un thread, un `epoll`, aucun mutex.** Aucune commande détachée (`nohup`, `&`, `disown`) nulle part, y compris dans les tests.
- Tous les descripteurs sont ouverts `CLOEXEC`. Toutes les écritures sont non bloquantes.
- **`unwatch()` précède toujours `close()`.** L'ordre inverse laisse une entrée `epoll` sur un numéro que le noyau peut réattribuer à la milliseconde suivante.
- Le français est la langue des messages destinés à l'utilisateur **et des commentaires** ; le code et les identifiants sont en anglais. Les commentaires portent le *pourquoi*, pas le *quoi*.
- Les messages de commit sont **sans accents** ; les caractères non-ASCII du code s'écrivent en **littéral** (`U'█'`, `U'┌'`), jamais échappés.
- Valeurs figées du projet, à ne jamais recalculer : rendu **33 ms**, contre-pression **1 Mo**, plafond **64 fenêtres**, ambiguïté `ESC` **50 ms**, leader **`Ctrl+A`**, plancher d'une fenêtre **16×5**, zone de travail minimale **40×12**, série d'accords **1,5 s**.
- **Jamais de processus identifié par son nom.** Toujours un `pid` obtenu par `fork()`.

## Commandes de référence

```bash
cmake --build build-release -j"$(nproc)"     # compiler en Release
./build-release/sshos_tests                  # toute la suite
./build-release/sshos_tests vt               # seulement les cas dont le nom contient "vt"

cmake --build build-dbg -j"$(nproc)"         # ASan + UBSan
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build-dbg/sshos_tests
```

**Ne jamais filtrer la sortie sur `grep -i FAIL`** : plusieurs cas portent `failed` dans leur nom. Lire la **ligne de bilan** finale (`N cas, M en echec`).

**Toute sonde bout-en-bout tue le démon d'abord** (`sshos --kill`, ou `SIGTERM` au `pid` trouvé en scannant `/proc/*/cmdline`). Le démon survit au client par conception : sans ça, la sonde mesure le bureau de l'essai précédent.

**Une trame n'a pas de retours à la ligne** : elle positionne le curseur par `\033[l;cH`. Une sonde qui lit un écran rejoue le flux dans une grille (helper `screen()` de `scratchpad/chain_probe.py`) au lieu de chercher un motif dans le flux brut.

---

## Structure des fichiers

| Fichier | Responsabilité |
|---|---|
| `src/pty/pty.hpp/.cpp` | Ouvrir un pseudo-terminal, lancer un processus dedans, l'assainir avant `exec`, poser sa taille |
| `src/pty/env.hpp/.cpp` | Construire l'environnement de l'enfant : `getpwuid`, delta du handshake, variables fixées par nous |
| `src/vt/parser.hpp/.cpp` | Machine à états DEC + décodeur UTF-8. Ne connaît ni grille ni couleur |
| `src/vt/sink.hpp` | Le contrat que la machine appelle : `print`, `execute`, `csi`, `esc`, `osc`, `dcs` |
| `src/vt/screen.hpp/.cpp` | La grille : curseur, retour différé, régions, effacements, éditions |
| `src/vt/attrs.hpp/.cpp` | `SGR` : 16 / 256 / `38;2;r;g;b`, attributs, remise à zéro |
| `src/vt/modes.hpp/.cpp` | `DECSET`/`DECRST` : ce que l'invité a demandé, lisible par le liant |
| `src/vt/scrollback.hpp/.cpp` | Tampon circulaire de lignes rognées, décalage de consultation |
| `src/vt/reply.hpp/.cpp` | Réponses aux requêtes : `DA`, `CPR`, `DECRQM` |
| `src/vt/charset.hpp/.cpp` | `SCS ESC ( 0` — semi-graphiques DEC |
| `src/input/encode.hpp/.cpp` | `KeyEvent` → octets pour l'invité (`DECCKM`, modificateurs) |
| `src/apps/terminal.hpp/.cpp` | L'application : liant PTY ↔ parseur ↔ écran ↔ `View` |
| `src/app/app.hpp` | *(modifié)* `App::on_child_exit`, `Host::watch_child` |
| `src/daemon/host.cpp` | *(modifié)* achemine `watch_child` |
| `src/daemon/session.cpp` | *(modifié)* route `on_child_exit` vers la bonne fenêtre |
| `src/daemon/daemon.cpp` | *(modifié)* `SIGCHLD` sur le `signalfd`, double boucle de récolte |
| `src/app/catalog.cpp` | *(modifié)* le Terminal entre au catalogue, en tête |

---

## Tâche 1 — Le PTY et l'assainissement de l'enfant

**Fichiers :** `src/pty/pty.hpp/.cpp`, `src/pty/env.hpp/.cpp`, `tests/test_pty.cpp`, `tests/test_env.cpp`

`posix_openpt(O_RDWR|O_NOCTTY)` → `grantpt` → `unlockpt` → `ptsname_r`, puis `fork`. Le maître est ouvert **`CLOEXEC` et non bloquant**.

Dans l'enfant : `setsid()`, `ioctl(esclave, TIOCSCTTY, 0)`, `dup2` de l'esclave sur 0/1/2, `execve`.

**Trois héritages traversent `execve` et cassent silencieusement les invités.** Aucun n'est visible dans le code de l'enfant ; tous s'annulent explicitement.

- Le **masque de signaux** survit. Le démon bloque `SIGCHLD` pour le recevoir par `signalfd` ; en hériter casse `make -j8`, qui n'apprend jamais que ses compilateurs sont morts. → `sigprocmask(SIG_SETMASK, &vide)`.
- Les **dispositions `SIG_IGN`** survivent aussi. Le démon met `SIGPIPE` à `SIG_IGN` ; un enfant qui en hérite ne s'arrête plus sur un tuyau fermé et `yes | head -1` tourne indéfiniment. → toutes les dispositions à `SIG_DFL`.
- Un **descripteur fuité** laisse le shell de la fenêtre A lire la sortie de la fenêtre B et empêche à jamais la libération du PTY. → tout est `CLOEXEC`.

L'environnement : le shell vient de `getpwuid()`, **pas de `$SHELL`** — l'environnement du démon est un fossile de la première session SSH. `Hello::env` porte déjà le delta (`SSH_AUTH_SOCK`, `SSH_CONNECTION`, `SSH_CLIENT`, `SSH_TTY`, `DISPLAY`, `XDG_SESSION_ID`), appliqué aux **nouveaux enfants seulement**. Fixés par nous : `TERM=xterm-256color` (il décrit **notre** émulateur), `COLORTERM=truecolor`, `SSHOS=1`. Ni `LINES` ni `COLUMNS` : la taille faisant autorité est celle du PTY.

**Tests :** un enfant qui rapporte son masque de signaux le trouve vide ; un enfant qui rapporte la disposition de `SIGPIPE` la trouve à `SIG_DFL` ; `yes | head -1` se termine ; un descripteur ouvert avant le `fork` n'est pas visible dans `/proc/<enfant>/fd` ; `TIOCSWINSZ` puis `stty size` dans l'enfant rend la bonne taille ; `$SHELL` menteur n'est pas suivi ; le delta arrive, l'environnement du démon non.

- [x] Tâche 1 livrée : tests, mutations, commit — `6b97b88`, 20 tests, 23 mutations

## Tâche 2 — La machine à états du parseur

**Fichiers :** `src/vt/sink.hpp`, `src/vt/parser.hpp/.cpp`, `tests/test_vt_parser.cpp`

Machine à états au sens strict, de forme DEC : `Ground`, `Escape`, `EscapeIntermediate`, `CsiEntry`, `CsiParam`, `CsiIntermediate`, `CsiIgnore`, `OscString`, `DcsPassthrough`, `SosPmApcString`, plus un décodeur UTF-8 dans `Ground`. Environ 300 lignes mécaniques ; tout le travail est dans les actions.

**L'état survit entre les appels.** Le parseur est nourri de morceaux arbitraires venant de `read()`, et une séquence coupée en deux doit fonctionner. C'est un test de première classe : toute séquence de la suite est rejouée **octet par octet** et doit produire exactement les mêmes appels qu'en un bloc.

Le `ParserSink` est un contrat pur (`print(char32_t)`, `execute(uint8_t)`, `csi(...)`, `esc(...)`, `osc(...)`, `dcs(...)`) : les tests l'implémentent par un mouchard qui enregistre, et n'ont besoin d'aucune grille.

**Tests :** table de séquences → appels attendus ; la même table rejouée octet par octet ; un UTF-8 tronqué en fin de morceau se recolle ; un octet invalide rend `U+FFFD` sans casser l'état ; `CsiIgnore` avale jusqu'au final ; les paramètres vides valent le défaut.

- [x] Tâche 2 livrée : tests, mutations, commit — `7a56afc`, 22 tests, 29 mutations

## Tâche 3 — L'écran : grille, curseur, retour à la ligne différé

**Fichiers :** `src/vt/screen.hpp/.cpp`, `tests/test_vt_screen.cpp`

La grille, le curseur, `CUP CUU CUD CUF CUB CHA VPA`, `IND RI NEL`, `LF CR BS HT`, les taquets de tabulation.

**Le retour à la ligne différé.** Écrire dans la dernière colonne ne provoque pas le retour : un drapeau « en attente » est posé et le retour n'a lieu qu'à l'arrivée du caractère suivant. Toute ligne de largeur pleine s'affiche de travers si on simplifie — et un `CUB` reçu entre les deux doit lever le drapeau sans avoir retourné.

Les caractères pleine chasse (`char_width == 2`) occupent deux cellules ; écrire un tel caractère en dernière colonne le pousse à la ligne suivante plutôt que de le couper.

**Tests :** écrire 80 caractères sur 80 colonnes ne descend pas d'une ligne ; le 81ᵉ descend ; `CUB` entre les deux annule l'attente ; un caractère double en colonne 80 descend entier ; les bornes de tous les déplacements ; `HT` s'arrête aux taquets.

- [x] Tâche 3 livrée : tests, mutations, commit — `4e010c7`, 39 tests, 41 mutations (1 équivalente)

## Tâche 4 — L'écran : effacements, éditions, régions

**Fichiers :** `src/vt/screen.cpp` *(étendu)*, `tests/test_vt_screen.cpp` *(étendu)*

`ED EL` (tous modes), `ICH DCH IL DL ECH`, `DECSTBM`, `DECSC` / `DECRC`, défilement à l'intérieur de la région.

Le piège : `IL`/`DL` hors région ne font rien, et une région rétrécie ne déplace pas le curseur hors d'elle. `DECSC` sauve curseur **et** attributs **et** jeu de caractères, pas seulement la position.

**Tests :** chaque mode de `ED`/`EL` sur une grille marquée ; `IL` au bas de la région ; `DL` en haut ; `DECSTBM` à une seule ligne ; `DECSC`/`DECRC` restitue les attributs ; un défilement pousse la ligne sortante au scrollback (tâche 7) et pas ailleurs.

- [x] Tâche 4 livrée : tests, mutations, commit — efda2a0, 43 tests, 51 mutations (4 équivalentes)

Deux parties de l'énoncé sont **reportées, faute de matière** : `DECSC` ne
sauve pour l'instant que la position et le retour différé, parce que les
attributs naissent à la tâche 5 et le jeu de caractères à la tâche 10 —
`SavedCursor` les attend, champ commenté à l'appui. *(Le report des
attributs est **soldé** à la tâche 5, commit `e599680` : `SavedCursor`
porte le style et `DECRC` le rend. Reste le jeu de caractères, tâche 10.)*
Et la ligne qui sort
par le haut n'est poussée nulle part puisque le scrollback est la tâche 7.
Le point d'accroche est prêt : `scroll_up()` est une façade d'une ligne
au-dessus de `scroll_slice_up(top_, bottom_, 1)`, gardée exprès parce
qu'elle est le seul chemin du défilement naturel — `IL` et `DL` appellent
la primitive directement, et n'ont pas à alimenter l'historique.

## Tâche 5 — SGR

**Fichiers :** `src/vt/attrs.hpp/.cpp`, `tests/test_vt_sgr.cpp`

`SGR` complet : 16, 256, `38;2;r;g;b`. Attributs gras / faible / italique / souligné / clignotant / inverse / caché / barré, et leurs remises à zéro individuelles.

Les pièges : `38;5;n` et `38;2;r;g;b` consomment des paramètres et un `SGR` malformé ne doit pas décaler la lecture des suivants ; `SGR` vide vaut `SGR 0` ; les séparateurs par deux-points (`38:2::r:g:b`) existent dans la nature.

**Tests :** table de séquences → attributs attendus ; un `38;5` tronqué n'emporte pas le reste ; `SGR 0` remet tout ; les couleurs vives 90-97 et 100-107.

- [x] Tâche 5 livrée : tests, mutations, commit — `d06ad0f` (SGR) et `e599680`
  (le stylo), 50 tests, 58 mutations (1 équivalente)

La tâche a débordé de ses fichiers, et c'était juste : `apply_sgr` seul
n'aurait eu **aucun appelant possible**, faute d'un endroit où ranger le
style. `ScreenCell` porte donc un `Style`, `Screen` un stylo courant, et
l'effacement peint le fond courant (`bce`, cf. le commit). Sans cela, les
tâches 7 (scrollback) et 8 (redimensionnement) se seraient construites sur
une grille sans couleur, et leurs tests auraient été à rouvrir.

Ce qui reste au liant (tâche 13) : appeler `apply_sgr` sur une copie de
`screen.pen()` et la reposer par `set_pen()`. L'écran ne connaît toujours
pas le parseur.

## Tâche 6 — Les modes `DECSET`/`DECRST`

**Fichiers :** `src/vt/modes.hpp/.cpp`, `src/vt/screen.cpp` *(étendu)*, `tests/test_vt_modes.cpp`

Modes 1 (`DECCKM`), 7 (retour automatique), 25 (curseur visible), 1000/1002/1003/1006 (souris), 1049 (écran alterné), 2004 (collage entre crochets).

`1049` bascule le tampon **et** sauve le curseur **et** efface le nouvel écran ; le retour restitue les trois. L'écran alterné n'alimente **jamais** le scrollback.

**Tests :** entrer et sortir de 1049 restitue l'écran principal au caractère près ; l'écran alterné ne pousse rien au scrollback ; 7 désactivé fait écraser la dernière colonne au lieu de retourner ; les modes souris sont lisibles par le liant.

- [x] Tâche 6 livrée : tests, mutations, commit — `a3281f4`, 21 tests, 31 mutations

Ce qui reste au liant (tâche 13) : sur un `CSI ? Pm h/l`, appeler
`apply_dec_private()` **puis** répercuter les deux modes qui ont un effet
mécanique — `screen.set_autowrap(modes.autowrap)` et
`screen.enter_alt_screen()` / `leave_alt_screen()` selon `modes.alt_screen`.
Le registre reste la seule source de vérité, y compris pour le `DECRQM` de
la tâche 9.

Pour la tâche 7 : `screen.alt_screen()` dit quand ne rien pousser à
l'historique. Pour la tâche 8 : `Screen` détient désormais **deux** grilles,
et le redimensionnement devra traiter `parked_` comme `grid_`.

## Tâche 7 — Le scrollback

**Fichiers :** `src/vt/scrollback.hpp/.cpp`, `tests/test_vt_scrollback.cpp`

Tampon circulaire de **10 000** lignes rognées, configurable. Décalage de consultation, borné aux deux bouts.

Les lignes y entrent **rognées** : garder 300 colonnes de blancs par ligne pour 10 000 lignes coûte pour rien.

**Tests :** au-delà de la capacité, la plus ancienne part ; le décalage ne dépasse ni le haut ni le bas ; une ligne rognée se relit identique à sa partie non vide ; l'écran alterné n'y écrit rien.

- [x] Tâche 7 livrée : tests, mutations, commit — `9e5f168`, 29 tests, 29 mutations
  (1 équivalente)

Comme aux tâches 5 et 6, le périmètre a débordé des fichiers annoncés :
`screen.cpp/.hpp` gagnent `set_scrollback()` et le branchement dans
`scroll_up()`, sans quoi le test « l'écran alterné n'y écrit rien » n'aurait
eu aucun objet. `Screen` ne connaît toujours ni parseur ni couche
d'affichage — `Scrollback` est une structure de données, pas une couche.

Pour la tâche 8 : le redimensionnement devra traiter `parked_` comme
`grid_`, et **ne pas** toucher aux lignes déjà rangées — le reflow est hors
v1 (spec §681), donc une ligne d'historique garde la largeur qu'elle avait.

Pour la tâche 13 : la molette ne défile l'historique que si
`screen.alt_screen()` est faux ; sinon elle part à l'invité. `Shift+PgUp` /
`PgDn` appellent `scroll_back()` / `scroll_forward()`, et toute frappe qui
écrit sur le PTY appelle `scroll_to_bottom()`.

## Tâche 8 — Le redimensionnement

**Fichiers :** `src/vt/screen.cpp` *(étendu)*, `tests/test_vt_resize.cpp`

La politique est écrite noir sur blanc dans la spec, parce que ne pas la décider est la recette exacte du bug « mon terminal est mélangé après un redimensionnement » :

| Aspect | Politique |
|---|---|
| Écran principal, rétrécissement | **Troncature**, pas de reflow |
| Écran principal, élargissement | Complété par des blancs |
| Écran alterné | Jeté et régénéré par l'invité, qui reçoit `SIGWINCH` |
| Curseur | Contraint dans les nouvelles bornes |
| Région de défilement | Remise à pleine hauteur à **tout** changement de taille |

Le reflow invaliderait la position du curseur et **tous les décalages du scrollback** ; c'est un projet en soi, pas une option de v1.

**Tests :** rétrécir puis réélargir ne restitue pas ce qui a été tronqué (c'est la politique, pas un défaut — le test la fige) ; le curseur reste dans les bornes ; la région redevient pleine hauteur ; l'écran alterné est vide après coup.

- [x] Tâche 8 livrée : tests, mutations, commit — `f88cd28`, 18 tests, 21 mutations

Le curseur gardé de côté par `1049` est borné ici : `restore_cursor()` borne
déjà le sien au moment de s'en servir, mais `leave_alt_screen()` ne bornait
rien — la dette laissée à la tâche 6 est soldée.

## Tâche 9 — Les réponses aux requêtes

**Fichiers :** `src/vt/reply.hpp/.cpp`, `tests/test_vt_reply.cpp`

`\033[c` (Device Attributes), `\033[6n` (position du curseur), `DECRQM`. **Un programme qui les émet bloque jusqu'à obtenir sa réponse.**

C'est le démon qui répond, en écrivant lui-même sur le maître du PTY. Relayer la question au vrai terminal serait faux à trois titres : la réponse décrirait le terminal du client, elle arriverait de façon asynchrone, et elle s'intercalerait au milieu des frappes de l'utilisateur.

**Tests :** `\033[6n` après un `CUP` rend la position posée ; `\033[c` rend une identité stable ; la réponse part sur le **maître**, jamais vers le client.

- [x] Tâche 9 livrée : tests, mutations, commit — `5933f43`, 14 tests, 26 mutations

Deux requêtes hors énoncé ont été traitées, parce qu'une question sans
réponse bloque son auteur : `CSI > c` (seconde identité) et `CSI ? 6 n`
(position étendue). `Modes` gagne au passage **une** table unique, en
pointeurs sur membre, partagée par `set`, `knows` et `get` — `DECRQM` en
aurait sinon imposé une troisième lecture.

## Tâche 10 — Le jeu de caractères DEC

**Fichiers :** `src/vt/charset.hpp/.cpp`, `tests/test_vt_charset.cpp`

`SCS ESC ( 0` — semi-graphiques DEC. `mc`, `dialog` et les vieux `ncurses` en dépendent pour leurs cadres.

**Tests :** la table de conversion ; `ESC ( B` revient à l'ASCII ; le jeu courant est sauvé par `DECSC`.

- [x] Tâche 10 livrée : tests, mutations, commit — `548b7b3`, 13 tests, 18 mutations

`SavedCursor` porte désormais ses quatre champs ; la note laissée à la
tâche 3 (« la tâche 10 y ajoutera le jeu de caractères ») est soldée.

## Tâche 11 — L'encodage clavier

**Fichiers :** `src/input/encode.hpp/.cpp`, `tests/test_encode.cpp`

`KeyEvent` → octets pour l'invité. Le miroir exact de `src/input/parser.cpp`, qui fait le chemin inverse.

`DECCKM` change les flèches (`\033[A` ↔ `\033OA`) — un `vim` en mode insertion devient inutilisable si on l'ignore. Les modificateurs s'encodent en paramètre (`\033[1;5C` pour `Ctrl+→`). `Ctrl+lettre` donne l'octet de contrôle ; `Alt+x` préfixe d'`ESC`.

**Tests :** aller-retour `parse(encode(k)) == k` sur toute la table des touches, dans les deux modes de `DECCKM` ; `Ctrl+A` rend `\001` ; `Alt+a` rend `\033a`.

- [x] Tâche 11 livrée : tests, mutations, commit — `f1a8c56`, 18 tests, 26 mutations

## Tâche 12 — `SIGCHLD` et la fin de processus

**Fichiers :** `src/app/app.hpp` *(modifié)*, `src/daemon/host.cpp` *(modifié)*, `src/daemon/session.cpp` *(modifié)*, `src/daemon/daemon.cpp` *(modifié)*, `tests/test_daemon.cpp` *(étendu)*

`Host` gagne `watch_child(pid_t)`, `App` gagne `on_child_exit(int status)`. Le démon reçoit `SIGCHLD` par son `signalfd` existant.

**Les signaux standards ne sont pas mis en file** : trois enfants morts entre deux lectures produisent un seul enregistrement, dont le `ssi_pid` n'en nomme qu'un. On boucle donc sur `waitpid(-1, WNOHANG)` jusqu'à `0` ou `ECHILD`, **et** on draine le `signalfd` jusqu'à `EAGAIN`. Sans cette double boucle : zombies permanents, maîtres jamais fermés, `kernel.pty.max` (4096) épuisé, et des fenêtres qui affichent un shell mort.

**Deux événements indépendants dans les deux sens** : un `nohup … &` garde l'esclave ouvert après la mort du shell, et un enfant qui se démonise ferme l'esclave avant sa propre mort. Deux drapeaux séparés donc — `[processus terminé]` s'affiche au premier, le descripteur n'est libéré qu'au second.

Et l'on ne ferme **jamais** le maître sur simple réception de `SIGCHLD` : cela jetterait la sortie encore en tampon dans la discipline de ligne. Les noyaux récents livrent d'abord les données puis rendent `EIO` — il suffit de drainer.

**Tests :** trois enfants morts d'un coup sont tous récoltés ; aucun zombie ne subsiste ; la sortie écrite juste avant la mort est lue en entier ; un enfant qui ferme l'esclave sans mourir n'affiche pas encore `[processus terminé]`.

- [ ] Tâche 12 livrée : tests, mutations, commit

## Tâche 13 — L'application Terminal

**Fichiers :** `src/apps/terminal.hpp/.cpp`, `src/app/catalog.cpp` *(modifié)*, `tests/test_terminal.cpp`, goldens

Le liant. `attach()` ouvre le PTY et le confie à `Host::watch` ; `on_io()` lit et nourrit le parseur ; `render()` peint la grille dans la `View` ; `on_key()` encode et écrit ; `on_resize()` pose `TIOCSWINSZ` ; `wants_cursor()` suit le curseur de la grille et le mode 25 ; `OSC 0/2` alimente `Host::set_title`.

**Souris transmise aux invités.** Si le terminal focalisé a activé 1000/1002/1003, les événements tombant dans sa zone cliente sont ré-encodés en SGR 1006 avec des coordonnées **locales** et écrits sur le PTY : `htop` reste cliquable, la souris de `vim` fonctionne. Aucun conflit avec le déplacement de fenêtre, qui vit sur la barre de titre et les bordures.

La molette fait défiler le scrollback quand l'écran alterné est inactif ; quand il est actif, elle est transmise à l'invité — comportement des vrais émulateurs.

La fenêtre affiche `[processus terminé (code 1) — Entrée ou clic pour fermer]` et **reste ouverte**, pour qu'on puisse lire la dernière erreur. Un **clic** dans la zone cliente d'un terminal mort la ferme aussi : une fonction qui n'a qu'un raccourci est incomplète. `can_close()` sur un processus vivant demande confirmation, puis `SIGHUP` au groupe de processus, puis `SIGKILL` après un délai de grâce.

**Tests :** la grille arrive dans la `View` ; un titre `OSC 2` remonte au cadre ; le curseur suit ; un redimensionnement pose la bonne taille ; la souris n'est transmise que si le mode est actif ; goldens d'un écran de terminal.

- [ ] Tâche 13 livrée : tests, mutations, commit

## Tâche 14 — Fuzzing du parseur sous ASan

**Fichiers :** `tests/test_vt_fuzz.cpp`

Le tableau des risques le demande explicitement : *« Le parseur VT est le composant le plus long et le plus subtil → machine à états standard, tests par propriété, fuzzing sous ASan, jalon dédié. »*

Fuzzer déterministe, sans dépendance : un générateur ensemencé produit des flux mêlant séquences valides, séquences tronquées, paramètres absurdes (`\033[999999999999m`), UTF-8 invalide et octets aléatoires. Propriétés vérifiées à chaque tour : aucune écriture hors grille, le curseur reste dans les bornes, le parseur revient toujours en `Ground` après un final, aucune fuite, et **le même flux rejoué octet par octet donne le même écran**.

Tourne dans la suite normale avec un budget de temps borné ; la graine est imprimée pour rejouer un échec.

- [ ] Tâche 14 livrée : tests, commit

---

## Vérification manuelle du jalon (§13.6)

À cocher avant de déclarer le jalon fini — aucune de ces lignes n'est couverte par un test automatique :

- [ ] `vim` : édition, couleurs, écran alterné, souris
- [ ] `htop` : rafraîchissement, couleurs, clic sur les colonnes
- [ ] `less` sur un gros fichier : défilement, recherche
- [ ] un `tmux` imbriqué dans une fenêtre
- [ ] un nom de fichier en japonais et un emoji ZWJ
- [ ] un client en 300 colonnes
- [ ] un redimensionnement en pleine compilation
- [ ] `yes | head -1` (prouve la remise à `SIG_DFL` de `SIGPIPE`)
- [ ] `make -j8` (prouve le masque de signaux vidé)
- [ ] une déconnexion SSH franche pendant que les deux tournent
