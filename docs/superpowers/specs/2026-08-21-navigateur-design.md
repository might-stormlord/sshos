# termos — un navigateur web écrit à la main

> **Document maître du chantier.** Écrit pour un contexte neuf : il suppose zéro
> connaissance de la conversation qui l'a produit. Tout chiffre cité vient d'une mesure
> faite sur ce dépôt, et la mesure est nommée.
>
> **État au 21 août 2026**, branche `m1-noyau`, commit `0d4f513`. Le chantier n'a **pas
> commencé** : aucune ligne de `src/web/` n'existe. Ce document ferme les décisions
> transverses ; il ne spécifie ni le DOM, ni CSS, ni JS, ni TLS en détail — chacun aura
> sa propre spec de jalon.
>
> **Chiffres du dépôt, recomptés le 21 août** : 281 commits, 1319 cas de test, 126
> fichiers dans `src/`, version 1.45, 19 800 lignes dans `src/`, 29 167 dans `tests/`.
> ⚠️ Ces nombres périment à chaque commit — les recompter, jamais les citer. Les quatre
> commandes sont en tête de `docs/REPRISE.md`.

---

## 1. Ce qu'on construit, et pourquoi

Un **navigateur web** pour le bureau `termos`, supportant les standards récents, et qui
**convertit les images pour conserver l'apparence d'origine** dans un terminal.

Deux cadrages ont été posés d'emblée et acceptés par l'utilisateur.

**Le rendu est hybride.** Rendre toute la page en pixels ne marche pas : un terminal de
200×50 cellules ne fait que 200×100 « pixels » en demi-blocs, et un paragraphe y devient
illisible. Le texte sort donc en **glyphes de terminal**, et seules les **boîtes
remplacées** (`<img>`, `<svg>`, `<canvas>`, affiche de `<video>`) passent par la
conversion pixel→cellule.

**L'échelle est celle d'un moteur, pas d'une application.** L'utilisateur l'a tranché :
*« nous ferons le code qu'il faut et ça prendra le temps que ça prendra »*. Le calendrier
n'est pas un critère de conception. L'ordre des travaux, si — et chaque jalon doit être
démontrable à l'écran ou par une mesure.

---

## 2. Ce qui n'est pas négociable

Repris du §4 de `docs/REPRISE.md`, plus ce que ce chantier y ajoute.

| Contrainte | Ce qu'elle interdit **ici** |
|---|---|
| C++20, `-Wall -Wextra -Wpedantic -Werror`, Debug sous ASan+UBSan | Voir §4 : deux extensions GNU vont mordre |
| **Zéro dépendance externe** | TLS, crypto, `inflate`, Brotli, PNG, JPEG, WebP, JS : **tout à la main**. Voir §3, décision 2 |
| **`CMakeLists.txt` : pas de logique ajoutée** | Pas de cible `install()`, pas de `configure_file`, pas de `-D`. Renommer une cible n'en ajoute aucune — fait le 21 août (`2e622b2`). Le C++ ne doit jamais dépendre de ce que CMake sait |
| **Un thread, un `epoll`, aucun mutex** *dans le démon* | Contourné par la décision 3 : le moteur vit ailleurs et **a le droit de bloquer** |
| `render()` ne touche ni au disque ni au réseau | Vaut pour l'`App` mince, pas pour le moteur |
| `\033` jamais `\e` | Une occurrence casse la compilation du projet entier |
| Commentaires en **français accentué**, identifiants en **anglais** | `PascalCase` types, `snake_case` méthodes, `snake_case_` membres, `kPascalCase` constantes, `enum class` systématique, API de test suffixées `_for_tests` |
| Messages de commit en français **sans accents** | |
| **La souris d'abord** | Toute fonction du navigateur doit être atteignable sans raccourci clavier. Le motif maison est le menu au bouton droit avec les raccourcis affichés en face |
| TDD strict : **rouge constaté**, campagne de mutation par tâche, un cas par survivante, un commit par tâche | S'applique intégralement, avec deux adaptations honnêtes — §15 |

### Quatre contraintes mécaniques, vérifiées sur ce dépôt

| | |
|---|---|
| `file(GLOB_RECURSE … src/*.cpp)` est **récursif** ; `file(GLOB … tests/test_*.cpp)` **ne l'est pas** | ⚠️ **Aucun `tests/web/`.** Tous les tests à plat : `tests/test_web_*.cpp`. Un `tests/web/test_x.cpp` serait ignoré **en silence**, la suite resterait verte, et le test n'existerait pas |
| Le glob ne prend que `.cpp` | Aucun `.c`, jamais |
| Tout `src/**/*.cpp` entre dans `sshos_core`, donc **le moteur est lié dans le démon** | ⚠️ **Aucun constructeur global non trivial dans `src/web/`** — il s'exécuterait au démarrage du bureau. Tables en `inline constexpr` ou statiques locales de fonction. Vérifiable : `nm libsshos_core.a \| grep _GLOBAL__sub_I_ \| grep web` |
| `src/main.cpp` est **hors** de `sshos_core` | Rien de ce qui s'y trouve n'est testable. Le point d'entrée du moteur est une **fonction** de `sshos_core`, appelée par une ligne de `main.cpp` — et un second `main()` casserait l'édition de liens |

⚠️ **`main.cpp` appelle `current_socket_name()` en première instruction, et `read_boot_id()`
lève.** La branche `--web-renderer` doit être placée **avant** ce bloc (l. 71-77 au commit
`0d4f513`). Le moteur n'a que faire du socket du bureau ; l'y soumettre lui donnerait un
mode d'échec sans rapport, dans les environnements mêmes que le §2 de `REPRISE.md`
signale — un conteneur restreint.

---

## 3. Les quatre décisions verrouillées

Prises par l'utilisateur, options et coûts présentés. Elles ne se rouvrent pas sans
raison neuve.

### 3.1 Images en demi-blocs, couleur vraie

Glyphe **`U+2580 ▀`**, avant-plan = pixel du haut, arrière-plan = pixel du bas, 24 bits
chacun. Une cellule porte deux pixels indépendants.

*Pourquoi celle-là.* C'est la seule technique qui tienne la promesse « conserver
l'apparence d'origine ». L'ASCII de densité (` .:-=+*#%@`) ne garde que la silhouette. Les
octants Unicode 16 donnent quatre fois la résolution mais **une seule paire de couleurs
par cellule**, et dépendent d'une police que peu de terminaux ont.

*Ce que le dépôt offre déjà.* `Cell` (`src/render/cell.hpp:84`) porte `fg` et `bg` RGB
**indépendants**, et `char_width(U'▀')` rend 1. **Le cœur de rendu ne change pas d'une
ligne** — voir §11 pour la nuance sur la largeur.

### 3.2 Zéro ligne de code empruntée

TLS 1.3, X.509/ASN.1-DER, la crypto (AES-GCM, ChaCha20-Poly1305, SHA-2, HMAC, HKDF,
ECDSA P-256, Ed25519, X25519, RSA-PSS), `inflate`/DEFLATE, Brotli, PNG, JPEG, WebP, et le
moteur JavaScript : **tout est écrit à la main**.

*Le risque a été présenté et accepté.* Une pile crypto maison est le seul sous-système
d'un navigateur où un défaut se paie en **compromission à distance** plutôt qu'en pixels
moches. La conception porte donc les disciplines qui le rendent défendable (§15), et
l'énoncé honnête est écrit au §16.

*Nuance qui n'est pas une entorse* : les **vecteurs de test** sont de la donnée, pas de
l'implémentation. Un fichier de référence versionné dans `tests/vectors/` a le même statut
que `tests/golden/`.

### 3.3 Le moteur tourne dans son propre processus

`termos --web-renderer`, enfant du démon, relié par un `socketpair`. L'`App` Navigateur
dans le démon reste mince : elle envoie URL et entrées, elle reçoit une grille de cellules.

*Pourquoi.* Le démon a **33 ms par trame** (`kFrameIntervalMs`, `src/daemon/daemon.cpp`) et
aucun mutex. Dans le démon, chaque algorithme du moteur — tokenizer, cascade, mise en
page, décodage d'image — devrait être **reprenable** et avancer d'une tranche par réveil.
Hors du démon, le moteur a le droit de bloquer. Et un défaut de parseur sur une page
hostile coûte un onglet, pas la session avec ses terminaux ouverts.

*Bénéfice non évident* : le démon ne pourrait pas faire de **DNS** du tout — `getaddrinfo`
bloque. La séparation en processus n'est pas une précaution, c'est ce qui rend le réseau
possible.

### 3.4 JavaScript dès la première ligne du DOM

*Pourquoi.* Greffer un moteur JS sur un DOM conçu sans lui oblige souvent à le réécrire.
C'est arrivé à plus d'un moteur. Le DOM naît donc scriptable.

*Conséquence assumée.* **Le premier jalon ne peut plus être « une page à l'écran ».**
L'ordre naïf (transport → HTML → CSS → mise en page → peinture → images → JS) ne tient
plus, et la feuille de route se réordonne autour de ça — §13.

---

## 4. Les deux sondes de compilation, mesurées

Lancées le 21 août 2026, `g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`, sous
`-std=c++20 -Wall -Wextra -Wpedantic -Werror`, dans `/var/tmp` (jamais `/tmp` : tmpfs de
2,7 Go).

