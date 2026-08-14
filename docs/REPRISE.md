# ssh_os 2.0 — dossier de reprise

> Document destiné à un contexte neuf. Il suppose zéro connaissance préalable de la
> conversation qui a produit le jalon 1. Tout ce qui suit a été vérifié, pas supposé :
> quand un fait vient d'une mesure, la mesure est citée.
>
> **Dernière mise à jour :** 14 août 2026, branche `m1-noyau`, **1010 tests au vert**
> en `Release` comme sous ASan/UBSan.
> **Les six jalons sont livrés**, et le travail qui a suivi est demandé au fil de
> l'eau par l'utilisateur. Le §3 ci-dessous donne la position exacte.
>
> ⚠️ Les §4 à §9 (contraintes, harnais, décisions, pièges d'environnement) ont été
> écrits au jalon 1 et restent **entièrement valides** — ils ne parlent pas
> d'avancement. Le §6, en revanche, ne décrit que le contenu du jalon 1 : pour les
> jalons 2 à 6, la source de vérité est leur plan respectif dans
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

**Attendu : `189 cas, 0 en echec, 0 assertions echouees`,** en Release comme en Debug,
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
| **2** | WM, panneau, menu, application factice | Tout le geste testable sans PTY | ✅ **livré** |
| **3** | Terminal | *Le projet devient utilisable pour de vrai* | ✅ **livré** |
| **4** | Gestionnaire de fichiers | Naviguer, renommer, supprimer | ✅ **livré** |
| **5** | Moniteur système | Processus, CPU, mémoire, réseau | ✅ **livré** |
| **6** | Éditeur | Ouvrir, modifier, enregistrer | ✅ **livré** |

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

## 10. Prochaine étape

**Les six jalons sont livrés et la v1 est complète.** Le travail se fait désormais
à la demande, un geste à la fois.

**Ce qui reste ouvert :**

1. **Un vrai menu contextuel** sur le clic droit de la barre des tâches : il agit
   aujourd'hui directement (nouvelle instance), sans rien proposer.
2. **Le semis de points du fond d'écran.** Il fonctionnait, mais rendait chaque
   repeint complet **27 % plus gros** (mesuré), ce qui faisait basculer
   `daemon_dirty_overflow_closes_the_connection` du rejet *Dirty* au rejet
   *Clean*. Retiré ; le commentaire qui dit pourquoi est resté à l'endroit où il
   se rebrancherait.
3. **Rétention mémoire par connexion** (~41 Mio) et **le fuseau horaire du
   démon plutôt que celui du client** : §7.2, inchangés depuis le jalon 1.

**Soldé le 14 août 2026 :** l'assistance à l'ancrage (elle donne enfin un
appelant à `snap_opposite()`), le caret, le SIGKILL au groupe à la fermeture, la
documentation des gestes sans accord, et trois méthodes sans lecteur.

Ce que les six jalons ont appris, et qui vaut pour la suite :

1. **Une méthode née sans appelant ne se signale qu'en faisant tourner le vrai
   logiciel — ou en la cherchant exprès.** **Sept fois** à ce jour :
   `Decoder::failed()`, la garde `A2`, `InputParser::timeout()` (qui rendait
   `vim` inutilisable), `App::refresh_ms()`, `App::wants_cursor()` (aucun caret
   nulle part dans le bureau), `Pty::kill_now()` et `snap_opposite()`.

   **Le balayage systématique marche et coûte deux minutes** : lister les
   fonctions déclarées dans `src/**.hpp`, compter leurs appels dans **tout**
   `src/`. Il a sorti `App::wants_cursor()`, `Pty::kill_now()`,
   `snap_opposite()`, la famille `set_tab()`/`clear_tab()`/`clear_all_tabs()`
   — d'où `ESC H` et `CSI g` n'étaient pas traités, et `tabs -4` ne faisait
   rien — puis `display_label()`, dupliquée sans qu'on s'en aperçoive.

   **Trois pièges du script, tous rencontrés :** ne lire que les `.cpp` rate
   les appels faits depuis les définitions en ligne des `.hpp` ; une borne
   `[^\w:]` devant le nom exclut tout appel qualifié `Classe::methode()` et
   fait passer la moitié du projet pour orpheline ; un `head -N` sur le
   `grep` de vérification fait passer une méthode appelée pour une orpheline.
   Les accesseurs `*_for_tests` sont des faux positifs légitimes — d'où le
   suffixe, à mettre systématiquement.

   Après la passe : **14 candidates sur 263**, toutes vérifiées à la main,
   toutes légitimes (API de test, ou `set_cloexec()` dont le commentaire dit
   maintenant pourquoi personne ne l'appelle).
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
