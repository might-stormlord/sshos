# ssh_os 2.0 — dossier de reprise

> Document destiné à un contexte neuf. Il suppose zéro connaissance préalable de la
> conversation qui a produit le projet. Tout ce qui suit a été vérifié, pas supposé :
> quand un fait vient d'une mesure, la mesure est citée.
>
> **Dernière mise à jour :** 15 août 2026, branche `m1-noyau`, HEAD `81fc657`.
> **1120 tests au vert** en `Release` (19,4 s) comme sous ASan/UBSan (46,7 s),
> 0 avertissement. Arbre de travail propre. 193 commits depuis `main`.
> **Les SEPT jalons sont livrés**, et le travail qui a suivi est demandé au fil de
> l'usage par l'utilisateur. Le §3 donne la position exacte.
>
> **Par où commencer :** §2 pour compiler et lancer, §3 pour savoir où l'on en est,
> §3 bis pour la carte du code, §4 pour ce qui n'est pas négociable, et **§9 bis pour
> le défaut qui revient dix fois dans ce projet**. Le reste se lit à la demande.
>
> ⚠️ Les §4, §8 et §9 (contraintes, méthode, pièges d'environnement) ont été écrits au
> jalon 1 et restent **entièrement valides**. Le §5 décrit un harnais toujours exact,
> mais ses repères chiffrés sont d'époque. Le §6 ne décrit que le contenu du jalon 1 :
> pour les jalons 2 à 7, la source de vérité est leur plan respectif dans
> `docs/superpowers/plans/`, dont les cases cochées portent le commit, le nombre de
> tests et le nombre de mutations de chaque tâche.

---

## 1. Ce qu'est le projet

Un environnement de bureau en mode texte, utilisable à travers SSH, qui **survit à la
déconnexion**. Fermer la fenêtre du terminal ne tue pas la session : on se rebranche et on
retrouve son écran intact, comme `tmux` mais avec des fenêtres, un menu et une barre des
tâches.

- **Langage :** C++20, zéro dépendance externe (pas de gtest, pas de ncurses, pas de fmt).
  Uniquement la bibliothèque standard et l'API POSIX/Linux.
- **Cible :** glibc/Linux (`epoll`, `signalfd`, `timerfd`, sockets UNIX abstraits).
- **Dépôt :** `/home/storm/dev/ssh_os_2.0`, branche de travail `m1-noyau` (base `main`).

> ⚠️ Il existe un ancien projet Rust dans `/home/storm/dev/ssh_os`. **On n'en reprend rien.**
> Consigne explicite de l'utilisateur : « non vieux projet on ne reprend rien de ça ».

### Architecture (choix « A », arrêté par l'utilisateur)

Client **mince**. Le démon détient tout l'état, compose une grille de cellules, calcule un
diff et n'envoie que des séquences ANSI déjà encodées. Le client met son terminal en mode
brut, relaie des octets bruts et recopie ce qu'il reçoit.

**Un seul thread, un seul `epoll`, aucun mutex.** Cette contrainte n'est pas négociable :
elle est la raison pour laquelle le code n'a pas de verrous et ne doit pas en gagner.

Conséquence à connaître avant de déboguer : puisque le protocole est **différentiel**, un
client déjà attaché ne reçoit **que les cellules modifiées**. Chercher une chaîne entière
(« clics: 3 ») dans son flux ne donnera rien après la première trame — seul le chiffre
changé transite. Pour lire l'état réel, **attacher un client neuf** : il reçoit un repeint
complet.

---

## 2. Compiler, lancer, tester

```bash
cd /home/storm/dev/ssh_os_2.0

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"

# Debug = ASan + UBSan (CMakeLists.txt les ajoute lui-même sur ce type)
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j"$(nproc)"
```

> ⚠️ **Il traîne un `build-dbg/` sur cette machine**, vestige du jalon 1 et plus
> reconstruit depuis le 13 août. La documentation d'avant y renvoyait. **Le
> répertoire de travail est `build-debug/`** ; `build-dbg/` peut être effacé sans
> rien perdre.

```bash
./build-release/sshos              # lance le bureau (démarre le démon si besoin)
./build-release/sshos --status     # « demon actif (pid N) » ou « aucun demon »
./build-release/sshos --kill       # arrête le démon
./build-release/sshos --daemon     # démarre le démon sans s'attacher
```

```bash
./build-release/sshos_tests            # ~19 s
./build-debug/sshos_tests              # ~47 s (ASan + UBSan, facteur 2,4)
./build-release/sshos_tests files_     # filtre par sous-chaîne du nom
```

**Attendu : `1120 cas, 0 en echec, 0 assertions echouees`,** en Release comme en
Debug, avec 0 avertissement de compilation (`-Wall -Wextra -Wpedantic -Werror`).

> Le binaire de test s'appelle **`sshos_tests`** (pas `sshos-test`). Erreur commise
> plusieurs fois.

### Le geste de vérification à la main

1. `./build-release/sshos`
2. `Ctrl+A` puis `Espace` ouvre le menu ; filtrer au clavier, `Entrée` lance.
3. Dans un Terminal, poser une marque : `MARQUE=persiste`.
4. **Fermer la fenêtre du terminal** — ou `Ctrl+Q`, qui détache explicitement.
5. Relancer `./build-release/sshos` → `echo $MARQUE` répond `persiste`.

`Ctrl+Q` **détache** et ne détruit rien ; détruire la session pour de bon se demande
par l'entrée « Fermer la session » du menu, qui pose une confirmation.

## 3. Où en est le projet dans la feuille de route

Source : `docs/superpowers/specs/2026-08-10-ssh-os-design.md` §15.

| Jalon | Contenu | Sortie visible | État |
|---|---|---|---|
| **1** | Rendu, diff, protocole, client, démon | Une boîte colorée à l'écran, à travers SSH | ✅ **livré** |
| **2** | WM, panneau, menu, application factice | Tout le geste testable sans PTY | ✅ **livré** |
| **3** | Terminal | *Le projet devient utilisable pour de vrai* | ✅ **livré** |
| **4** | Gestionnaire de fichiers | Naviguer, renommer, supprimer | ✅ **livré** |
| **5** | Moniteur système | Processus, CPU, mémoire, réseau | ✅ **livré** |
| **6** | Éditeur | Ouvrir, modifier, enregistrer | ✅ **livré** |
| **7** | Le gestionnaire de fichiers, façon Dolphin | Vue scindée, sélection multiple, copier/coller sans bloquer, colonnes triables, historique, raccourcis | ✅ **livré** |

### Après la v1 — ce que l'utilisateur a demandé depuis, et qui est livré

Ces travaux ne figurent dans aucun plan : ils sont venus un par un, en réaction à
l'usage réel. Ils sont tous dans l'historique de `m1-noyau`.

| Demande | Ce qui a été fait | Commit |
|---|---|---|
| Retirer `Bloc` et `Battement` | Ils deviennent des doublures de test (`tests/fake_apps.hpp`) ; le catalogue ne les propose plus. La session amorce désormais par une **fabrique** injectable (`set_seed_factory_for_tests`) | `f119b26` |
| Repositionnement intelligent | Entrée de menu « ranger les fenêtres » : grille en colonnes, reste aux premières (`src/wm/tile.cpp`) | `8b7cebd` |
| Deux sorties qu'on ne confond plus | `Ctrl+Q` détache (la session survit) ; « Fermer la session » détruit, **avec confirmation** | `8b7cebd` |
| Le moniteur devient un widget | L'application disparaît ; le fond d'écran porte quatre sections encadrées (CPU, mémoire, réseau, charge, processus), jauges vertes/jaunes/rouges, signature « SSH OS » centrée | `444bac6`, `3919486`, `1600142` |
| Ouvrir deux fois la même application | Le menu et le **clic droit** sur la barre des tâches ouvrent une **nouvelle instance** ; le clic gauche garde le rappel | `a0924ec` |
| Ancrage façon Windows | `Ctrl+flèche` ancre la fenêtre active sur une moitié d'écran, **sans passer par l'accord** — trois touches pour un geste qu'on répète était inutilisable | `1d5e3ff`, `7987cc6` |
| Onglets du terminal | Chaque onglet a son PTY, sa grille, son historique et ses modes. Barre cliquable toujours visible (`+` pour ouvrir, `×` pour fermer, **re-cliquer l'onglet actif le renomme**), et `Alt+t` / `Alt+w` / `Alt+flèches` / `F2` au clavier | `80eda0d`, `d818249`, `3daa305` |
| Le cadre nomme l'onglet | `Terminal (build)` plutôt que `build` tout court | `f872c25` |
| Barre des tâches lisible | Le libellé s'étire dans la place libre ; huit cellules restent le **plancher**, pas la mesure | `a4ed85b` |
| **Le caret** | `App::wants_cursor()` n'avait **aucun appelant** : le démon passait `std::nullopt` à chaque trame, et aucun champ de saisie du bureau n'avait de curseur | `41eb781` |
| **Fermer emporte le groupe** | `~Pty` ne fermait que le maître : un shell qui refuse SIGHUP survivait pour toute la vie du démon | `41eb781` |
| **Assistance à l'ancrage** | Ancrer laisse une moitié vide ; le bureau y propose les autres fenêtres, un clic en amarre une | `b888ec5` |
| L'aide dit les gestes directs | Section « Sans accord : » — `Ctrl+Q`, `Ctrl+flèches`, les onglets, `F2` | `2121e43` |
| Hygiène du balayage | Un signal n'est plus annoncé comme un code de sortie ; `hit_window_at` délègue au chemin du clic ; **HTS et TBC** branchés (`tabs -4` marche) | `7f8de71`, `0a32551`, `222cd7c` |

**Ce que l'ancrage coûte, et c'est assumé :** `Ctrl+gauche` et `Ctrl+droite`
déplacent par mot dans `readline`, donc dans tout shell. Le bureau les prend à
l'invite ; les flèches nues et `Maj+flèche` lui restent.

**Ce que la barre d'onglets coûte, et c'est assumé :** une ligne de grille, même à
un seul onglet. Elle porte le `+`, qui est la **seule voie à la souris** vers un
second onglet — et l'utilisateur pilote à la souris.

**Ce que la fermeture d'une fenêtre emporte, et c'est mesuré** (`src/pty/pty.cpp`,
en face de `Pty::shutdown()`) :

| | fermer le maître seul | + SIGKILL au groupe |
|---|---|---|
| shell ordinaire | mort | mort |
| tâche de fond | partie | partie |
| **`trap '' HUP`** | **vivant, pour toujours** | mort |
| enfant `setsid` | vivant | vivant |

`setsid` reste la porte de sortie — c'est ce que font `nohup` et `disown`, et
c'est la seule façon de faire survivre un travail à sa fenêtre.

**L'aide entière tient dans un 80×24, et c'est cette contrainte qui a décidé de
sa mise en page.** Une ligne vide de séparation en plus, ou une ligne de
raccourci de plus, et la dernière ligne tombe hors du cadre — c'est-à-dire celle
des gestes qu'on ne peut découvrir autrement.

**Ce que le widget de fond coûte :** le bureau n'est plus à 0 jiffie au repos. Il
se rafraîchit une fois par seconde, mesuré à **12 jiffies / 3 s**, et cette valeur
est la même à zéro, un ou deux terminaux ouverts (sonde du 13 août 2026).

Le jalon 3 tient sa promesse, et elle a été vérifiée à la main : `vim`, `htop`,
`less` et un `tmux` imbriqué tournent dans une fenêtre, une compilation survit à
trois `SIGWINCH` d'affilée, et **le shell survit à la mort du client**. Voir la
liste de vérification à la fin de
`docs/superpowers/plans/2026-08-12-ssh-os-m3-terminal.md`.

**Le rythme d'une tâche**, invariant depuis le jalon 1 : code + tests écrits
d'abord (le rouge est constaté, pas supposé), commit `wip(...) avant campagne de
mutation`, campagne, tests ajoutés pour chaque survivante, commit
`feat(...) (jalon N, tache M)`, puis `docs(mN): tache M cochee`.

**Volume réel au 15 août 2026 : 40 777 lignes sur 162 fichiers** — `src/` en compte
16 568 sur 108, `tests/` 24 209 sur 54. L'estimation d'origine (12 000 à 15 000) est
dépassée d'un facteur trois, et le rapport tests/code d'environ 1,5 pour 1 n'est pas
une coquille : c'est ce qui rend les campagnes de mutation possibles.

> **Point d'attention relationnel.** À la livraison du jalon 1, l'utilisateur a été déçu :
> « aucun changement sur l'app c'est encore que des click de souris qui augmente ». C'est
> attendu et conforme — le compteur de clics est le *mouchard* du jalon 1, pas
> l'application : la plus petite chose prouvant qu'une entrée souris traverse le parseur,
> modifie l'état du démon, repasse par le diffeur et survit à la mort du terminal. Mais
> **il faut le dire avant, pas après.** Le jalon 2 est celui qui produit du visible.

---

## 3 bis. La carte du code

~16 500 lignes dans `src/`, ~24 200 dans `tests/`. **Le rapport n'est pas une
coquille** : le projet écrit plus de tests que de code, et c'est ce qui rend les
campagnes de mutation possibles.

| Module | Ce qu'il fait | À savoir avant d'y toucher |
|---|---|---|
| `src/common/` | Descripteurs, sockets UNIX abstraits, file de sortie, protocole, UTF-8, horloge de trame | `OutQueue` a un plafond ; son dépassement se classe *Clean* ou *Dirty* et la réaction diffère (A7) |
| `src/render/` | `Surface` (grille de cellules), `View` (sous-rectangle clippé), `Differ` (trames ANSI), thème, largeurs Unicode | Une application ne reçoit **jamais** autre chose qu'une `View` — elle ne peut pas peindre hors de sa fenêtre |
| `src/input/` | Machine à états du clavier et de la souris, table des accords `<leader>` | `\033` seul est ambigu : le démon arme un délai de 50 ms. Sans lui, `vim` est inutilisable |
| `src/vt/` | Émulation VT : parseur DEC, écran, historique, SGR, modes, réponses, jeux de caractères | `Screen` est pure : ni descripteur, ni horloge. C'est ce qui la rend fuzzable |
| `src/pty/` | Pseudo-terminal, environnement de l'enfant | `Pty::shutdown()` porte **toute** la politique de fermeture (SIGHUP, maître, SIGKILL) : le destructeur et la fermeture d'onglet l'appellent tous deux |
| `src/wm/` | Fenêtres, pile, décorations, hit-test, ancrage, rangement | `hit_window()` est l'inverse exact de `draw_decor()` ; les deux lisent la **même** géométrie |
| `src/shell/` | Panneau, menu, modale, aide, horloge, moniteur de fond, assistance à l'ancrage | Chaque composant calcule sa géométrie **une fois** dans `layout()`, et `draw()` comme `hit()` la relisent |
| `src/daemon/` | Boucle `epoll`, session, hôte applicatif, démonisation, récolte | `Session::render()` compose **toute** la géométrie ; `Session::on_mouse()` route les gestes |
| `src/app/` | Le contrat `App` / `Host`, le catalogue du menu | Tout est virtuel avec un défaut utilisable : une application qui ne sait que dessiner n'écrit qu'une méthode |
| `src/apps/` | Terminal (à onglets), Fichiers (façon Dolphin), Éditeur | Chacune ne connaît que `View`, `Host` et les événements |

### Ce qu'une application a le droit de demander (`src/app/app.hpp`)

`set_title`, `request_close`, `invalidate`, `watch`/`unwatch` (un jeton opaque, jamais
l'epoll), `watch_child` (la récolte est globale au démon), et **`open_app`** — « ouvre
ça dans sa propre fenêtre », par quoi Fichiers ouvre l'Éditeur sans rien savoir du
bureau.

Côté `App` : `attach`, `render`, `on_key`, `on_mouse`, `on_resize`, `on_io`,
`on_child_exit`, `wants_cursor`, `min_size`, `can_close`, `refresh_ms` et
**`on_refresh`** — le travail périodique **hors du rendu**, puisque `render()` ne doit
toucher ni au disque ni au réseau.

---

## 4. Contraintes non négociables

Elles proviennent du plan (« Global Constraints ») et de consignes explicites de
l'utilisateur. Les violer casse la compilation ou se fait rejeter en revue.

| Contrainte | Détail |
|---|---|
| **Commentaires en français, avec accents** | Le code et les identifiants sont en **anglais**. Les commentaires portent le *pourquoi*. Un round a été nécessaire pour recorriger des commentaires écrits sans accents (`be9a62c`). |
| **`\033`, jamais `\e`** | `\e` est une extension GCC. Avec `-Wpedantic -Werror`, **une seule occurrence casse la compilation du projet entier**. |
| **Ne pas modifier `CMakeLists.txt`** | Contrainte reprise dans tous les briefs. |
| **Zéro dépendance externe** | Ni gtest, ni ncurses, ni fmt. |
| **Un thread, un `epoll`, aucun mutex** | |
| **Un seul binaire `sshos`** | modes `(aucun)` / `--daemon` / `--kill` / `--status`. |
| **Pas de commande détachée** | Jamais `nohup`, `&` ou `disown` dans le travail d'un sous-agent : **deux agents s'y sont bloqués définitivement** (round T11/2, round T13/1) et le contrôleur a dû relever les résultats à leur place. |
| **Le contrôleur ne touche pas à la configuration système** | Sans accord explicite de l'utilisateur. (Le changement de fuseau horaire a été fait *par lui*.) |

Autres valeurs figées : plafond de rendu **33 ms** (30 fps), contre-pression **1 Mo**,
plafond de fenêtres **64**, délai d'ambiguïté `ESC` **50 ms**, touche leader `Ctrl+A`,
adresse de socket abstraite `\0sshos/<uid>/<boot_id>`.

### Détail de compilation à connaître

`CMakeLists.txt` contient `else() add_compile_options(-O2)`. La ligne réelle finit par
`-O3 -DNDEBUG … -Werror -O2` : **le dernier `-O` gagne**, donc Release compile bien en `-O2`.
Prouvé par comparaison des vidages `g++ -Q --help=optimize`. Ce n'est pas un bug, ne pas
le « corriger ». Les globs de sources sont en `CONFIGURE_DEPENDS`.

---

## 5. Le harnais de test — pièges

`tests/harness.hpp` est maison. Deux familles de macros :

- `CHECK` / `CHECK_EQ` : enregistrent l'échec et **continuent**.
- `REQUIRE` / `REQUIRE_EQ` : **retournent** (par un `return;` nu, pas une exception).

> **Conséquence :** toute ressource libérée en fin de fonction **fuit** sur le chemin
> d'échec d'un `REQUIRE`. Un défaut *Critical* réel en est né (petit-fils de processus
> bloqué à vie parce qu'un `REQUIRE` sautait sa libération). D'où les gardes RAII
> `FifoReleaseGuard`, `WaitpidGuard`, `UnlinkGuard`, `TzGuard` dans les tests.

### Garde-temps

Le lanceur (`tests/main.cpp`) fonctionne en **superviseur / ouvrier** : un seul `fork()`,
deux sémaphores POSIX anonymes en `mmap(MAP_SHARED|MAP_ANONYMOUS)`, `sem_timedwait()`, et
un balayage par tranches de 50 ms avec `waitpid(WNOHANG)`. Objectif : un test qui se bloque
ou qui plante n'immobilise pas la suite.

Mesures de référence **du jalon 1** : coût nominal négligeable (7,02 / 7,08 / 6,97 s sans
garde contre 7,11 / 7,18 / 7,04 s avec) ; un `SIGSEGV` injecté coûtait 38 s avant
l'optimisation par tranches, **7 380 ms après**. Deux cas expirent volontairement.

> La suite entière prend **19,4 s en Release** et **46,7 s sous ASan/UBSan** au
> 15 août 2026 (1120 cas). L'essentiel de ce temps est de l'attente délibérée :
> `user+sys` ne fait que 3,9 s des 19,4 s de mur, donc **80 % du temps est passé à
> attendre des sous-processus** — pseudo-terminaux, démons, copies — et non à
> calculer.

**Ajouter un fichier de tests ne demande rien** : `CMakeLists.txt` fait un
`GLOB tests/test_*.cpp`. Le nom du fichier n'a pas d'importance, mais le **préfixe
des `TEST(...)` sert de filtre** en ligne de commande — d'où `files_`, `copy_`,
`terminal_`, `session_`, `daemon_`, `dir_`, `snapassist_`…

> ⚠️ **Tous les cas tournent dans le MÊME processus**, et ceux de `test_daemon.cpp`
> appellent `reap_children()`, qui fait `waitpid(-1, WNOHANG)`. Un cas qui `fork()`
> doit donc **récolter ses propres pids avant de rendre la main**, même ceux qu'il
> vient de tuer : un zombie oublié est ramassé par le premier cas de démon qui passe,
> et le `try_reap()` qui l'attendait reçoit `ECHILD` pour toujours. Mesuré : un échec
> **sur quatre lancements**, sur un cas que personne n'avait touché.

---

## 6. Ce que contient le jalon 1

13 tâches, toutes livrées, relues et fusionnées. **70 commits** depuis `main` —
*à l'époque du jalon 1 seul* ; la branche en porte **193** au 15 août 2026.

| Fichier | Responsabilité |
|---|---|
| `src/common/fd.*` | `Fd` RAII, `set_nonblock`, `set_cloexec` |
| `src/common/net.*` | Socket UNIX abstrait : `bind` comme mutex, `connect`, `SO_PEERCRED` |
| `src/common/proto.*` | Codec des messages, décodeur incrémental (`Decoder`) |
| `src/common/outqueue.*` | File de sortie, contre-pression, libération de capacité |
| `src/common/frameclock.hpp` | Cadence de rendu (30 fps) |
| `src/common/utf8.*` | `utf8_decode`, `encode_utf8` |
| `src/common/platform.*` | Enveloppes système |
| `src/render/cell.hpp` | `Color`, `Style`, `Cell`, `Rect`, `Size`, `Pos` |
| `src/render/width.*` | Table Unicode de largeur embarquée, politique East Asian Ambiguous |
| `src/render/surface.*` | `Surface` (grille) et `View` (rectangle clippé et translaté) |
| `src/render/profile.*` | Profil de sortie, quantification des couleurs, encodage SGR |
| `src/render/diff.*` | Diffeur : enveloppe de frame, état SGR, règles de largeur |
| `src/input/events.hpp` | `KeyEvent`, `MouseEvent`, `PasteEvent`, `FocusEvent` |
| `src/input/parser.*` | Octets → événements : CSI, souris `Cb`, collage, `ESC` isolé |
| `src/client/tty_guard.*` | Mode brut, restauration, filet de sécurité sur crash |
| `src/client/client.cpp` | Boucle client |
| `src/daemon/daemonize.*` | Détachement (double `fork`, `setsid`) |
| `src/daemon/session.*` | État de session, composition de l'écran |
| `src/daemon/daemon.cpp` | Boucle `epoll` du démon |
| `src/main.cpp` | Aiguillage des modes |

### Décisions dont le *pourquoi* ne se devine pas

Ces points ont coûté cher à établir. Ne pas les défaire sans relire la mesure.

- **`TtyGuard`, armement du filet de crash.** GCC plaçait l'armement **au milieu** de la
  copie du `termios` (vérifié par désassemblage : `nm` donne `g_crash_fd` en `.data`,
  `g_crash_saved` en `.bss`, 60 o). Corrigé par `std::atomic_signal_fence` release/acquire.
- **`daemonize`, pas de `signal(SIGHUP, SIG_IGN)` avant `execv`.** Une disposition `SIG_IGN`
  est **héritée à travers `execv`** — sonde : `SigIgn: 0000000000000001` avec, `0` sans.
- **`OutQueue::release_buffer()`** (`std::string{}.swap`) est appelée par `compact()` **et**
  par le rejet, seuil `kReleaseCapacityThreshold = 8 Mio`. Sonde : un pic de 4 Mio reste
  retenu, 9 et 20 Mio retombent à une capacité de 15, le chemin chaud reste stable à 800 o.
- **`take_overflow()` renvoie `OutQueue::Overflow{None, Clean, Dirty}`** et la fusion est
  **monotone** : `if (this_rejection > overflow_) overflow_ = this_rejection;`. Sans cela un
  second rejet dégradait `Dirty` en `Clean`. Un `static_assert` sur `None < Clean < Dirty`
  (via `std::underlying_type_t`) protège l'ordre de déclaration de l'enum.
- **Boucle active du démon.** Le `timeout` d'`epoll_wait` est calculé **au site de
  consommation** : `const bool renderable = client && client->differ; timeout = renderable ?
  clock.delay_ms(...) : -1;`, plus `FrameClock::reset()` dans `drop_client()`. Sans cela :
  **201 jiffies / 2 s sans aucun client** (mesuré).
- **Horloge locale.** `::localtime_r` précédé de `::tzset()`, jamais `::gmtime_r`.
- **Éviction du client** déplacée de l'`accept` vers la réception d'un `Hello` compatible,
  avec une architecture `client` / `pending`, une seule `pending` à la fois et
  `kPendingHelloTimeout{5000}`. Sans cela, `sshos --status` **détachait la session**.
- **`Decoder::failed()`** est consulté aux trois emplacements, **après drainage** et **avant
  promotion**. Il était né sans aucun appelant.

---

## 7. Dette ouverte

### 7.1 — Round `EPOLLHUP` / drainage : **soldé** ✅ (11 août 2026)

Le dernier élément inachevé du jalon 1. **Il n'y a plus de dette ouverte sur le jalon 1.**

- **Symptôme :** cliquer puis fermer le terminal dans la même fraction de seconde perdait
  les tout derniers messages.
- **Cause :** la branche du client attaché honorait `EPOLLHUP|EPOLLERR` puis faisait
  `continue` **avant** de drainer `EPOLLIN`. `epoll_wait()` coalesce les deux bits en un
  seul réveil quand le pair écrit puis ferme aussitôt : les octets déjà arrivés partaient à
  la poubelle. Même motif sur la branche `pending`.
- **Correctif appliqué** (+51/−20, les deux branches) : le bloc `EPOLLHUP|EPOLLERR`+
  `continue` est retiré du haut et remplacé, **après** le bloc `EPOLLIN`, par
  `if (closed || (events & (EPOLLHUP | EPOLLERR)) != 0) { drop_*(); continue; }`. Le `||`
  garantit **un seul** `drop_*` par fermeture ; `dec.failed()` reste après le drainage et
  reste atteignable ; `session.wants_quit()` reste dans le bloc `EPOLLIN`.
- **Trois tests discriminants**, dans `tests/test_session.cpp`, **20/20 en échec contre le
  code d'avant** :

  | Cas | Observable |
  |---|---|
  | `daemon_processes_input_sent_just_before_the_client_closes` | Ctrl+Q envoyé juste avant la fermeture arrête quand même le démon |
  | `daemon_keeps_clicks_sent_just_before_the_client_closes` | 3 clics puis fermeture : un client neuf lit `clics: 3` (le symptôme utilisateur) |
  | `daemon_honours_a_hello_coalesced_with_its_senders_closure` | branche `pending` : un Hello coalescé avec la fermeture de son expéditeur est honoré (le client en place reçoit `Detached`) |

  Sortie exacte contre le code d'avant :

  ```
  FAIL tests/test_session.cpp:1476  REQUIRE(exited)
  FAIL tests/test_session.cpp:1546  CHECK(wait_for_frame_containing(b.get(), dec_b, "clics: 3", "ssh_os", 3000))
  FAIL tests/test_session.cpp:1622  CHECK(saw_detached)
  189 cas, 3 en echec, 3 assertions echouees
  ```

- **Piège trouvé en route, à retenir : `kill(SIGSTOP)` est asynchrone.** Il rend la main
  avant que la cible ne soit réellement arrêtée. Écrire dans la foulée laisse au démon une
  fenêtre pour drainer l'`EPOLLIN` tout seul avant de se figer — le test passe alors contre
  le code défectueux. **Mesuré, pas supposé :** le test des clics n'échouait que **5 fois
  sur 10** et celui du Ctrl+Q **19 fois sur 20**. Parade : `wait_until_stopped()` attend
  l'état `T` dans `/proc/<pid>/stat` (champ 3), processus désigné **par son pid**. Avec
  elle, les trois cas sont à **20/20**.
- **Absence de boucle active : mesurée.** Sonde dédiée, pid connu par `fork()` direct.
  **0 jiffie / 2 s** sur les trois scénarios de fermeture (client attaché qui écrit puis
  ferme ; sonde muette ; Hello puis fermeture immédiate). La sonde est elle-même
  discriminante, vérifié par mutation : en neutralisant la garde par `if (false)`, elle
  mesure **194 jiffies / 2 s** — cohérent avec les 201 jiffies/2 s du défaut de boucle
  active documenté au §6.
- **Restauration du code de production vérifiée par `sha256sum`** après la mutation :
  `b8964e6ed7d59eb66217258573af5a24dc1d0dcfce8507b7fa9d880adf463909`.
- **Brief d'origine :** `docs/hup-drain-brief.md` (conservé comme trace du round). Le
  fichier `docs/hup-drain-correctif-en-suspens.diff` a été **supprimé** : le correctif est
  désormais dans l'arbre, et garder un diff « en suspens » déjà appliqué induit en erreur.
  Il reste récupérable dans l'historique git.

### 7.2 — Points reportés au jalon 2 — **et toujours ouverts après le jalon 7**

> Le titre d'origine disait « reportés au jalon 2 ». Ils n'ont été traités ni au
> jalon 2 ni aux suivants : ils sont encore là. Vérifié le 15 août 2026 —
> `struct Hello` (`src/common/proto.hpp`) ne porte toujours aucun champ de fuseau,
> et `src/apps/files/files.cpp` répète le même défaut pour la colonne « Date ».

| Point | Détail |
|---|---|
| **Rétention mémoire par connexion** | ~8 Mio (`OutQueue`) + ~33 Mio (`Decoder`) retenus par client. Acceptable à un client, à revoir quand il y en aura plusieurs. |
| **« Le fuseau de qui ? »** | Le démon affiche *son* fuseau. Avec un client distant dans un autre fuseau, c'est faux. Le message `Hello` devra porter le fuseau du client. |

### 7.3 — Points mineurs connus

- **Garde A2 non discriminable.** Une garde du code est conservée mais aucun test ne la
  distingue. Constaté honnêtement à deux reprises plutôt que maquillé par un test complaisant.
- **`~DaemonHandle`** tue un pid qui pourrait avoir été recyclé (*Minor*).
- Le plan `docs/superpowers/plans/2026-08-10-ssh-os-m1-noyau.md` porte **6 marqueurs
  `PERIME`** sur des blocs dont le code a divergé (dont `class Decoder` l. 1891 et
  l'éviction à l'`accept` l. 4315). Les lire comme tels.

---

### 7.4 — L'état au 15 août 2026

Ce qui suit remplace toute lecture d'avancement faite ailleurs. Les §7.1 à 7.3
ci-dessus datent du jalon 1 : 7.1 est **soldé**, 7.2 et 7.3 restent **vrais**.

| Point | État | Ce qu'il coûte aujourd'hui |
|---|---|---|
| Rétention mémoire par client (~41 Mio) | ouvert, §7.2 | Acceptable à un client ; à revoir quand il y en aura plusieurs |
| Fuseau horaire du démon, pas du client | ouvert, §7.2 | L'horloge du panneau **et** la colonne « Date » de Fichiers mentent pour un client distant. Le `Hello` devra porter le fuseau |
| Garde A2 non discriminable | ouvert, §7.3 | Conservée et déclarée ; aucun cas ne la distingue |
| `~DaemonHandle` tue un pid recyclable | ouvert, §7.3 | *Minor* |
| 6 marqueurs `PERIME` dans le plan du jalon 1 | ouvert, §7.3 | À lire comme tels |
| Menu contextuel du clic droit sur la **barre des tâches** | ouvert | Il agit directement (nouvelle instance) sans rien proposer. Le gestionnaire de fichiers, lui, en a un depuis le 15 août |
| Semis de points du fond d'écran | retiré volontairement | Il rendait chaque repeint complet **27 % plus gros** (mesuré), ce qui faisait basculer `daemon_dirty_overflow_closes_the_connection` du rejet *Dirty* au rejet *Clean*. Le commentaire qui dit pourquoi est resté là où il se rebrancherait |
| `Pty::saw_eof()` sans lecteur | documenté sur place | Sous Linux un maître dont le dernier esclave s'est fermé rend `EIO`, pas 0 : `note_eof()` n'est atteinte que dans des cas de bord |
| `set_cloexec()` sans appelant | documenté sur place | Chaque descripteur naît déjà `CLOEXEC` en un seul appel système, ce qui est le motif **sûr** |

**Il n'y a aucune dette cachée connue au-delà de cette table.** Le balayage des
méthodes sans appelant (§9 bis) a été passé le 15 août : ses candidats restants sont
tous vérifiés à la main et légitimes.

---

## 8. Méthode de travail

L'utilisateur a choisi le **mode 1** : *un sous-agent frais par tâche, revue entre chaque*
(SDD — subagent-driven development).

> ⚠️ **`.superpowers/` est ignoré par git** (`.gitignore:3`). Le ledger, les 20 briefs et les
> ~20 diffs de revue **n'existent que sur le disque de cette machine** et disparaissent à un
> `clone`. Ce qu'un contexte neuf doit absolument avoir a donc été recopié dans `docs/`.

- **Ledger :** `.superpowers/sdd/2026-08-10-ssh-os-m1-noyau/progress.md` *(non versionné)*.
- **Plan :** `docs/superpowers/plans/2026-08-10-ssh-os-m1-noyau.md`.
- **Spec :** `docs/superpowers/specs/2026-08-10-ssh-os-design.md`.
- Chaque tâche a un `task-N-brief.md` (consigne) et souvent un `task-N-report.md` (retour).
  Les rounds de correction ont leurs propres briefs (`clock-round-brief.md`,
  `integration-fix-brief.md`, `branch-review-brief.md`, `hup-drain-brief.md`,
  `final-review-brief.md`). Les diffs relus sont archivés en `review-*.diff`.
- « **parfait continue** » de la part de l'utilisateur signifie : *exécution continue entre
  les tâches, sans redemander*.

### Ce que la revue a appris

Le contrôleur a trouvé plusieurs défauts *Critical* que la revue automatique avait manqués,
et a **rejeté** plusieurs constats de revue avec preuve à l'appui. Deux réflexes valent la
peine d'être conservés :

1. **Un correctif n'est acquis que si un test échoue sans lui.** Systématiquement vérifié en
   recompilant la suite *actuelle* contre le commit *d'avant* (ex. : suite courante contre
   `193045a` → 1 échec ; contre `54019f9` → échec de compilation).
2. **Une garde qu'aucun test ne discrimine doit être déclarée telle**, pas couverte par un
   test qui passerait de toute façon. Vérifié par mutation : seul `CHECK(unblocked)`
   (`test_daemonize.cpp:498`) discriminait ; `CHECK(elapsed_ms >= 3500)` (`:477`) passait
   même sous la mutation.

---

## 9. Pièges d'environnement — faux positifs récurrents

Tous ont été rencontrés pour de vrai, plusieurs fois. Ils font perdre des heures.

| Piège | Parade |
|---|---|
| **`ps` / `pgrep -f` matche sa propre ligne de commande.** Survenu **3 fois**, dont un « défaut reproduit » entièrement faux. | **Ne jamais identifier un processus par correspondance de nom.** Utiliser `/proc/PID/cwd`, ou `sshos --status` (qui s'appuie sur `SO_PEERCRED`). |
| **`grep -i FAIL`** matche le nom de test `..._after_failed_explicit_release`. | Filtrer sur la ligne de bilan, pas sur une sous-chaîne. |
| **`$PPID` est figé à l'initialisation du shell.** Mesuré `$PPID=2757136` (parent déjà mort) contre un ppid réel de `1`. A produit un test instable à 2/30. | `$(cut -d' ' -f4 /proc/$$/stat)`. |
| **`dash` réinitialise le masque de signaux hérité**, mais **pas** les dispositions `SIG_IGN`. | Tester avec `/bin/cp`, pas `sh -c grep`. |
| **`redirect_std_to_devnull()`** écrase les fd 0/1/2 avant `execv`. | En tenir compte dans toute sonde qui espère lire une sortie. |
| **Injecter des octets dans un pty :** écrire sur `/dev/tty` depuis son propre shell n'atteint **pas** le pty du programme testé. | Alimenter l'entrée via un tube nommé : `script -qc "$S" /dev/null < fifo`, en gardant le tube ouvert (`exec 3>fifo`). |

---

## 9 bis. Le défaut signature du projet — et comment le trouver en deux minutes

**Dix fois**, du code a existé sans aucun appelant en production. Aucune suite de
tests ne l'a jamais signalé, parce que ce qui manque n'est pas la couverture : c'est
**l'appel**. Un test unitaire ne peut pas le voir. Une campagne de mutation non plus —
muter du code mort ne casse rien, et la mutation se déclare « équivalente ».

| # | Ce qui n'était pas branché | Ce que ça coûtait |
|---|---|---|
| 1 | `Decoder::failed()` | Trouvé en revue |
| 2 | La garde A2 | Jamais discriminée |
| 3 | `InputParser::timeout()` | **`vim` inutilisable** : `Échap` ne quittait jamais le mode insertion. 732 tests au vert |
| 4 | `App::refresh_ms()` | Le moniteur ne se rafraîchissait pas |
| 5 | `App::wants_cursor()` | **Aucun curseur nulle part** dans le bureau. Mesuré : 6 `?25l` envoyés, 0 `?25h` |
| 6 | `Pty::kill_now()` | Un shell qui refuse SIGHUP survivait à la fermeture de sa fenêtre, pour des semaines |
| 7 | `snap_opposite()` | L'assistance à l'ancrage, écrite ET testée, sans appelant pendant des jours |
| 8 | `Screen::set_tab()` et sa famille | `ESC H` et `CSI g` non traités : `tabs -4` ne faisait rien |
| 9 | `Files::display_label()` | Devenue une duplication silencieuse de la règle de nommage |
| 10 | **Les mouvements de souris** | `Session::on_mouse` : *« au-delà de cette ligne, tout est un appui »*. Aucune application n'a jamais reçu un mouvement. Tout un glisser-déposer écrit au-dessus d'un canal inexistant |

**Le n° 10 est le plus instructif.** Ses six cas unitaires appelaient
`files.on_mouse(Motion…)` **directement**. Ils prouvaient que le gestionnaire réagit
bien à un mouvement ; ils ne prouvaient rien sur le fait que quelqu'un lui en envoie.
**Un test qui appelle la méthode lui-même ne teste jamais son appelant.**

### Le balayage, à repasser après tout gros ajout

```python
# Toutes les fonctions declarees dans src/**.hpp, sans appelant dans src/.
import re, os, io
bodies, decl_lines, decls = [], set(), {}
for root, _, files in os.walk("src"):
    for f in sorted(files):
        if not f.endswith((".cpp", ".hpp")): continue
        text = io.open(os.path.join(root, f), encoding="utf-8").read()
        bodies.append(text)                      # les .hpp AUSSI : beaucoup
        if not f.endswith(".hpp"): continue      # d'appels sont en ligne
        for line in text.split("\n"):
            t = line.strip()
            if t.startswith("//") or t.startswith("*"): continue
            m = re.match(r"^[A-Za-z_][\w:<>,&\* ]*\s[\*&]?([a-z_][a-z0-9_]*)\s*\(", t)
            if m and t.rstrip().endswith(";"):
                decls.setdefault(m.group(1), os.path.join(root, f))
                decl_lines.add((m.group(1), t))
whole = "\n".join(bodies)
for name, where in sorted(decls.items()):
    if name.endswith("_for_tests"): continue
    calls = 0
    for line in whole.split("\n"):
        t = line.strip()
        if t.startswith("//") or (name, t) in decl_lines: continue
        if re.match(r"^[A-Za-z_][\w:<>,&\* ]*\s[\*&]?(\w+::)?%s\s*\(" % name, t):
            continue                             # sa definition
        calls += len(re.findall(r"(?<![\w])%s\s*\(" % name, line))
    if calls == 0: print("  %-26s %s" % (name, where))
```

**Trois pièges du script, tous rencontrés :**

1. **Ne lire que les `.cpp`** rate les appels faits depuis les définitions en ligne
   des `.hpp`.
2. **Une borne `[^\w:]` devant le nom** exclut tout appel qualifié
   `Classe::methode()` et fait passer la moitié du projet pour orpheline.
3. **Un `head -N` sur le `grep` de vérification** fait passer une méthode appelée pour
   une orpheline. Vérifier chaque candidat **sans troncature**.

Les accesseurs suffixés `_for_tests` sont des faux positifs légitimes — **d'où le
suffixe, à mettre systématiquement** sur toute méthode qui n'existe que pour les
tests. C'est ce qui rend le balayage exploitable.

**Et le filet qui attrape ce que le balayage ne voit pas :** une sonde bout-en-bout
qui pilote le **vrai démon** sous pty et lance de **vrais programmes**. Les défauts
3, 4, 5 et 10 n'ont été vus que comme ça.

---

## 10. Où l'on en est, et ce qui reste

**Les sept jalons sont livrés.** Il n'y a **pas de plan en cours** : le travail se
fait à la demande, un geste à la fois, en réaction à l'usage réel.

### Le jalon 7 — ce qu'il change, et la contrainte qui l'a décidé

Plan : `docs/superpowers/plans/2026-08-14-ssh-os-m7-dolphin.md`. Neuf tâches,
120 mutations pour le jalon, **21 de plus** pour le lot souris qui a suivi.

**LA SOURIS D'ABORD — c'est la règle du projet, et elle a dû être redite.** Le
jalon a d'abord été livré au clavier ; l'utilisateur a demandé « **toutes** les
fonctions au bouton droit ». Le tableau ci-dessous se lit donc dans cet ordre : le
geste souris est le chemin principal, le raccourci en est le doublon.

| À la souris | Au clavier | Ce que ça fait |
|---|---|---|
| **Bouton droit, n'importe où dans le panneau** | — | **Le menu contextuel : 18 entrées**, chacune avec son raccourci écrit en face. Il s'ouvre aussi sur le vide et sur la ligne d'état — c'est justement là qu'on veut « Nouveau dossier » ou « Coller » |
| **Glisser un fichier vers l'autre panneau, ou sur un dossier** | — | **Le déplace.** Un clic n'est pas un glissement : c'est le mouvement qui décide |
| Clic sur le liseré de gauche | `F9` | Les raccourcis — Racine, Maison, Temporaire, Etc |
| Clic sur l'en-tête d'une colonne | — | Trie par nom, taille ou date ; recliquer inverse |
| Clic sur un segment du fil d'Ariane | `Alt+←` / `Alt+→` | Y monter ; l'historique au clavier |
| `Ctrl`+clic, `Maj`+clic | `Espace`, `Ctrl+A`, `Maj+flèches` | La sélection multiple |
| Clic, puis re-clic sur la même ligne | `Entrée` | Ouvrir — un dossier se descend, **un fichier s'ouvre dans l'Éditeur** |
| — | `F3` | Scinde en deux panneaux indépendants ; `Tab` passe de l'un à l'autre |
| — | `F7` / `Maj+F7` | Un dossier, un fichier vide |
| — | `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copier, couper, coller — destination : le panneau qui a la main |
| — | `F2`, `Suppr` | Renommer, supprimer (une seule question pour toute la sélection) |

**LA CONTRAINTE QUI DÉCIDE DE TOUT :** le démon est mono-thread. Copier deux
gigaoctets d'un `read`/`write` en boucle gèlerait toutes les fenêtres et tous
les clients pendant la copie. `CopyJob` avance donc **par tranches d'un
mégaoctet**, une par réveil, et l'arborescence est parcourue **paresseusement**
— un `readdir()` quand on y arrive, jamais un parcours complet avant de
commencer. Mesuré à la sonde : **39 ms** de temps de réponse du bureau pendant
la copie de 6 Mo.

`App` gagne `on_refresh()` au passage : le travail périodique **hors du rendu**,
puisque `render()` ne doit toucher ni au disque ni au réseau. Et une fenêtre
réduite qui a demandé l'horloge la garde — une application ne la demande pas
pour dessiner, elle la demande pour travailler.

**Deux canaux ont dû être ouverts dans le bureau pour que tout cela existe :**

- **`Host::open_app()`** (`src/app/app.hpp`, `src/daemon/host.cpp`) — une
  application ne sait pas ouvrir de fenêtre, mais elle sait fabriquer une autre
  application. La session la prend **au tour suivant** : ouvrir au milieu du
  traitement d'un clic ferait bouger la pile sous les pieds de celui qui l'a
  demandé. C'est par là que Fichiers ouvre l'Éditeur.
- **La prise de souris** (`Session::mouse_grab_`) — un appui dans le corps d'une
  fenêtre lui donne aussi les **mouvements** et le **relâchement**. Avant, le
  bureau ne livrait **que des appuis**, et le glisser-déposer était
  structurellement impossible. Mais pas les appuis suivants : le terminal n'est
  pas fiable sur les relâchements, et une prise qui ne se rendrait jamais rendrait
  tout le bureau incliquable.

**Ce qui reste ouvert** (la table complète est au §7.4) **:**

1. **Un vrai menu contextuel sur la barre des tâches.** Le clic droit y agit
   aujourd'hui directement — nouvelle instance — sans rien proposer.
   ⚠️ À ne pas confondre : le **gestionnaire de fichiers** a le sien depuis le
   15 août, avec 18 entrées.
2. **Le semis de points du fond d'écran.** Il fonctionnait, mais rendait chaque
   repeint complet **27 % plus gros** (mesuré), ce qui faisait basculer
   `daemon_dirty_overflow_closes_the_connection` du rejet *Dirty* au rejet
   *Clean*. Retiré ; le commentaire qui dit pourquoi est resté à l'endroit où il
   se rebrancherait.
3. **Rétention mémoire par connexion** (~41 Mio) et **le fuseau horaire du
   démon plutôt que celui du client** : §7.2, inchangés depuis le jalon 1.

**Soldé les 14 et 15 août 2026 :** l'assistance à l'ancrage (elle donne enfin un
appelant à `snap_opposite()`), le caret, le SIGKILL au groupe à la fermeture, la
documentation des gestes sans accord, les taquets de tabulation `HTS`/`TBC`, le
menu contextuel du gestionnaire de fichiers, **l'Éditeur enfin branché** — le
message « l'editeur arrive au jalon 6 » traînait depuis *avant* la livraison du
jalon 6 — le glisser-déposer, et la prise de souris qui le rendait possible.

Ce que les sept jalons ont appris, et qui vaut pour la suite :

1. **Du code né sans appelant ne se signale qu'en faisant tourner le vrai
   logiciel — ou en le cherchant exprès. Dix fois à ce jour.** La liste, le
   script de balayage et ses trois pièges sont au **§9 bis**, qui est la section
   la plus rentable de ce dossier.
2. **Le plan liste les fichiers neufs, pas ceux qu'il faut brancher.** Quatre
   tâches du jalon 3 ont débordé de leur périmètre annoncé, toujours pour cette
   raison. Le prévoir en écrivant le plan du jalon 4.
3. **Une mutation survivante est presque toujours un trou de test, pas une
   équivalence.** Sur 246 mutations jouées au jalon 3, 2 seulement étaient
   réellement équivalentes ; les autres ont chacune montré un cas que les tests
   n'atteignaient pas. La campagne des onglets (43 mutations, août 2026) a même
   fait mieux qu'un trou de test : elle a montré un **défaut de production** que
   964 cas laissaient passer — la réponse à un `CSI 6 n` reçu d'un onglet de fond
   partait sur le maître de l'onglet regardé.
4. **Le code écrit avant ses tests se paie, et c'est chiffré.** La seule tâche du
   jalon 4 écrite code d'abord a laissé **14 survivantes sur 33** au premier tour,
   contre 8/23, 7/23 et 4/26 pour les trois autres, écrites en TDD.