| Sonde | Verdict | Conséquence |
|---|---|---|
| Littéral chaîne de 70 000 caractères | **ACCEPTÉ** | `-Wpedantic` ne déclenche pas `-Woverlength-strings` en C++ ici. Les corpus de vecteurs **peuvent** tenir en `R"(...)"`. La contrainte redoutée n'existe pas |
| `unsigned __int128` nu | **REFUSÉ** — `error: ISO C++ does not support '__int128' [-Werror=pedantic]` | Même mécanisme que `\e` : une occurrence casse tout |
| `__extension__ typedef unsigned __int128 u128;` | **ACCEPTÉ** | **Le repli 32 bits n'est pas obligatoire** |

**Décision qui en découle**, à écrire dans `src/web/crypto/bignum.hpp` :

```cpp
// `__int128` est une extension GNU : sous -Wpedantic -Werror, l'écrire nu casse la
// compilation du projet entier (même mécanisme que `\e`, cf. §4 de REPRISE.md).
// `__extension__` est l'échappatoire prévue, et elle est confinée à ce seul typedef --
// c'est ce qui la rend acceptable ici alors que `\e` ne l'est nulle part : une
// occurrence, un fichier, un commentaire.
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 Wide;   // limbes 64 -> produit 128
#endif
```

Et le **chemin 32 bits reste écrit**, non comme un mode dégradé, mais comme
l'**implémentation scolaire de référence du fuzzing différentiel** (§15). Une seule
écriture sert la portabilité hors x86-64 **et** l'oracle qui n'existe pas autrement.

---

## 5. La carte des modules, et la règle de couches

`src/web/` est au Navigateur ce que `src/vt/` + `src/pty/` sont au Terminal. Mettre le
moteur sous `src/apps/` ferait du seul répertoire dont la règle dit qu'il ne connaît que
`render/` et `app/` le plus gros sous-système du projet.

```
src/web/
  wire/   protocole du socketpair          url/     parse, normalise, résout
  text/   encodages de caractères          codec/   inflate, gzip, brotli
  crypto/ primitives                       x509/    DER, certificat, chaîne, magasin
  tls/    TLS 1.3 client                   http/    DNS, TCP, HTTP/1.1, cookies, cache
  loader/ la couture de récupération       html/    tokeniseur suspendable + arbre
  dom/    arène, poignées, mutation        js/      le langage SEUL
  bind/   le pont DOM <-> JS               css/     tokeniseur, sélecteurs, cascade
  layout/ unités, boîtes, bloc, en-ligne   image/   décodeurs, rééchantillonnage
  paint/  liste d'affichage, peintre       engine/  navigation, interruption, budgets
  ui/     menu contextuel du moteur        renderer/ le processus enfant
src/apps/browser/   browser · urlbar · spawn · grid   <- l'App mince du démon
```

Le projet a déjà une règle de dépendance à sens unique : `render → rien`, `wm → render`,
`shell → wm, render`, `apps/* → render, app/`, `daemon → tout`. Le chantier y ajoute
`web/*`, et **trois arêtes interdites portent tout le poids** :

- **`web/js` ne connaît pas `web/dom`.** Le langage reste hermétique : `eval("1+2")` se
  teste sans arbre, sans réseau, sans processus. Le pont est `web/bind`, seul à inclure
  les deux.
- **`web/dom` ne connaît pas `web/html`.** Le constructeur d'arbre écrit dans le DOM **par
  la même porte que JavaScript** : `Mutation`. C'est ce qui rend « scriptable dès la
  première ligne » *structurel* au lieu d'aspirationnel — il n'existe pas d'autre porte à
  oublier de brancher.
- **`apps/browser` ne connaît pas `web/engine`.** L'application du démon ne peut pas
  appeler le moteur par mégarde ; le seul vocabulaire partagé est `web/wire`.

**Garde mécanique** : `tools/couches.py`, frère de `tools/balayage.py` — grep des
`#include "web/X/…"` par fichier, table des couches, `--strict` sort non nul. `tools/`
n'est pas globé par CMake, l'ajouter ne coûte rien.

---

## 6. Le protocole `App` ↔ moteur

### 6.1 Un second protocole, pas une extension de `Msg`

**On n'ajoute rien à `sshos::Msg`.** Quatre raisons, chacune vérifiable dans l'existant :

1. **Le versionnement porterait à faux.** `kBuildId` est l'axe de compatibilité
   **client ↔ démon** ; le client attaché peut être plus ancien que le démon (§2 quater de
   `REPRISE.md`). Le moteur, lui, est **toujours le même binaire** que le démon qui le
   lance. Partager `kBuildId` obligerait à l'incrémenter à chaque évolution *web*, donc à
   répondre `Incompatible` à des clients que ça ne concerne pas.
2. **Le plafond ne se transporte pas.** `kMaxMessageBytes = 32 Mio` est dimensionné sur un
   pire cas explicite et daté : un repaint 500×500 truecolor. Le pire cas du lien moteur
   est un autre objet.
3. **Le modèle de menace est inversé.** Sur le lien client, `Decoder` parse un pair
   authentifié par `SO_PEERCRED`. Sur le lien moteur, le démon parse des octets produits
   par **le seul processus du système qui vient de digérer du HTML, du TLS et du JPEG
   venus d'Internet**. C'est la frontière la plus sensible du projet.
4. **Une divergence de règle est voulue.** `proto.cpp` **saute** un tag inconnu, par
   compatibilité ascendante. Sur le lien moteur, un tag inconnu veut dire « ce processus
   n'est pas celui que j'ai lancé » : `web::WireDecoder` **échoue**. ⚠️ Cette inversion
   **doit porter son commentaire en face**, sinon un relecteur « corrigera » vers
   `proto.cpp` en croyant réparer une incohérence.

⚠️ **Ne jamais ajouter de variante à `sshos::Msg`.** Le `std::visit` exhaustif de
`Session::on_input` est le garde permanent du projet (§9 bis de `REPRISE.md`).

### 6.2 Une extraction préalable — la toute première tâche

