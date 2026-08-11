# ssh_os 2.0 — dossier de reprise

> Document destiné à un contexte neuf. Il suppose zéro connaissance préalable de la
> conversation qui a produit le jalon 1. Tout ce qui suit a été vérifié, pas supposé :
> quand un fait vient d'une mesure, la mesure est citée.
>
> **Dernière mise à jour :** au commit `ebd79d8`, branche `m1-noyau`, 186 tests au vert.

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

# Release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"

# Debug (ASan + UBSan)
cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dbg -j"$(nproc)"
```

```bash
./build-release/sshos              # lance le bureau (démarre le démon si besoin)
./build-release/sshos --status     # « demon actif (pid N) » ou « aucun demon »
./build-release/sshos --kill       # arrête le démon
./build-release/sshos --daemon     # démarre le démon sans s'attacher
```

```bash
./build-release/sshos_tests                    # rapide
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build-dbg/sshos_tests   # armé
./build-dbg/sshos_tests diff                   # filtre par sous-chaîne du nom
```

**Attendu : `186 cas, 0 en echec, 0 assertions echouees`,** en Release comme en Debug,
avec 0 avertissement de compilation.

> Le binaire de test s'appelle **`sshos_tests`** (pas `sshos-test`). Erreur commise plusieurs
> fois.

### Vérifier la persistance à la main

1. `./build-release/sshos`, cliquer quelques fois dans la boîte (le compteur monte).
2. **Fermer la fenêtre du terminal.** Il n'y a pas de raccourci de détachement :
   fermer *est* le détachement.
3. Relancer `./build-release/sshos` → le compteur et l'horloge sont intacts.

Vérifié de bout en bout sous pty au commit `ebd79d8` : 3 clics injectés, terminal tué par
`kill -9`, `--status` répond « demon actif », client neuf affiche `clics: 3`.

---

## 3. Où en est le projet dans la feuille de route

Source : `docs/superpowers/specs/2026-08-10-ssh-os-design.md` §15.

| Jalon | Contenu | Sortie visible | État |
|---|---|---|---|
| **1** | Rendu, diff, protocole, client, démon | Une boîte colorée à l'écran, à travers SSH | ✅ **livré** |
| **2** | WM, panneau, menu, application factice | Tout le geste testable sans PTY | ⬜ **à faire, plan non écrit** |
| 3 | Terminal | *Le projet devient utilisable pour de vrai* | ⬜ |
| 4 | Gestionnaire de fichiers | | ⬜ |
| 5 | Moniteur système | | ⬜ |
| 6 | Éditeur | | ⬜ |

Volume total estimé : 12 000 à 15 000 lignes. Le jalon 1 en représente **10 287** (52
fichiers `src/` + `tests/`), soit l'essentiel de la plomberie.

> **Point d'attention relationnel.** À la livraison du jalon 1, l'utilisateur a été déçu :
> « aucun changement sur l'app c'est encore que des click de souris qui augmente ». C'est
> attendu et conforme — le compteur de clics est le *mouchard* du jalon 1, pas
> l'application : la plus petite chose prouvant qu'une entrée souris traverse le parseur,
> modifie l'état du démon, repasse par le diffeur et survit à la mort du terminal. Mais
> **il faut le dire avant, pas après.** Le jalon 2 est celui qui produit du visible.

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

Mesures de référence : coût nominal négligeable (7,02 / 7,08 / 6,97 s sans garde contre
7,11 / 7,18 / 7,04 s avec) ; un `SIGSEGV` injecté coûtait 38 s avant l'optimisation par
tranches, **7 380 ms après**. Deux cas expirent volontairement ; le cas légitime le plus
lent prend **4 295 ms**.

---

## 6. Ce que contient le jalon 1

13 tâches, toutes livrées, relues et fusionnées. **70 commits** depuis `main`.

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

### 7.1 — Round `EPOLLHUP` / drainage : **correctif écrit, jamais compilé** ⚠️

**Le seul élément vraiment inachevé du jalon 1.**

- **Symptôme :** si l'on clique et que l'on ferme le terminal dans la même fraction de
  seconde, les tout derniers messages sont perdus.
- **Cause :** `src/daemon/daemon.cpp:337-340` honore `EPOLLHUP|EPOLLERR` puis fait `continue`
  **avant** de drainer `EPOLLIN`. Le noyau coalesce `EPOLLIN|EPOLLHUP` en un seul réveil :
  les octets déjà arrivés partent à la poubelle. Même motif sur la branche `pending`
  (l. 399).
- **Correctif rédigé** (jugé complet côté production, +51/−20) : dans les deux branches, le
  bloc `EPOLLHUP|EPOLLERR`+`continue` est **retiré du haut** et remplacé, **après** le bloc
  `EPOLLIN`, par :

  ```cpp
  bool closed = false;                      // déclaré avant le bloc EPOLLIN
  if ((events & EPOLLIN) != 0) { closed = drain_socket(*client); ... }
  if (closed || (events & (EPOLLHUP | EPOLLERR)) != 0) { drop_client(nullptr); continue; }
  ```

  Le `||` garantit **un seul** `drop_*` par fermeture ; le test `client->dec.failed()` reste
  après le drainage et reste atteignable ; `session.wants_quit()` reste dans le bloc
  `EPOLLIN`.
- **Où il se trouve :** figé dans `.superpowers/sdd/2026-08-10-ssh-os-m1-noyau/hup-drain-partial.diff`
  (119 lignes, sha256 `5f339d9a023a39b5…`), et dans le worktree
  `.claude/worktrees/agent-aba4275accf38f581` (base `af36d6d`, non commité). Le worktree
  peut avoir été nettoyé ; le `.diff` fait foi.
- **Pourquoi c'est inachevé :** l'agent est mort sur `API Error: ConnectionRefused` juste
  avant de compiler.
- **Reste à faire :** compiler ; écrire le **test discriminant** (avec la sortie d'échec
  exacte contre le code d'avant) ; **prouver par la mesure** l'absence de boucle active à
  100 % CPU sur HUP ; `sha256sum` de restauration ; 20 exécutions Debug + 20 Release ;
  commit `fix(daemon): …` ; fusion.
- **Brief détaillé :** `.superpowers/sdd/2026-08-10-ssh-os-m1-noyau/hup-drain-brief.md`.

### 7.2 — Points reportés au jalon 2

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

## 8. Méthode de travail

L'utilisateur a choisi le **mode 1** : *un sous-agent frais par tâche, revue entre chaque*
(SDD — subagent-driven development).

- **Ledger :** `.superpowers/sdd/2026-08-10-ssh-os-m1-noyau/progress.md`.
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

## 10. Prochaine étape

**Écrire le plan du jalon 2 : « WM, panneau, menu, application factice ».** Sortie visible
attendue : *tout le geste testable sans PTY* — fenêtres déplaçables et redimensionnables,
barre des tâches dockable en bas **et** à gauche, menu, applications épinglables.

Le jalon 2 s'appuie intégralement sur le jalon 1 : les fenêtres seront des `View` sur la
`Surface`, le panneau un consommateur du même diffeur, les gestes des `MouseEvent` déjà
parsés. C'est donc surtout du dessin et de la logique de geste, pas de la plomberie.

Avant de commencer, décider si l'on solde d'abord le round `EPOLLHUP` (§7.1) — il est court,
le correctif est écrit, et il concerne précisément la fonctionnalité que l'utilisateur vient
de tester.