`src/common/proto.cpp` contient, dans son espace anonyme, les primitives d'octets et
**l'arithmétique d'enveloppe** de `Decoder::next()` — celle qui a déjà coûté deux
correctifs (le calcul en 32 bits qui bouclait près de `0xFFFFFFFF`, la queue d'octets
avalée par l'ancien `buf_.erase`). Le commentaire de `kHeaderSize` interdit lui-même la
solution paresseuse : *« deux copies entretenues à la main auraient pu diverger sans
qu'aucun avertissement du compilateur ne le signale »*.

On extrait donc, plutôt que d'écrire deux fois :

- `src/common/wirebuf.hpp` — `ByteSink` / `ByteReader`, déplacement pur
- `src/common/framing.hpp` / `.cpp` — `FrameReader{FrameLimits}`, l'enveloppe seule

`sshos::Decoder` devient une enveloppe mince ; sa surface publique ne bouge pas d'un
caractère, et **`tests/test_proto.cpp` (486 lignes) est le filet du refactor**. Le rouge se
constate en cassant volontairement `FrameReader::compact()` avant de le brancher.

### 6.3 Ce qui traverse : une grille complète, jamais un diff

Compressée par plages (`Style` / `Run` / `Text` / `Row`).

*Pourquoi pas un différentiel côté moteur.* Le démon possède déjà **le seul diffeur du
projet** (`src/render/diff.cpp`). Un second étage différentiel serait un **état à garder
synchronisé de part et d'autre d'une frontière faillible** — la forme exacte du seul gel
silencieux et définitif que le projet ait rencontré (`OutQueue::Overflow::Dirty`). Une
grille pleine perdue est réparée par la suivante ; un diff perdu ne se répare jamais.

**Plafonds recalculés, pas copiés** : `kMaxViewportCols/Rows = 500`, pire cas 20 o/cellule
→ `web::kMaxMessageBytes = 8 Mio`. Quatre fois **sous** le plafond du lien client — le pair
le moins fiable a le plafond le plus serré. Au passage, ces deux bornes ferment un trou que
`proto.hpp` documente sans le combler (« aucune borne sur cols/rows elles-mêmes »).

**La cellule du fil porte `{ch, fg, bg, attrs}` — pas `width`.** `View::put` recalcule la
largeur par `char_width` et maintient l'invariant tête/continuation
(`src/render/surface.cpp:70-101`). Un octet de moins, et surtout **une surface de confiance
de moins** : un moteur compromis ne peut pas corrompre la grille du bureau. Le `ch` reçu est
validé au décodage (substitut `U+FFFD` sur surrogate ou > `0x10FFFF`).

### 6.4 Contre-pression : un crédit d'une trame

Le danger précis : le moteur repeint plus vite que le lien SSH n'absorbe → l'`OutQueue` du
**client** (plafond 1 Mo) déborde → l'utilisateur perd un repaint (`Clean`) **ou toute sa
session** (`Dirty` → fermeture).

Mécanisme : le moteur ne peut avoir qu'un `Grid` non acquitté par onglet. Et **le crédit
n'est pas émis à la réception, mais après que le démon a composé une trame le contenant** :

```
WebApp::on_io()   -> décode Grid{seq}, range, host_->invalidate()
WebApp::render(V) -> blit ; pose ack_due_ = seq   (render n'écrit RIEN)
WebApp::flush_out() -> émet Credit{seq}
```

La cadence se cale alors automatiquement sur le plus lent des trois : le moteur, le
`FrameClock` à 33 ms, et le débit réel du lien. Aucun réglage neuf, aucune minuterie neuve,
aucun mutex.

⚠️ **Piège du minimisé** : `App::refresh_ms()` n'est honoré que pour les fenêtres
**visibles**. Fenêtre minimisée → pas de crédit → moteur bloqué à vie. Parade : sans
crédit, le moteur **compose quand même et garde sa dernière grille**, qu'il émet dès le
crédit reçu. Une trame n'est jamais perdue, seulement supersédée.

### 6.5 Le chrome appartient à l'`App`, dans le démon

Barre d'URL, barre d'onglets, ligne d'état. Raison décisive : elles doivent rester vivantes
et **cliquables quand le moteur est mort ou bloqué**. Un bouton « Recharger » dessiné par le
processus qu'il doit relancer ne sert à rien. Le fil ne transporte que la zone de contenu, et
une page ne peut **structurellement** pas peindre par-dessus le chrome (`blit` dans
`v.sub(content_rect)`).

### 6.6 Un moteur par fenêtre, N onglets dedans

Pas un moteur par onglet. Un PTY est quasi gratuit ; un moteur avec sa pile TLS, son cache
DNS, son pot à biscuits et ses décodeurs ne l'est pas — et ces états **doivent** être
partagés entre onglets d'une même fenêtre. L'échappatoire d'isolation existe déjà et
l'utilisateur la connaît : le clic droit sur la barre des tâches ouvre une **nouvelle
instance**. Coût énoncé : une page hostile peut bloquer les autres onglets *de sa fenêtre*
— d'où la bande d'erreur qui **nomme l'onglet**. Plafond `kMaxRenderers = 8`.

---

## 7. Le lancement du moteur

**Patron : `launch_updater` (`src/daemon/session.cpp:116-164`), un `fork()` simple.**
⚠️ Surtout pas `spawn_detached` : son double `fork` rend le pid de l'intermédiaire, et un
enfant à surveiller doit être récoltable par le `waitpid(-1)` de `reap_children`.

Dans l'enfant, fonctions sûres vis-à-vis des signaux uniquement :

1. `sigprocmask(SIG_SETMASK, &empty)` — **le masque survit à `execve`**, et le démon bloque
   SIGTERM/INT/CHLD pour son `signalfd`
2. `sigaction(SIG_DFL)` sur SIGPIPE, HUP, TERM, INT, CHLD
3. **`drop_oom_protection()`** — le démon est à `oom_score_adj = -1000` et le réglage
   s'hérite ; puis le moteur se remonte à **+500** : quand la mémoire manque, c'est lui qui
   doit tomber en premier
4. `setsid()` — pour qu'un `SIGKILL` au groupe atteigne tout
5. `setrlimit` AS / NPROC=0 / FSIZE / CORE=0, puis `prctl(PR_SET_NO_NEW_PRIVS, 1)`
6. `dup2(sv[1], 3)` — **`dup2` efface `FD_CLOEXEC`**, ce qui est exactement l'effet voulu :
   tout le reste des descripteurs du démon naît `CLOEXEC` et disparaît seul à l'`execve`
7. 0/1/2 vers `/dev/null`, puis `execv`, puis `_exit(127)`

L'errno d'un `execve` raté remonte par un tube `CLOEXEC` que l'exec réussi referme tout seul
— motif déjà écrit et éprouvé dans `Pty::spawn`. Sans lui, un exec raté ne se découvre que
par une fenêtre vide.

**Bac à sable, en tâche séparée** : un filtre seccomp-BPF écrit à la main (~150 lignes,
jamais `libseccomp`, ce serait une dépendance). C'est **la mesure la plus rentable pour une
pile crypto maison** : elle transforme une corruption mémoire exploitée en mort de
processus. Conséquence architecturale, et elle est bonne : si `openat` est interdit après
l'armement, **pas de cache disque en v1** — cache mémoire borné, pot à biscuits sur un fd
ouvert au démarrage. Et un test doit **prouver que le filtre est armé** (tenter un appel
interdit, attendre `SIGSYS`), sans quoi le filtre est du code sans appelant.

### La mort du moteur

Trois morts, trois traitements, **aucun `waitpid`** (l'`App` passe par
`Host::watch_child` + `App::on_child_exit`) :

- **Crash / OOM / sortie.** Deux signaux arrivent dans un **ordre non garanti** :
  `on_child_exit` et l'`EOF`/`EPOLLHUP` sur le socketpair. Le traitement doit être
  idempotent. **La fenêtre reste, la dernière grille peinte reste**, et une bande d'une
  ligne apparaît avec `[Recharger]`. Pas d'écran blanc (indiscernable d'une page vide), pas
  de relance automatique. Le compte des relances est un `RestartBudget` de la même forme que
  `src/client/restart.hpp` : **il ne borne que le stérile** — leçon littérale du commit
  `6ab3a2e`.
- **Blocage.** Le moteur a le droit de bloquer : « pas de trame depuis 10 s » n'est pas une
  anomalie. Ce qui l'est, c'est **ne plus répondre aux gestes** — d'où un `in_seq` croissant
  et son écho dans `Status`. Sans écho depuis 10 s : bande `[Attendre] [Arrêter]`.
  **Jamais de tuerie automatique.**
- **Moteur félon.** `FromEngineDecoder::failed()` → `SIGKILL` au groupe, bande, ligne au
  journal. Tag inconnu compris.

Le moteur ouvre **son propre** `Journal` (`src/daemon/journal.hpp`, réutilisé tel quel) sur
`<données>/web-journal.log`, avec `arm_crash_note()`. Conséquence héritée de la philosophie
du journal : **une vie qui commence par une ligne et ne se termine par aucune est la
signature d'un SIGKILL** — exactement ce qu'on veut savoir après une mort par mémoire.

---

## 8. La couche terminal — unités et projection

### 8.1 Le point fixe, jamais le flottant

```cpp
// src/web/layout/unit.hpp
using LayoutUnit = int32_t;                 // 1/64 px CSS
inline constexpr LayoutUnit kPx = 64;
struct CellMetrics { LayoutUnit w = 8*kPx, h = 16*kPx; };  // 1:2, le ratio d'un terminal
```

Le projet a des références **bit-exactes** ; l'accumulation flottante rend la mise en page
non déterministe entre `-O2` et `-O0`. `int32_t` en 1/64 couvre ±33 M px.

Le **zoom** est le seul réglage : `CellMetrics` varie de `4×8` à `16×32`. Rien d'autre dans
la chaîne ne connaît le zoom.

Conséquence heureuse du choix des demi-blocs : une cellule porte 1×2 pixels d'image, donc
**8 px CSS par pixel d'image dans les deux axes** — carré. Une image 640×480 occupe 80
colonnes sur 30 lignes, une taille jouable.

### 8.2 `font-size` ne change pas l'avance

**Décision dure.** Un caractère avance de `char_width(cp) × kCellW`, quelle que soit la
taille. C'est la seule projection qui rende la mise en page en ligne exacte **et** le
hit-testing exact sur un média à chasse unique.

Ce que `font-size` pilote :

- **La hauteur de ligne**, arrondie **une fois par bloc**, jamais par ligne. Arrondir par
  ligne donnerait 1, 2, 1, 2, 1 rangées pour `line-height: 1.5` — un paragraphe qui respire
  irrégulièrement. **L'uniformité bat l'exactitude** ici.
- **Une échelle d'emphase** : ≥ 1,8× racine → `Bold` + filet `─` pleine largeur sous le
  bloc ; 1,2–1,8 → `Bold` ; 0,85–1,2 → normal ; < 0,85 → `Dim`. Plus `font-weight ≥ 600`
  → `Bold`, `italic` → `Italic`, `underline`/`line-through` → `Underline`/`Strike`. **Les
  huit bits d'`attr::` suffisent, aucun n'est à ajouter.**

### 8.3 Rasteriser les titres en demi-blocs : non

L'argument décisif n'est pas le coût, c'est que **la rasterisation ne peut pas honorer la
taille CSS demandée**. À `8×16`, un `<h1>` de 32 px dispose de 2 cellules d'avance et de
4 demi-rangées. Un glyphe lisible exige au minimum 5×7 pixels. Pour être lisible il
faudrait dessiner **plus gros que ce que le CSS dit** — la rasterisation trahit alors la
propriété qu'elle prétendait servir.

Le coût, pour solder la question : « Bonjour » en 5×7 par glyphe = 168 cellules ≈ **3 360
octets**, contre **19 octets** en glyphes (7 octets + un SGR Bold + un CUP). **Rapport
177×**, et le résultat n'est ni sélectionnable ni copiable.

La police de blocs 3×5 de `src/shell/sysinfo.cpp` reste ce qu'elle est : une signature
décorative de six lettres connues d'avance, pas un moteur de texte.

**DECDWL/DECDHL (`ESC # 3/4/5/6`) sont également rejetés** : ce sont des attributs de
**ligne d'écran entière**. La `Surface` n'a pas d'attribut de ligne, et le compositeur peut
avoir deux fenêtres côte à côte sur la même rangée — une ligne double-largeur casserait le
voisin.

### 8.4 On arrondit les ARÊTES, jamais les étendues

`x0 = arrondi(x/w)` et `x1 = arrondi((x+w)/w)`. Deux boîtes adjacentes partagent alors
**exactement** la même arête : ni recouvrement, ni trou. Arrondir position **et** taille
séparément produit un trou d'une cellule entre deux `<td>` adjacents ; arrondir les deux
arêtes ne le peut pas, par construction.

**Résidu vertical** en flux de blocs : les marges verticales fusionnent d'abord (vraie règle
CSS), *puis* on arrondit, *puis* le reste passe au frère suivant. Cinq marges de 6 px
= 30 px = 1,875 rangée donnent 0, 0, 1, 0, 1 — pas 5×0 ni 5×1.

**Plancher** : toute boîte de largeur CSS non nulle reçoit au moins 1 cellule, sinon un
séparateur de 3 px disparaît.

### 8.5 `border-width: 1px` — le budget de bordures

1 px vertical vaut 1/16 de rangée. Arrondi à 1 : **16× trop épais**. Arrondi à 0 : la
structure de la page disparaît. Décision :

1. Toute bordure non nulle et **visuellement porteuse** (couleur distincte du fond du
   parent) vaut exactement **1 cellule**, dessinée aux glyphes d'encadrement.
2. Elle est **prélevée sur le padding d'abord** (`box-sizing` forcé à `border-box` au moment
   de la projection seulement), pour ne pas pousser le contenu d'une rangée entière.
3. `BorderBudget` **fusionne** les bordures de boîtes imbriquées qui atterriraient sur la
   même arête de cellule ; la plus intérieure garde sa couleur.

Sans (3), quatre `<div>` imbriqués à 1 px consomment **8 rangées** sur les ~36 du viewport
— 22 % de la page perdus pour 8 px CSS.

`border-radius` : **pas de rasterisation.** Rayon ≥ une demi-cellule → coins
`╭ ╮ ╰ ╯` (`U+256D..U+2570`), sinon coins carrés. Rasteriser un coin ferait basculer toute
la boîte en îlot pixel et détruirait le texte à l'intérieur.

---

## 9. La frontière glyphe / demi-bloc

**Une règle, une seule :**

> Un **îlot pixel** n'est créé que par un **élément remplacé**. Tout le reste se résout en
> `(glyphe, fg, bg, attrs)`.

| Ce que c'est | Où ça passe |
|---|---|
| Texte, titres, listes, tableaux | **glyphes** |
| `background-color`, `rgba()` | **fond de cellule**, composé au peintre |
| `linear-gradient` / `radial-gradient` | **fond de cellule**, échantillonné au centre ; raffinement vertical en `▀` si aucun texte ne tombe sur la cellule |
| Bordures, `<hr>`, filets de tableau | **glyphes** `─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼`, repli `Border::Ascii` si `!profile.utf8` |
| `border-radius` | **glyphes**, ci-dessus |
| `box-shadow` | **abandonné** ; option : une bande `attr::Dim` d'une cellule. Le flou est ignoré |
| `opacity` | **résolu** dans la couleur de fond de cellule |
| `<img>`, `<canvas>`, `<video>` (poster), `<svg>` | **demi-blocs** |
| `::marker`, cases, radios | **glyphes** `• ▸ ☐ ☑ ○ ◉`, repli ASCII |

Le point clef, et la raison pour laquelle un dégradé ne devient **pas** un îlot : un îlot
pixel *avale* tout ce qui est peint dessus. Un `<h1>` sur un fond dégradé doit rester du
texte. Tant qu'un effet s'exprime comme « une couleur de fond par cellule », il reste sous
le texte et le texte reste du texte.

### Les grappes de graphèmes — et pourquoi `Cell::cluster` ne revient pas

`é` composé (`e` + `U+0301`) n'est pas représentable dans `Cell`
(`src/render/cell.hpp:76-83`, où le champ a été retiré et sa justification conservée).
Trois mesures :

1. **Normalisation NFC à la mise en forme**, table Latin + diacritiques courants. Couvre
   l'écrasante majorité des pages réelles.
2. Ce qui résiste : base rendue seule, marques combinantes déposées (`char_width` rend déjà
   0) ; repli `fold_to_ascii()` si `!profile.utf8`.
3. **La sélection et la copie lisent le texte SOURCE, jamais la grille** (§12).

Donc : **ne pas reposer `cluster`.** Le réservoir de grappes serait une **seconde copie** du
texte que la liste d'affichage détient déjà — exactement le motif que le retrait de ce champ
a soldé.

---

## 10. Le pipeline image

```
src/web/codec/inflate.*      RFC 1951, partagé par PNG et Content-Encoding: gzip
src/web/image/png.cpp        RFC 2083 : IHDR/PLTE/tRNS/IDAT/IEND, 5 filtres, Adam7
src/web/image/jpeg.cpp       JFIF baseline puis progressif, Huffman, IDCT entière
src/web/image/gif.cpp        LZW, palettes, GCE, 4 méthodes de disposition
src/web/image/bmp.cpp        BMP/ICO -- trivial, et c'est le format des favicons
```

Cible commune : `RgbaImage` en sRGB 8 bits, **alpha PRÉMULTIPLIÉ**.

**Trois décisions que presque tout le monde rate :**

1. **Prémultiplié, et c'est une décision.** Le filtre de boîte doit moyenner des valeurs
   prémultipliées, sinon la couleur arbitraire des pixels totalement transparents fuit dans
   les bords — le halo classique autour d'un logo PNG détouré.
2. **Rééchantillonnage en lumière linéaire, pas sur les octets sRGB.** Le chiffre qui
   tranche : un damier de noir pur et de blanc pur moyenné correctement vaut **188, pas
   128**. Moyenner les octets sRGB assombrit systématiquement toutes les textures fines.
   Deux tables (`kSrgbToLinear[256]`, `kLinearToSrgb[4096]`), **4,5 Kio, zéro excuse**.
3. **Boîte moyennée à la réduction, plus proche voisin à l'agrandissement.** La réduction
   est typiquement de **5× à 40×**. Un noyau bilinéaire n'échantillonne que 4 des ~400
   pixels source par pixel cible : crénelage garanti, et **scintillement au défilement**
   puisque la fenêtre d'échantillonnage saute. La boîte intègre *tous* les pixels source et
   son résultat est **stable au défilement** — la propriété qui compte le plus ici, parce
   que chaque scintillement est une cellule qui change, donc des octets sur le lien SSH. À
   l'agrandissement (favicon 16×16), le bilinéaire transformerait un pixel-art en flou,
   alors que la grille de cellules *est* déjà un média pixel-art.

**Hauteur impaire : le problème n'existe pas si on ne le crée pas.** La cible du
rééchantillonnage est **toujours `2 × rows` lignes de pixels**. L'alternative rejetée —
compléter la dernière ligne et sortir un `▀` à moitié vide — produirait une **arête franche**
en bas de chaque image, bien plus visible qu'un écrasement de 3 %.

**Transparence** : l'îlot reçoit le **plan de fond résolu par le CSS**, une couleur par
cellule, pas une couleur fixe. Sans ça, un logo transparent sur une page sombre reçoit un
halo blanc rectangulaire.

### ⚠️ `quantize_color()` est inutilisable pour une image

`quantize_256()` (`src/render/profile.cpp:19-22`) fait `v*5/255` en division entière : tout
canal ≤ 50 s'effondre sur 0, et 51 saute à 95. Mesuré sur une photo réduite en 40×12 :

| quantifieur | erreur RGB **moyenne** | erreur RGB **max** |
|---|---|---|
| troncature actuelle | **31,40** | **86,60** |
| plus proche (cube 6×6×6 **+ rampe de gris 232-255**) | **3,44** | **8,66** |

**Facteur 9,14.** Les zones sombres d'une photo deviennent du noir plat.

Donc : `image_quantize_256()` dans `src/web/paint/quantize.hpp`, et **on ne touche pas à
`quantize_color()`** — son commentaire dit qu'elle est la règle unique du projet parce que
`theme.cpp` doit répondre la même chose qu'elle, et
`theme_keeps_every_meaningful_distinction_on_every_depth` en dépend. Le chemin image n'a pas
ce contrat : il n'a pas à prouver que deux rôles restent distincts, il a à minimiser une
erreur. **Deux problèmes, deux fonctions**, et le commentaire de la neuve doit dire
exactement ça.

### `Mono16` : les demi-blocs n'y tiennent pas, et il faut le dire

16 couleurs, deux par cellule, sans niveaux intermédiaires : une photo devient une mosaïque
de six teintes. Réponse : **mode glyphes d'ombrage** — le triplet
`(fg, bg, glyphe ∈ { ' ', ░, ▒, ▓, █ })` qui minimise l'erreur contre la moyenne des deux
demi-pixels. Cinq pas de luminance entre chaque paire des 16 couleurs → de l'ordre de **100
teintes distinguables au lieu de 16**. Repli si `!profile.utf8` : rampe ASCII `" .:-=+*#%@"`
en fg seul.

Ce n'est **pas** une réouverture de la décision 3.1 : c'est ce qu'on fait là où les
demi-blocs ne peuvent structurellement pas tenir la promesse.

**Tramage (dithering) : mesuré, et c'est cher.** Floyd–Steinberg sur une photo 40×12 :
**1 978 o → 3 867 o, +96 %**. Le tramage détruit la cohérence locale dont vit
`sgr_transition`. **Désactivé par défaut**, exposé au menu contextuel, **interdit en
animation**.

**GIF animés** : frame 0 rendue, animation **arrêtée par défaut**, menu contextuel pour
l'activer, plafond **10 fps**, viewport visible seulement. Le moteur consulte le remplissage
de la file de sortie et **laisse tomber des trames d'animation** — jamais de contenu — quand
elle se remplit.

---

## 11. Le coût du diffeur — mesuré, puis soldé

`src/render/diff.cpp:108` porte une **règle 3** : `if (cell.ch >= 0x80) pos_known = false;`
— tout glyphe non-ASCII force la cellule suivante à se réancrer par `CUP` absolu.
`U+2580` vaut `0x2580`, donc **chaque cellule d'image déclenche la règle**.

### Le coût réel

Simulation fidèle de `Differ::frame` + `sgr_transition` en TrueColor, repeint complet d'une
vignette **40×12 = 480 cellules** :

| | octets/trame | **o/cellule** |
|---|---|---|
| photo | **18 647** | **38,8** |
| logo PNG | 16 434 | 34,2 |
| capture d'écran (aplats) | 12 545 | 26,1 |
| **page de texte pleine, 100×36** | **5 878** | **1,63** |

> **Une vignette de 40×12 coûte 3,2 fois une page entière de texte.** C'est ça, le problème.

Le `CUP` moyen vaut **7,5 à 8,3 octets** (`cup()` rend `4 + chiffres(y+1) + chiffres(x+1)`),
pas 14 — les 14 viennent du commentaire de `proto.hpp:73`, qui raisonne sur 65535×65535 : un
dimensionnement de tampon, pas un coût de trame.

### Les quatre leviers

| Levier | Gain mesuré | Où |
|---|---|---|
| **1. Lever la règle 3 pour les glyphes de largeur *arrêtée*** — `if (!width_is_settled(cell.ch)) pos_known = false;` | **−19,5 %** photo · −22,1 % logo · −39,7 % capture | `diff.cpp` + `width.hpp` |
| **2. Glyphe le moins cher** : quand les deux demi-pixels sont proches, une **espace** au fond coloré rend la même chose, coûte 1 octet au lieu de 3, un seul SGR au lieu de deux, **et laisse `pos_known` vrai** — donc pas de `CUP` pour la suivante non plus | seuil 0 : **−5,8 % SANS AUCUNE PERTE** · seuil 24 : **−34,2 %**, écart max 4/canal | `web/paint/raster.cpp` |
| **3. Accrocher la palette** avant l'appariement — arrondir chaque canal à un multiple de N crée des plages où `sgr_transition` n'émet rien | **−50,7 % cumulé** à N=8, invisible · −65,3 % à N=32 | idem, **six lignes** |
| 4. SGR fg+bg fusionné | −4,7 % — **rejeté** | — |

**Bilan : 38,8 → 19,1 o/cellule (−50,7 %) sans perte visible ; 13,5 (−65,3 %) en mode lien
lent.** Autres profondeurs, tout cumulé : **Indexed256 −47,9 %**, **Mono16 −57,0 %**.

Raffinement obligatoire du levier 2 : la cellule espace doit porter **l'avant-plan de la
cellule précédente** (invisible sous un espace), pas `Color::def()`. Sinon on paie
`\033[39m` à l'aller et un code RGB complet au retour.

Cas particulier qui rapporte gros : quand les deux demi-pixels valent exactement le fond de
cellule, on écrit `Cell{}` — l'égalité totale que `diff.cpp:60` exige — et la logique de
queue effaçable **écrase toute la fin de ligne transparente en un `CSI K` de 3 octets**. Une
image détourée en PNG, très fréquente, y gagne massivement.

Le levier 4 est **rejeté** : deux octets par paire, et il déplacerait
`sgr_returns_to_default_color_with_39_and_49` (`tests/test_profile.cpp:88-94`), le seul cas
de la suite où fg et bg changent ensemble. À noter dans le commentaire de `sgr_transition`
pour que la question ne soit pas rouverte tous les six mois.

### ⚠️ Le prédicat n'est pas « ASCII », c'est « largeur arrêtée »

Le danger de la règle 3 est un danger de **largeur** : si le client dessine une colonne là
où le démon en compte deux, tout ce qui est à droite se décale et le diff ne le répare
jamais.

**`U+2580` n'est PAS East Asian Narrow.** `EastAsianWidth.txt` classe `2580..2593` en
**Ambiguous**. Il est étroit dans ce projet seulement parce que `kAmbiguous`
(`src/render/width.cpp:31-36`) **omet les Block Elements**. Le risque est donc nul
aujourd'hui et **non nul le jour où quelqu'un « complète » la table depuis les données
Unicode**.

D'où le commentaire qui doit accompagner `width_is_settled()` :

```
// Les points de code dont la largeur ne dépend d'AUCUN réglage : ASCII, et une table
// d'exceptions explicites. EastAsianWidth.txt classe les Block Elements (2580..259F) en
// Ambiguous ; on s'en écarte SCIEMMENT, parce que ce projet s'en sert comme de PIXELS et
// qu'un pixel de deux colonnes n'est pas un pixel. Compléter kAmbiguous depuis les données
// Unicode ne doit PAS annuler cette exception -- c'est elle qui autorise diff.cpp à ne pas
// se réancrer après un demi-bloc.
```

### L'exposition réelle de `diff.cpp`, et le garde qui vaut tous les autres

⚠️ **AUCUN golden ne couvre `diff.cpp`.** `tests/test_golden.cpp:139-177` (`dump_chars` /
`dump_colors`) sérialise la **`Surface`**, pas la sortie du `Differ` — `run_scenario`
n'instancie jamais de `Differ`. Les 18 fichiers de `tests/golden/` y sont **insensibles**.
Le vrai périmètre exposé est **17 cas** dans `tests/test_diff.cpp`, plus 11 dans
`test_profile.cpp`.

Et le levier 1 ne déplace **aucun** d'entre eux : le seul cas de réancrage,
`diff_reanchors_after_a_non_ascii_glyph`, utilise `日` (`U+65E5`), hors table d'exceptions.

**`tests/test_diff_roundtrip.cpp` s'écrit AVANT la levée de la règle 3.** Le projet possède
un émulateur VT complet qu'il a écrit lui-même : on peut donc **prouver** que les octets du
diff reconstruisent la grille — `Surface → Differ::frame → vt::Parser → vt::Screen`,
comparaison cellule par cellule, plus mille surfaces aléatoires à graine fixe. On ne lève
pas une règle de sûreté du diffeur sans avoir d'abord construit l'oracle qui dira si on a eu
tort. Il garde ensuite toutes les optimisations suivantes gratuitement.

Tests neufs qui gardent la décision : `width_block_elements_are_narrow_under_both_policies`
(tombe si quelqu'un complète `kAmbiguous`) · `diff_does_not_reanchor_after_a_settled_narrow_glyph`
· `diff_still_reanchors_after_an_ambiguous_box_glyph` (avec `─`, `U+2500`, **dans**
`kAmbiguous` — c'est lui qui prouve que la liste blanche est « étroit arrêté » et pas
« non-ASCII ») · `a_forty_by_twelve_photo_costs_under_20_bytes_per_cell` (le seul défaut de
ce plan qui serait invisible à l'œil).

---

## 12. Les entrées

**Le défilement ne relance JAMAIS la mise en page.** Il ne change que l'origine de peinture.

```
mutation DOM / redimensionnement / zoom  ->  style -> layout -> liste d'affichage
défilement / survol / sélection / caret  ->  peinture seule
```

Repeindre 100×36 depuis une liste d'affichage ≈ **50 µs** ; une mise en page complète d'un
document de 3 000 nœuds ≈ **1 ms** ; le budget de trame est **33 ms**. C'est ce qui rend un
défilement fluide possible et une relayout par trame impossible.

La molette arrive déjà : `Session::on_mouse` la route vers la fenêtre **sous le pointeur**,
sans focus ni prise, seulement sur `WinHit::Client`. Rien à faire côté bureau.

**Barre de défilement** : une colonne à droite, curseur et gouttière **réutilisent
`gauge_bar()`** — c'est le seul usage de `█`/`░` du projet et son en-tête dit pourquoi il ne
doit pas y en avoir un second. Le glisser du curseur fonctionne **parce que le mode 1002
délivre les mouvements bouton enfoncé** : c'est exactement le geste pour lequel 1002 a été
choisi.

### ⚠️ `:hover` est impossible, et ce n'est pas négociable

Sous 1002, `MouseAction::Motion` n'arrive **que bouton enfoncé**.

`?1003h` reste écarté, et le chiffre le dit : un pointeur qui traverse 80 colonnes émet
**80 paquets SGR de ~12 octets** et surtout **80 réveils du démon**, 80 invalidations, 80
repeints — et une relayout à chaque fois qu'une règle survolée change une géométrie. En
outre `?1003h` est posé par `tty_setup_sequence()` (`src/client/tty_guard.cpp:106`) pour
**toute la session** : l'activer pour une page l'activerait pour les terminaux de toutes les
autres fenêtres. **Ne pas construire l'échappatoire.**

La substitution :

1. Une règle `:hover` est **classée** à la compilation du sélecteur : *sûre* si elle ne
   touche que `color`, `background-color`, `text-decoration`, `border-color` ;
   *géométrique* sinon.
2. Les règles *sûres* sont rejouées sur **`:active`** — le retour visuel arrive au bon
   moment.
3. Les règles *géométriques* sont **ignorées**.
4. Les liens portent `attr::Underline` **en permanence**. C'est ce qu'un utilisateur de
   terminal attend de toute façon, et ça paye la différence.
5. `:focus` est pleinement supporté.

### Hit-testing en O(1), et la copie qui ne lit pas la grille

`PaintedPage` porte un plan `hit` de `NodeId` par cellule, **écrit par le même code qui
écrit la cellule**, dans l'ordre de peinture : le dernier écrivain gagne — ce qui *est* la
réponse du hit-test CSS. Coût : 4 o × 200×50 = **40 Ko**, plus 20 Ko de `flags`, contre
200 Ko pour la grille — **30 % de surcoût pour supprimer entièrement le parcours d'arbre**.

⚠️ **Le plan `hit` ne traverse jamais le tuyau.** Le démon demande (`WebHitQuery`), le moteur
répond, et le DOM reste entièrement de son côté.

**L'extraction du texte ne passe PAS par la grille.** `Surface::text_row()` existe et saute
correctement les continuations, mais sur un îlot pixel elle rendrait un mur de `▀`, et sur
un « é » décomposé elle rendrait `e`. La copie lit la liste d'affichage —
`PaintedPage::text_in_range(anchor, focus)`. C'est la seconde raison, indépendante du §9, de
conserver la liste d'affichage après la peinture.

### Le menu contextuel

Clic droit dans le viewport, avec les raccourcis affichés en face — le motif maison.

⚠️ **`shell/Menu` n'est PAS réutilisable** : c'est le *lanceur* du bureau, avec sa boîte de
recherche et son filtrage incrémental, et `MenuItem` n'a pas de colonne de raccourci. Lui en
ajouter une toucherait les goldens du bureau. Donc `src/web/ui/popup.{hpp,cpp}`,
**délibérément séparé**, et son en-tête doit dire pourquoi — ce projet punit la duplication
silencieuse (défaut n° 9). Ce qui est partagé, c'est la **discipline** :
`layout()` / `draw()` / `hit()` lisent la même géométrie mémorisée. C'est elle qui garantit
qu'un bouton dessiné est cliquable au bon endroit — le défaut « bouton peint sur le bureau et
incliquable » du 20 août.

---

## 13. La feuille de route

**Le critère d'ordre**, puisque le flux de données ne marche plus avec « JS dès le début » :

> On construit d'abord ce dont la **forme** des autres dépend ; en dernier ce qui est une
> fonction pure derrière une interface stable.

TLS, la crypto, `inflate`, PNG et JPEG sont des `octets → octets` : **rien dans le reste du
navigateur ne change de forme selon leur implémentation**, ils se testent entièrement hors
ligne contre des vecteurs, et leur risque architectural est nul. Ce sont des **satellites**,
exécutables dans le désordre ou en parallèle.

Et **JS passe avant CSS**, contre l'intuition. Le jalon 2 pose un point de suspension du
tokeniseur, une API de mutation, des bits sales et un protocole d'objet hôte. **Tant que
JavaScript n'existe pas, tout cela est du code sans appelant** — le défaut signature du
projet, vingt-quatre fois (§9 bis de `REPRISE.md`). Le laisser sans appelant pendant toute
la durée du plus gros jalon serait exactement le motif qui a coûté un `vim` inutilisable et
un bureau sans curseur.

| Jalon | Livre | Ce qu'on voit / prouve à la fin |
|---|---|---|
| **J1 — La fenêtre et le tuyau** | `apps/browser`, `web/wire`, `web/url`, `web/loader` (`file:`, `data:`), `web/renderer`, page texte brut | Menu → Navigateur. `file:///etc/os-release` s'affiche et se replie. `kill -9` sur le moteur : bande d'erreur, bureau intact, **aucun zombie**. Et **le chiffre de bande passante, mesuré** |
| **J2 — L'arbre** | `web/text`, `web/html` (tokeniseur suspendable), `web/dom` + mode lecture | Un `.html` local **se lit** : titres, paragraphes, listes. Tokeniseur fuzzé |
| **J3 — JavaScript** | `web/js` puis `web/bind`, `<script>` pendant l'analyse, `document.write` | La même page **modifiée par son propre script**, en direct. Chaque couture du J2 a désormais un appelant |
| **J4 — Style et mise en page** | `web/css`, `web/layout`, `web/paint` | **La première vraie page**, en couleurs |
| **J5a — HTTP clair** ⟡ | `web/codec`, `web/http` | Un vrai site en `http://`, sous-ressources en parallèle |
| **J5b — TLS 1.3** ⟡ | `web/crypto`, `web/x509`, `web/tls` | `https://`. **Le transcript RFC 8448 rejoué octet pour octet** |
| **J6 — Les images** ⟡ | `web/image`, demi-blocs | **Des photos dans le terminal** |
| **J7 — Le web vivant** | Formulaires, événements, `fetch`, historique, sélection | Se connecter à quelque chose |

⟡ = satellite, démarrable dès le J1.

### ⚠️ À dire avant, pas après

Le §3 de `REPRISE.md` porte l'avertissement en toutes lettres : à la livraison du jalon 1,
l'utilisateur a été déçu (*« aucun changement sur l'app c'est encore que des click de
souris »*), et la conclusion écrite est **« il faut le dire avant, pas après »**.

**J2 et J3 produisent un navigateur qui LIT des pages mais ne les PEINT pas.** Ça se dit à
l'ouverture du J2, pas à sa clôture. La contrepartie proposée — le mode lecture — n'est pas
un mouchard jetable : il reste le repli pour `text/plain`, l'arbre d'accessibilité, et le
rendu quand la mise en page rend les armes.

### Décomposition documentaire

- **Ce document** : la spec maîtresse. Elle ferme les décisions transverses et **déclare ce
  qu'elle ne spécifie pas**.
- **Une spec par jalon** qui introduit des interfaces publiques : J2 (DOM/HTML), J3
  (JS/liaisons), J4 (CSS/mise en page), et une par satellite. **Pas de spec pour J1** : ce
  document suffit. Le gabarit est
  `docs/superpowers/specs/2026-08-11-jalon-2-wm-design.md`, qui s'ouvre exactement ainsi.
- **Un plan par jalon, toujours**, dans `docs/superpowers/plans/`, avec ses cases et son
  bilan (cas, mutations, survivantes).
- **`docs/REPRISE-NAVIGATEUR.md`** et un renvoi depuis `REPRISE.md` — qui fait déjà 1787
  lignes, et le chantier sera aussi gros que le projet actuel.

---

## 14. Le jalon 1, en détail

**Titre : « La fenêtre et le tuyau ».** Le Navigateur existe, il a son processus, son fil,
sa barre d'URL, et il affiche un fichier texte local. Aucun HTML, aucun réseau, aucun JS.

### 14.1 La parade au défaut signature commande l'ordre

L'ordre naturel — primitives crypto d'abord, page à l'écran en dernier — garantirait **des
mois de code sans appelant**. Donc on inverse.

**T0 — extraire `framing` / `wirebuf`** (§6.2). Autonome, `test_proto.cpp` est le filet.

**T1 — un `WebApp` au catalogue qui lance un moteur qui peint une grille fixe et renvoie les
frappes en écho.** Aucun réseau, aucun HTML. ~300 lignes. À la fin de T1, **tous les
appelants existent et tournent** : l'entrée du catalogue, le `fork`/`execve`, le socketpair,
les deux `std::visit`, l'`OutQueue`, le crédit, la récolte, la bande d'erreur. Chaque module
ultérieur atterrit dans un chemin d'appel **qui tourne déjà**.

La même règle plus bas : on n'écrit pas `inflate` en espérant qu'HTTP l'appelle. On écrit
d'abord l'aiguillage sur `Content-Encoding` avec `identity` seul supporté et `gzip` rendant
une erreur nommée ; puis on implémente, et **on change une ligne**. Ce changement d'une ligne
*est* la preuve de l'appelant.

### 14.2 L'ordre des tâches

`web/wire` → `web/url` (+ les ~700 cas WHATWG) → `web/loader` + `file:`/`data:` →
`PlainTextPage` + `Page` + `Interrupt` → `web/renderer/loop` + `args` →
`apps/browser/spawn` → `apps/browser/urlbar` → `apps/browser/grid` + `browser` → **la ligne
du catalogue et la ligne de `main.cpp`** → la mesure de bande passante → la sonde
bout-en-bout.

**Cinq des onze tâches n'ont besoin d'aucun processus** et se testent entièrement à l'unité.

### 14.3 Deux coutures posées au J1 parce qu'elles coûteraient une réécriture plus tard

- **`web/engine/interrupt.hpp`** — `class Interrupt { bool stopped() const; void poll(); }`,
  passé **par référence dans toute boucle longue dès la première ligne** : `Loader::poll`,
  l'analyse, la mise en page, l'interprète. Le moteur a le droit de bloquer, mais « Stop »,
  naviguer ailleurs, redimensionner et fermer doivent aboutir. Un moteur écrit en ligne
  droite n'est pas interruptible : c'est une réécriture de chaque boucle. **Test au J1** :
  une page synthétique qui occupe 10 s doit s'arrêter en moins de 100 ms.
- **`Loader` asynchrone dès le J1, implémenté en synchrone.** `start(Request) -> RequestId`,
  `poll(Interrupt&) -> optional<Response>`, `cancel(RequestId)`. Une page a trente
  sous-ressources ; un `fetch` bloquant rendant un corps obligerait à réécrire la boucle du
  moteur le jour où elles arrivent en parallèle. **Coût aujourd'hui : zéro.**

### 14.4 Fichiers existants modifiés au J1, et rien d'autre

| | |
|---|---|
| `src/app/catalog.cpp` | **une ligne** — `{"navigateur", "Navigateur", &make_browser}`. C'est elle qui empêche le défaut signature au format industriel |
| `src/main.cpp` | la branche `--web-renderer`, **avant** le bloc `current_socket_name()`, et **absente de la ligne `usage:`** — ce n'est pas un mode humain |

**Zéro modification de `CMakeLists.txt`.**

Les modifications de `src/render/width.*` et `src/render/diff.cpp` (§11) appartiennent au
**jalon images**, et seulement **après** `test_diff_roundtrip.cpp`.

### 14.5 Le critère de fin, observable

1. `./build-release/termos` → `Ctrl+A` `Espace` → filtrer « nav » → une fenêtre **Navigateur**
2. `file:///etc/os-release` s'affiche, replié à la largeur
3. Redimensionner : le texte **se replie autrement** (prouve `Viewport` → moteur → grille)
4. `PgDn` et molette défilent, **dans le moteur** (prouve l'aller-retour de l'entrée)
5. `ps --ppid <pid du démon>` montre un `termos --web-renderer` ; fermer la fenêtre le fait
   disparaître, et `ps -o stat` ne montre **aucun `Z`**
6. `kill -9` sur le moteur : « le moteur s'est arrêté », `[Recharger]`, le bureau vit, et
   `tools/sonde.py::jiffies()` montre que le démon **ne tourne pas à vide**
7. `./build-release/termos_tests web_` et `browser_` verts en **Release et sous ASan/UBSan**,
   0 avertissement ; `balayage.py --strict` et `couches.py --strict` sans candidat
8. Une campagne `tools/mutation.py` sur les fichiers du jalon, chaque survivante devenue un
   cas ou une équivalence déclarée sur place

---

## 15. Stratégie de vérification

### 15.1 Les gardes du compilateur — ils tombent sur toutes les machines

| Garde | Ce qu'il ferme |
|---|---|
| `std::visit` + `overloaded` **exhaustif**, sans `default` ni `auto&&`, **dans les deux processus** | Ajouter un message sans le traiter ne compile pas. L'idiome est déjà en tête de `src/daemon/session.cpp` — l'utiliser, pas le recopier |
| `enum class` + `-Wswitch -Werror` sur `ChainResult`, `TlsAlert`, `HttpError`, `AeadKind`, `SigAlg`, `NamedGroup`, `LoadState` | Le précédent `AcceptOutcome`, déjà argumenté dans `net.hpp` |
| **`[[nodiscard]]`** sur tout ce qui vérifie ou parse | Un résultat de vérification ignoré devient une **erreur de compilation**. ⚠️ Règle **neuve** : `[[nodiscard]]` n'existe nulle part dans `src/` aujourd'hui |
| **`Secret<N>`** sans `operator==`, sans `operator[]` rendant une référence, sans conversion implicite | Comparer ou indexer un secret **ne compile pas**. Les seules opérations viennent de `src/web/crypto/ct.hpp` et rendent des **masques**, jamais des `bool` — un `bool` est l'endroit où quelqu'un ajoutera un `if` dans six mois |
| `static_assert` là où l'ordre numérique d'un enum porte du sens | Le précédent `OutQueue::Overflow` |

⚠️ **Ce que ces gardes NE couvrent PAS** : les **virtuelles** de `App` et `Host`, référencées
par la vtable même si personne ne les appelle. Les défauts n° 4, 5 et 10 du §9 bis étaient de
celles-là, et le `WebApp` en ajoute une douzaine. D'où les deux couches suivantes.

### 15.2 `tools/atteignabilite.py` — l'outil qui manque

`balayage.py` trouve les **fonctions** sans appelant. Il ne peut pas trouver « le fichier
`brotli.cpp` entier n'est jamais atteint parce que `Content-Encoding: br` n'est jamais
demandé ». À l'échelle d'un navigateur, c'est ce second cas qui va se produire.

Vérification d'atteignabilité **par l'éditeur de liens** : build de côté dans `/var/tmp`
(jamais `/tmp`) avec `-ffunction-sections -fdata-sections -Wl,--gc-sections
-Wl,--print-gc-sections`, puis filtrer les sections jetées appartenant à `src/web/`.

⚠️ **Lier `termos`, JAMAIS `termos_tests`.** Lier le binaire de test garde tout vivant —
c'est précisément l'illusion qui a caché vingt-quatre défauts.

⚠️ **L'éprouver contre une réponse connue avant de le croire** : `git archive` du commit qui a
retiré une orpheline connue, puis vérifier qu'elle ressort. `balayage.py` a menti **deux
fois** ; un outil de vérification qu'on n'éprouve pas dérive. Angles morts à écrire dans la
docstring : les virtuelles, tout ce dont on prend l'adresse, `[[gnu::used]]`, LTO.

### 15.3 Les sondes — la couche qui a trouvé quatre défauts sur dix

Quatre neuves, toutes marquées par `TERMOS_BOOT_ID` relu dans `/proc/PID/environ` pour ne
jamais tuer le bureau vivant de l'utilisateur :

- **`tools/verif_navigateur.py`** — démon neuf, **ouvrir le navigateur par un clic** dans le
  menu, `screen()` la trame, chercher le texte dans la **grille rejouée** — jamais dans le
  flux brut, une trame n'a pas de retours à la ligne. Une sonde, toute la colonne vertébrale.
- **`tools/verif_moteur_meurt.py`** — `kill -9`, la fenêtre survit, la bande porte le bon
  texte, le bureau répond encore, **aucun zombie**.
- **`tools/verif_moteur_bloque.py`** — un serveur qui accepte et ne répond jamais ; la bande
  vers 10 s, et **la cadence du bureau ne bouge pas**, mesurée par `jiffies()`.
- **`tools/verif_contrepression.py`** — une page qui se repeint sans cesse ; l'`OutQueue` du
  client ne déborde **jamais**.

Le serveur de test est **local** : `http.server` pour le J5a, puis un serveur TLS avec
certificat auto-signé et **`TERMOS_WEB_ROOTS` pointant dessus**. L'échappatoire du magasin de
racines n'est donc pas qu'une fonction d'exploitation : **c'est ce qui rend tout le chemin
TLS sondable hors ligne et de façon déterministe.**

### 15.4 Les vecteurs sont de la donnée, pas de l'implémentation

`tests/vectors/`, résolution en deux temps (`$TERMOS_VECTORS_DIR` puis `__FILE__`, comme
`test_golden.cpp`), format **plat et orienté ligne** — jamais du JSON, ce serait une
dépendance sous un autre nom. `tools/vecteurs.py` convertit **hors ligne, une fois**, et
inscrit le SHA-256 amont et la date dans chaque fichier généré. Il n'est **jamais** lancé par
la suite.

| Suite | Utilisable sans dépendance ? |
|---|---|
| **WHATWG `urltestdata.json`** (~700 cas, données pures) | **Oui, entièrement. Le meilleur rapport valeur/effort du chantier** — à faire au J1 |
| **html5lib-tests** (`.dat`, déjà plat, parseur seul) | **Oui** — des milliers de cas gratuits le jour où le DOM existe |
| **WHATWG `encoding`** | **Partiellement** : les tables d'index sont normatives et sont de la donnée ; les tests sont en JS |
| **RFC 8448 / 7748 / 8032 / 8439 / 5869 / 4231** | **Oui**, vecteurs dans le texte |
| **NIST CAVP** (`.rsp`, texte plat) | **Oui**, échantillon **déterministe et stratifié**, règle d'échantillonnage dans le générateur |
| **Wycheproof** | **Oui** — et c'est de là que vient toute la couverture **négative** : signatures en BER, `s > n/2`, `r = 0`, points hors courbe, X25519 tout à zéro. La différence entre « mon ECDSA calcule juste » et « mon ECDSA rejette les deux cents signatures qui ont cassé la pile des autres » |
| **PngSuite** (~170 PNG, malformés compris) | **Oui** |
| **WPT `html/`, `css/`**, suites CSS du W3C | **Non** — JS + DOM, ou reftests pixel. À dire franchement. Substitut : notre corpus golden maison, avec `UPDATE_GOLDEN=1` qui **imprime toujours le diff AVANT** de réécrire |

### 15.5 Fuzzing différentiel, sans oracle externe

1. **Rapide contre naïf, dans la suite.** Pour `bignum`, `p256`, l'exponentiation modulaire :
   écrire **dans le fichier de test** une seconde implémentation scolaire, à temps variable,
   manifestement correcte, et croiser les deux sur un `Rng` xorshift64* à graine. Zéro
   dépendance, et ça attrape exactement ce que les vecteurs ratent — **les propagations de
   retenue à des frontières de limbes précises**.
2. **Aller-retour encodeur/décodeur** sur le protocole web, le générateur étant lui-même un
   `std::visit` — donc ajouter un message sans générateur ne compile pas.
3. **Interopérabilité comme oracle, hors suite** : `tools/fuzz_diff_tls.py` contre
   l'`openssl` du système. Vrai oracle, dépendance externe → **jamais dans le chemin
   obligatoire**, mais une vérification **datée** avant chaque livraison.

Pour les parseurs, la forme éprouvée du dépôt : `tests/test_web_*_fuzz.cpp`, mutateur
conscient de la structure, `Rng` xorshift64*, **jamais `rand()` ni l'horloge**, itérations
bornées dans la suite normale et `TERMOS_FUZZ_ITERS=1000000` à la demande. **Un échec se
rejoue depuis sa seule graine.**

### 15.6 Déterminisme : cinq axes, pas trois

`test_golden.cpp` en fige déjà trois (`GoldenPlatform`, `TzGuard`,
`SysInfo::freeze_for_tests()`). Ce sous-système en ajoute deux :

| Axe | Seam | Où vit l'implémentation de test |
|---|---|---|
| temps · zone · machine | existants | `tests/` (sauf `freeze_for_tests`, précédent assumé) |
| **aléa** | `RandomSource` neuf | **`tests/` uniquement, JAMAIS `src/`** |
| **réseau** | machines à états **pures** (TLS, HTTP, DNS) + interface `Transport` | `tests/` |

⚠️ **Le seam d'aléa est dangereux** : une couture qui permet à un test de forcer l'aléa est
une couture qu'on pourrait basculer en production. Parade, calquée sur `GoldenPlatform` (qui
vit dans `test_golden.cpp`, pas dans `src/`) : l'implémentation réelle est **l'argument par
défaut** du constructeur, l'implémentation déterministe **n'existe que dans `tests/`**, et un
cas assère que le défaut est bien la source du noyau.

Le seam réseau n'est pas un ajout : c'est **la même décision** qui rend RFC 8448 rejouable —
`Handshake::step(in, out)` ne contient pas de socket, ce qui permet de le piloter flight par
flight et d'asserter chaque secret intermédiaire.

### 15.7 Deux adaptations honnêtes du TDD

1. **Sur un module à vecteurs, le rouge des cas positifs ne prouve rien.** Une fonction vide
   échoue sur les 700 vecteurs ; c'est acquis d'avance. Ce qu'il faut constater rouge, ce
   sont **les cas négatifs** : écrire le test d'entrée malformée, le voir échouer **parce que
   le parseur a accepté le malformé**, puis corriger.
2. **Un test piloté par fichier qui ne charge rien passe au vert avec zéro cas.** Mode de
   défaillance réel. Parade mécanique : tout test à vecteurs commence par
   `REQUIRE(cases.size() >= N)`, **N en dur**.

### 15.8 Rappels du harnais qui vont mordre ici

- **`REQUIRE` fait un `return;` nu** : toute libération de fin de fonction est sautée. Les
  tests de ce chantier ouvrent des sockets et forkent des serveurs → **gardes RAII
  obligatoires** (`WaitpidGuard`, `UnlinkGuard`, `FifoReleaseGuard`, `EnvVarGuard`).
- **Tous les cas tournent dans le MÊME processus**, et `test_daemon.cpp` fait `waitpid(-1)` :
  **un cas qui forke doit récolter ses pids avant de rendre la main**, y compris ceux qu'il
  vient de tuer.
- Une fixture ne doit pas nettoyer **seulement ce qu'elle a créé** : nettoyage par
  `nftw(…, FTW_DEPTH | FTW_PHYS)` — la leçon des **1943 répertoires** oubliés dans `/tmp`.
- Campagnes de mutation dans **`/var/tmp`**, jamais `/tmp` (tmpfs de 2,7 Go) ; restauration
  par `copyfile + utime`, **jamais `copy2`**.
- **Préfixe `TEST(web_<module>_…)` obligatoire**, pour que `FILTERS = ["web_der_"]` soit
  mécanique — parade directe à l'incident du filtre `["files_"]` qui ne voyait pas `copy_`.

> **Prédiction utile pour lire les campagnes** : taux de mise à mort proche de 100 % dans les
> primitives crypto (muter une constante de ronde **doit** être mordu par les vecteurs
> officiels), et **bas dans les chemins de refus** — validation de chaîne, rejets DER,
> plafonds HTTP. C'est là que les survivantes enseigneront quelque chose.

---

## 16. Hors périmètre, dit une fois

Pas de révocation de certificat (le panneau TLS l'**affiche** : *« ce navigateur ne verifie
pas la revocation »*) · pas de TLS 1.2 (doublerait la surface d'attaque et ramènerait CBC, le
transport de clé RSA et la renégociation) · pas de reprise ni de 0-RTT (rejouable par
construction) · pas de `nameConstraints` (traité comme extension critique non supportée →
**rejet**, fail-closed assumé plutôt que mal implémenté) · pas de Certificate Transparency ·
pas de HTTP/2 · pas de cache disque · pas de DNSSEC ni DoH · pas de happy eyeballs (une
source de non-déterminisme pour un gain marginal) · pas de gestionnaire de mots de passe, pas
de certificats client, pas de WebCrypto, **aucun stockage d'identifiant** — ce qui est en jeu
se borne à ce que l'utilisateur tape pendant sa session.

**Le CN n'est pas consulté du tout** pour la validation de nom. Pas « seulement en l'absence
de SAN » : pas du tout. En 2026 c'est la bonne règle, et elle a son test.

**Ce qui est hors de portée, et qu'il ne faut pas prétendre :**

- La résistance à un attaquant local mesurant le cache. Un test de temps constant sur une
  machine partagée mesure le bruit de la machine.
- La garantie que `-O2` n'a pas réintroduit un branchement dans une primitive à temps
  constant. Atténuation à portée, et **une seule fois** : `__attribute__((noinline))` sur les
  primitives CT et **une lecture du désassemblage à la main, datée, consignée en
  commentaire** — exactement ce que le projet a déjà fait pour le `-O3 … -O2` de
  `CMakeLists`.
- Toute comparaison avec OpenSSL.

**L'énoncé honnête, à écrire plutôt qu'à laisser deviner :**

> *Cette pile est conforme octet à octet sur N traces publiées et rejette M entrées
> malformées publiées ; elle n'a jamais été relue par un cryptographe et n'a aucun
> historique.*

---

## 17. Deux décisions encore ouvertes

À trancher **dans ce document**, pas par accident dans le premier fichier qui en a besoin.

1. **`font-size` sur un média à une seule taille de glyphe.** Le §8.2 pose que l'avance ne
   change pas. Reste : la taille sert-elle uniquement à l'échelle d'emphase (gras / estompé /
   filet), ou aussi à donner **deux rangées de hauteur** à un titre ? Les deux réponses sont
   défendables ; ce qui ne l'est pas, c'est de trancher par accident.

2. **Le bureau doit-il posséder la sélection de texte ?** Le §3 de `REPRISE.md` documente que
   la question a été explorée, tranchée « non construite », et que **si elle revient**, le
   point qui décide est que `Maj`+glisser sélectionne la trame **composée** — donc que le
   bureau devrait posséder la sélection lui-même. Un navigateur rend la question urgente : on
   copie sans arrêt depuis une page.
   ⚠️ **Si oui, la peinture doit conserver la correspondance cellule → nœud dès le départ**
   (§12) ; la reconstituer après coup depuis une grille de cellules est impossible.
   ⚠️ Et le transport vers le presse-papiers passerait par **OSC 52**, or **aucun octet venu
   d'une application n'est jamais relayé tel quel** vers le client. Ouvrir une voie « octets
   bruts » dans `FrameMsg` est une décision à part entière.
   *Note d'environnement, 21 août 2026* : chez l'utilisateur, **copier depuis le terminal ne
   fonctionne pas** (coller, si — cause non diagnostiquée). Le repli `Maj`+glisser n'est donc
   peut-être pas le repli qu'on croyait.

---

## 18. Ce qui a été affirmé pendant la conception, et qui était faux

Le format maison finit par cette section. Trois erreurs ont été corrigées par la mesure ;
deux d'entre elles changeaient une décision.

| Affirmé | Réel | Conséquence |
|---|---|---|
| « `diff.cpp` est couvert par 18 références golden » | **Zéro** (§11) | Modifier `diff.cpp` est nettement moins risqué qu'annoncé |
| « `U+2580` est East Asian Narrow » | **Faux** : Unicode le classe **Ambiguous** ; il est étroit ici parce que `kAmbiguous` omet les Block Elements | La levée de la règle 3 doit être une **exception écrite et testée**, pas un constat sur lequel s'appuyer |
| « une cellule d'image coûte ~51 o, un `CUP` en vaut 14 » | **38,8 o/cellule** mesuré, `CUP` moyen **7,5 à 8,3 o** | Le problème reste réel : 3,2 fois une page de texte |
| « `-Wpedantic` interdit les littéraux > 64 Kio » | **Faux**, sonde du §4 : accepté | Les corpus peuvent tenir en `R"(...)"` |
| « si `__int128` est refusé, il faut écrire en limbes 32 bits » | **Incomplet** : refusé nu, **accepté via `__extension__`** | Le chemin 32 bits devient l'oracle du fuzzing différentiel, pas un mode dégradé |
