# ssh_os 2.0 — installation locale et mise à jour depuis le bureau

> Conception arrêtée le 17 août 2026, avec l'utilisateur, avant toute implémentation.
>
> **Révision 2.** La révision 1 a été passée à quatre relecteurs indépendants ; chacun de
> leurs constats a ensuite été revérifié dans le code. Elle contenait **trois inversions**
> — des mécanismes qui faisaient le contraire de ce qu'elle affirmait — et **deux
> promesses intenables**. Le §13 les liste, avec ce qui les corrige. Ce document est
> réécrit, pas retouché.

---

## 1. Ce qu'on construit, et pourquoi

Aujourd'hui, `ssh_os` ne se lance que depuis l'arbre de développement, par
`./build-release/sshos`. Conséquence vécue : **recompiler ferme le bureau**, puisque
c'est le même binaire et le même socket.

L'objectif est d'avoir **deux instances qui s'ignorent** :

- une **installée**, stable, poste de travail quotidien — c'est elle qui héberge la
  session Claude qui travaille sur le projet ;
- celle de **développement**, qu'on casse et qu'on recompile sans conséquence.

Et, dans l'instance installée : une **vérification automatique une fois par jour**, une
**pastille cliquable** dans la barre des tâches, et **une entrée de menu dont le libellé
change** selon l'état.

---

## 2. Ce qui n'est pas négociable

Les contraintes du §4 de `docs/REPRISE.md` s'appliquent intégralement. Quatre décident de
presque tout ce qui suit.

| Contrainte | Ce qu'elle interdit ici |
|---|---|
| **`CMakeLists.txt` intouchable** | Pas de cible `install()`, pas de `configure_file`, pas de `-D` pour graver une version. |
| **Un thread, un `epoll`, aucun mutex** | Le démon ne bloque jamais. Ni `git`, ni `cmake`, ni réseau dans son fil. |
| **Zéro dépendance externe (C++)** | Fichier d'état en clé=valeur analysé à la main. |
| **Aucun code de compatibilité** *(ajoutée ici)* | Écrit pour le système de son auteur. Pas de `#ifdef` pour de vieilles glibc. Un système plus ancien **compile** (§5). |

**Mécanique vérifiée :** `CMakeLists.txt:17` est `file(GLOB_RECURSE … CONFIGURE_DEPENDS
…/src/*.cpp)` et `CMakeLists.txt:26` est `file(GLOB … …/tests/test_*.cpp)` — **`GLOB` nu,
non récursif** pour les tests. Les fichiers de ce travail sont donc pris automatiquement
**à condition** que le test soit à plat dans `tests/` : `tests/test_update_service.cpp`,
jamais `tests/shell/test_update_service.cpp`, qui serait **silencieusement ignoré**.

---

## 3. L'isolation entre l'installé et le développement

### 3.1 Le levier, et ce qu'il ne suffit pas à faire

`net.hpp:29` définit `kBootIdEnvVar = "SSHOS_BOOT_ID"`, consultée en priorité par
`read_boot_id()`. Le nom du socket est `sshos/<uid>/<boot_id>` (`net.cpp:118`).

Le lanceur `~/.local/bin/sshos` **n'est pas le binaire** :

```sh
#!/bin/sh
SSHOS_BOOT_ID="${SSHOS_BOOT_ID:-local}"
SSHOS_EXE="$HOME/.local/libexec/sshos"
export SSHOS_BOOT_ID SSHOS_EXE
exec "$SSHOS_EXE" "$@"
```

### 3.2 La fuite que la révision 1 n'avait pas vue — CORRECTIF OBLIGATOIRE

`pty/env.cpp` ne retire de l'environnement des shells enfants que `kSessionVars`
(`env.cpp:16-19` : SSH_AUTH_SOCK, SSH_CONNECTION, SSH_CLIENT, SSH_TTY, DISPLAY,
XDG_SESSION_ID) et `kBanned` (`env.cpp:36` : `LINES`, `COLUMNS`) ; `daemon_env()` recopie
**tout `environ`**.

**`SSHOS_BOOT_ID` est donc hérité par chaque shell du bureau.** Or le §1 dit que le bureau
installé héberge la session Claude qui travaille sur le projet : dans ce shell,
`./build-release/sshos --kill` calcule le même nom de socket et **tue le bureau**. C'est
exactement le piège que la révision 1 déclarait « structurellement impossible ».

> **Correctif :** ajouter `SSHOS_BOOT_ID` **et** `SSHOS_EXE` à `kBanned` (`env.cpp:36`).
> Un enfant du bureau n'hérite plus de l'identité de son propre bureau.
>
> **Conséquence assumée :** `sshos` tapé dans une fenêtre du bureau ne se rattache plus à
> ce bureau — il vise le nom par défaut, donc l'instance de développement. C'est le
> comportement voulu : le rattachement imbriqué n'a aucun intérêt, et le `--kill` qui
> détruit la session de travail en a un très négatif.

### 3.3 `SSHOS_BOOT_ID` est une frontière de nommage, pas de sécurité

`net.hpp:66-68` le dit : *« Une adresse abstraite n'a pas de permissions : tout processus
de la machine peut se placer dans le backlog, et le contrôle d'uid n'intervient qu'après
l'`accept()` »*. Les noms abstraits sont de surcroît listés dans `/proc/net/unix`. La
seule frontière réelle est le contrôle d'uid de `net.cpp:188`.

Ce que l'isolation achète est donc précis et suffisant : **elle empêche la collision
accidentelle**, qui est le problème à résoudre. Elle n'empêche rien d'intentionnel, et ce
document ne le prétend nulle part.

### 3.4 Une constante contredit un choix déjà écrit — et c'est assumé

`net.cpp:104-108` écarte nommément l'idée d'une valeur fixe : *« Une constante fixe serait
pire encore : identique à chaque redémarrage, elle réintroduirait la confusion de référence
périmée que `boot_id` existe pour éviter »*.

Ce raisonnement vise un **repli automatique** qui remplacerait `boot_id`. Ici,
`SSHOS_BOOT_ID=local` est une **échappatoire posée par un opérateur**, l'usage même que
`net.hpp:22-28` prévoit. Le danger visé — un écouteur périmé survivant à un redémarrage —
n'existe pas : une adresse abstraite disparaît avec le processus qui la tient. La
prévisibilité du nom n'abaisse que le coût du déni de service déjà documenté en
`net.cpp:138`, qui n'est pas fermé de toute façon.

---

## 4. Le fichier d'état

`~/.local/share/sshos/state` — **toujours sous le home de l'utilisateur**, quel que soit
le préfixe d'installation (§9). Clé=valeur, une paire par ligne :

```
schema=1
prefix=/home/user/.local
source=git
installed_commit=4de7722a8f1c9b0e5d3a2f6c8b1e4d7a0c3f6b9e
previous_commit=6849b6bc7f99c06a5a77585ddf008cb83c0a5133
remote_commit=
checked_at=1755400000
status=up-to-date
pid=
message=
```

| Clé | Sens |
|---|---|
| `schema` | `1`. Valeur inconnue → traité comme fichier absent. |
| `prefix` | Racine d'installation. Sert au service à savoir s'il peut proposer une mise à jour. |
| `source` | `git` \| `release` \| `archive` \| `local` |
| `installed_commit` | Empreinte complète, ou `unknown`. |
| `previous_commit` | Ce que `--rollback` restaurerait. Vide si aucun. |
| `remote_commit` | Vu à la dernière vérification. |
| `checked_at` | Époque Unix, secondes. |
| `status` | `idle` \| `checking` \| `applying` \| `up-to-date` \| `available` \| `restart-pending` \| `check-failed` \| `apply-failed` \| `history-rewritten` \| `updates-disabled` |
| `pid` | Pid du script en cours, renseigné **seulement** pour `checking` et `applying`. |
| `message` | Une ligne, destinée à l'utilisateur. |

### 4.1 Écriture — la discipline du script

- **Atomique** : écrire `state.tmp` dans le **même répertoire**, puis `rename()`.
- **`checking` / `applying` sont écrits AVANT de travailler**, avec `pid=$$`. Sans ça, un
  démon qui redémarre pendant une application ne peut pas savoir qu'un travail court.
- **`message` est assaini par l'écrivain** : tout octet de contrôle (`\000-\037`, `\177`)
  remplacé par une espace, puis **troncature à 200 octets**. Le §8 y verse le résumé d'une
  compilation cassée : un seul `\n` non filtré **forgerait une paire clé=valeur**.

### 4.2 Lecture — la discipline du C++

Tolérante, jamais devineresse. Fichier absent, vide, `schema` inconnu, ou **plus grand que
4 Kio** → état `Idle`, sans message. Ligne sans `=` → ignorée. Découpe au **premier** `=`.
**La première occurrence d'une clé gagne** — une ligne ajoutée après coup ne peut pas
écraser une valeur déjà lue. Valeur illisible → défaut du champ. Aucun cas ne lève.

`checked_at` hors de `[0, maintenant]` est ramené à `0`, ce qui traite d'un seul geste
l'horloge reculée et le débordement de `checked_at + 86400`.

### 4.3 Un travail en cours dont le pid est mort

`status=checking` ou `applying` avec un `pid` qui n'existe plus (`kill(pid, 0)` → ESRCH)
est traité comme **`Failed`**, message *« interrompu »*. C'est le cas du démon redémarré
pendant une application, et celui du script tué.

### 4.4 `message` ne voyage jamais dans le protocole

`client.cpp:282` écrit `Detached::reason` **directement sur le vrai terminal**, en mode
brut, avant que `~TtyGuard()` n'ait restauré l'écran :
`std::fprintf(stderr, "\r\nsshos: detache (%s)\r\n", …)`. C'est le seul puits du projet
qui ne passe ni par la grille ni par le `Differ`.

> **Interdiction explicite :** `message` — issu de `git`, `cmake` ou du compilateur — ne
> doit jamais être placé dans `Detached::reason`. La raison de détachement est une
> constante (§7.4).

*(La grille, elle, est immunisée : `width.cpp:56` rend une largeur nulle pour C0, DEL et
C1, et `surface.cpp:112` fait `if (cw == 0) continue;` — aucun octet de contrôle n'entre
jamais dans une `Cell`. Rien à durcir de ce côté.)*

---

## 5. L'échelle d'acquisition

| # | Condition | Ce qu'on fait | `source=` |
|---|---|---|---|
| **1** | `git` disponible | `clone` / `fetch`, compilation, suite complète, installation | `git` |
| **2** | pas de `git`, mais HTTPS | binaire + tests + références de la dernière Release, SHA256 vérifiés, démarrage éprouvé | `release` |
| **3** | pas de binaire exploitable | archive des sources au sha résolu, compilation, suite complète | `archive` |
| **4** | rien de joignable | l'arbre local d'où l'installeur est lancé | `local` |

### 5.0 Le modèle de menace, écrit une fois pour toutes

**La racine de confiance est le compte GitHub `might-stormlord`.** `SHA256SUMS` est publié
dans la même release, par le même canal : il prouve l'**intégrité du transport** — cache
menteur, miroir tronqué, coupure — **pas l'authenticité**. Qui peut publier une release
publie un `SHA256SUMS` cohérent.

Et « on n'installe qu'après le vert » **exécute déjà la charge utile** : la sonde lance le
binaire téléchargé, la suite lance `sshos_tests` téléchargé, `cmake` exécute le
`CMakeLists.txt` distant. La propriété réelle est donc :

> **On n'installe pas du code cassé.** Ce mécanisme ne dit rien du code malveillant, qui a
> déjà tourné au moment où le vert s'affiche.

Aucune signature détachée n'est prévue : le dépôt est personnel, et le coût d'une clé à
gérer dépasse le gain pour cet usage. **C'est une décision, pas un oubli.** Le jour où
d'autres personnes installeraient ce logiciel, elle serait à rouvrir.

**Durcissement du transport, obligatoire :** `curl --proto '=https' --proto-redir '=https'
--tlsv1.2 -fsSL`, ou `wget --https-only`. Par défaut `curl` suit une redirection
`https → http`, et les URL d'assets GitHub redirigent.

### 5.1 Échelon 1 — git

`git clone https://github.com/might-stormlord/sshos.git ~/.local/share/sshos/src`, puis
`git fetch`. **Jamais une copie de l'arbre de développement.**

Vérification : `git ls-remote origin main` — aucun objet téléchargé.

**Contrôle de descendance, obligatoire :**

```sh
git merge-base --is-ancestor "$installed_commit" "$remote_commit"
```

Si le commit installé n'est **pas** un ancêtre du distant, ce n'est pas une mise à jour :
c'est un historique réécrit. `status=history-rewritten`, libellé
`Reinstaller depuis GitHub`, jamais « Mettre a jour ».

> Ce cas n'est pas théorique ici. `docs/REPRISE.md` §2 bis documente que **`main` a été
> réécrit et force-poussé deux fois le 17 août 2026**. Un `filter-branch` de plus change
> les 210 empreintes et ferait proposer une « mise à jour » vers un historique sans
> relation avec celui en place.

### 5.2 Échelon 2 — le binaire publié

`api.github.com/repos/might-stormlord/sshos/releases/latest` donne les assets ; on
télécharge `sshos`, `sshos_tests`, **`golden.tar.gz`** et `SHA256SUMS`, et on vérifie les
empreintes avant tout.

**Pourquoi `golden.tar.gz`.** `tests/test_golden.cpp:42-46` déduit le répertoire des
références de `__FILE__`, **gravé à la compilation** — donc un chemin du conteneur de CI,
absent de la machine cible. Les **9** cas `golden_*` échoueraient par construction, et
l'échelon 2 raterait systématiquement son propre garde-fou.

> **Correctif, en deux gestes :** `tests/test_golden.cpp` lit d'abord la variable
> `SSHOS_GOLDEN_DIR` et ne retombe sur `__FILE__` que si elle est absente ; la release
> publie `golden.tar.gz`, que l'installeur déplie et pointe par cette variable. La suite
> passe alors **entière** en mode binaire, et la propriété de sûreté tient sur les quatre
> échelons.

**Éprouver le binaire avant de l'installer.** Appel avec un **drapeau inconnu** :
`main.cpp:109-111` répond `usage: sshos [--daemon|--status|--kill]` sur stderr et rend
**2**.

> ⚠️ **Cette sonde n'est pas indépendante de l'environnement**, contrairement à ce
> qu'affirmait la révision 1. `main.cpp:69-75` calcule le nom du socket **avant** de lire
> `argv`, et `read_boot_id()` **lève** si `/proc/sys/kernel/random/boot_id` est illisible
> et qu'aucune variable n'est posée → **retour 1**, sans `usage:`. Un binaire sain serait
> classé cassé.
>
> **Correctif :** la sonde s'exécute avec `SSHOS_BOOT_ID=probe` dans son environnement, ce
> qui court-circuite `/proc` (`net.cpp:74`).

Discrimination des codes de retour, à écrire telle quelle :

| Code | Lecture |
|---|---|
| **2** + ligne `usage:` | le binaire se charge et s'exécute → **installer** |
| **1** | il s'est chargé, l'environnement est incomplet → **installer** (ce n'est pas le binaire) |
| **126**, **127**, mort par signal | l'éditeur de liens l'a refusé → **échelon 3** |

**Ne pas sonder avec `--status`** : il rend 1 sans démon et 0 avec — son code dépend de
l'environnement (`main.cpp:83-92`).

### 5.3 Échelon 3 — l'archive

**Résoudre le sha d'abord** : `api.github.com/repos/might-stormlord/sshos/commits/main`,
puis télécharger `codeload.github.com/might-stormlord/sshos/tar.gz/<sha>` — jamais
`refs/heads/main`, qui est une cible mouvante et ne dirait pas ce qu'on a pris. Le tarball
et le sha viennent ainsi du même instant, et `installed_commit` est **connu** (la
révision 1 le laissait à `unknown`, ce qui enfermait cet échelon dans « Reinstaller » à
vie). Déplier en absorbant le répertoire de tête ajouté par GitHub.

### 5.4 Échelon 4 — l'arbre local

Ni `git` ni HTTPS. On installe depuis l'arbre où l'installeur est lancé, zip déplié
compris, puis `status=updates-disabled` avec la raison. Libellé
`Mise a jour indisponible (git absent)`, **inerte**.

> Faire semblant de vérifier serait le pire comportement possible.

### 5.5 La descente d'échelon est interdite pendant une mise à jour

**À la première installation**, l'échelle se descend librement.

**Pendant une mise à jour, jamais.** Un échec de l'échelon inscrit dans `source=` donne
`apply-failed`, pas une descente silencieuse.

> Sinon le garde-fou du §5.2 devient un déclencheur de dégradation : il suffit de servir
> un binaire volontairement non chargeable pour faire retomber la machine sur un canal
> moins contrôlé.

### 5.6 Le commit inconnu

`installed_commit=unknown` (échelon 4, ou arbre de provenance douteuse) : aucune
comparaison n'est possible, et on ne prétend pas le contraire. Libellé
`Reinstaller depuis GitHub`, qui remplace l'inconnu par un commit connu.

### 5.7 Un arbre de compilation impropre

`~/.local/share/sshos/src` est réutilisé si et seulement si `src/.git` existe **et** que
`git remote get-url origin` rend l'URL attendue. Sinon il est effacé et refait — le cas
d'une installation par archive suivie de l'apparition de `git`, et celui d'un `clone`
interrompu. Si l'effacement échoue (droits, disque plein), `apply-failed` avec la raison ;
on ne compile pas dans un arbre dont on ne sait rien.

---

## 6. `UpdateService` — la machine à états

`src/shell/update_service.{hpp,cpp}`. Sans interface, sans réseau, sans `git`.

### 6.1 Les sept états

| État | Libellé du menu | Actif | Pastille |
|---|---|---|---|
| `Idle` | `Verifier les mises a jour` | oui | non |
| `Checking` | `Verification en cours...` | **non** | non |
| `UpToDate` | `Verifier les mises a jour` | oui | non |
| `Available` | **`Mettre a jour`** | oui | **oui** |
| `Applying` | `Mise a jour en cours...` | **non** | non |
| `RestartPending` | **`Redemarrer pour terminer`** | oui | **oui** |
| `Failed` | `Verifier les mises a jour` | oui | non |

Deux libellés se substituent au premier : `Reinstaller depuis GitHub` (§5.6, §5.1) et
`Mise a jour indisponible (…)` (§5.4, et préfixe système au §9).

**Sans accents**, comme `Ranger les fenetres` (`menu.cpp:46`) et `Fermer la session`
(`menu.cpp:52`).

**`RestartPending` répond à un mensonge de la révision 1.** Après « Installer maintenant »,
le binaire posé n'est pas celui qui tourne : `installed_commit` passait au commit neuf, la
vérification suivante répondait `up-to-date`, la pastille s'éteignait — et l'utilisateur
continuait sur l'ancienne version, sans aucune indication, potentiellement des semaines
puisqu'un démon ne meurt jamais seul. La pastille reste donc allumée **jusqu'au
redémarrage effectif**, et le démon efface `pending_restart` au démarrage quand son propre
binaire correspond.

### 6.2 « Inerte » a besoin d'un observable

`MenuItem` est `{ id, label }` (`menu.hpp:11-14`) : rien n'exprime l'inactivité, et un
libellé « en cours » n'empêche personne d'appuyer sur Entrée.

> **Correctif :** `MenuItem` gagne `bool enabled = true`. `Session::run_menu()` ignore un
> identifiant désactivé, et le rendu le grise. Le service rend `{label, enabled}`, donc le
> cas se teste **unitairement**, sans passer par la session.

La garde ne repose pas que sur l'interface : le routage de `update:*` revérifie l'état
avant d'agir.

### 6.3 Les transitions et leur horloge

**La couture existe déjà** : `platform.hpp:12-22` expose `now()` (murale), `steady_now()`
(monotone) et `read_file()`, et `Session` en tient une référence (`session.hpp:243`).
`UpdateService` porte un `const Platform&`.

`platform.hpp:16-19` tranche la question des unités : *« now() rend une heure MURALE,
qu'un ajustement d'horloge peut faire reculer : inutilisable pour mesurer un délai. Tout
ce qui expire dans ce projet se mesure ici »* — donc :

- au démarrage, `checked_at` (murale) sert **une seule fois** à calculer un reliquat,
  **borné à `[0, 24 h]`** ;
- l'échéance elle-même court sur `steady_now()`.

Transitions :

- **Au démarrage du démon** : vérification planifiée à `min(reliquat, …)`, et jamais avant
  **30 s**, pour ne pas ralentir l'ouverture du bureau.
- **Toutes les 24 h** ensuite.
- **`Verifier les mises a jour`** → `Checking` immédiatement.
- **`Mettre a jour`** → confirmation (§7.3) → `Applying`.
- **`Redemarrer pour terminer`** → confirmation (§7.3) → redémarrage (§7.4).
- **Mort de l'enfant** → relecture du fichier d'état ; s'il n'a pas changé et que le code
  de retour est non nul → `Failed`, message générique. Sans cette règle, rien ne distingue
  « rien fait » de « fait ».

### 6.4 Le réveil — ce qui manquait

`on_refresh()` n'existe que sur `App` (`app.hpp:135`), appelé uniquement par
`Session::mark_refresh_due()` sur les fenêtres (`session.cpp:506`). Pire, `daemon.cpp:319`
lit `session.refresh_delay_ms()` **seulement `if (client)`**, et `daemon.cpp:298-300` ne
pose le plancher d'une seconde que dans le même cas — le commentaire l'assume : *« sans
client il n'y a aucun plancher du tout : le démon au repos continue de bloquer
indéfiniment »*.

Un bureau détaché — l'état **normal** de ce projet — ne verrait donc jamais échoir ses 24 h.

> **Correctif, avec un précédent exact :** `Session::update_delay_ms()`, replié dans le
> délai d'`epoll_wait` **sans la garde `if (client)`**, comme `session.help_delay_ms()` à
> `daemon.cpp:311-314`. Une vérification automatique est autorisée sans client : elle
> n'affiche rien (§8), elle ne fait qu'écrire le fichier d'état.

### 6.5 Le lanceur d'enfant — `spawn_detached` est INTERDIT ici

`daemonize.cpp:48-55` fait un **double fork** : il rend le pid de l'**intermédiaire**, qui
`_exit()` aussitôt après le second fork ; le petit-enfant est réparenté à init et **ne peut
structurellement jamais être récolté** par le démon. `daemonize.hpp:37` le dit :
*« Rend le pid de l'enfant intermédiaire ; l'appelant DOIT le récolter »*.

Utilisé tel quel, `Applying` retomberait **~1 ms** après le clic, sur un fichier d'état
inchangé, pendant que la compilation tourne hors radar — et le script survivrait à l'arrêt
du démon.

> **Le bon primitif est celui de `Pty`** (`pty.cpp:81`) : un `fork()` **simple** puis
> `execv()`. Trois obligations dans l'enfant, avant l'exec :
>
> 1. **Réinitialiser le masque de signaux** — `daemon.cpp:98` bloque SIGTERM/SIGINT/SIGCHLD
>    pour le `signalfd`, et **le masque survit à `execve`**. `pty.cpp:98-99` fait déjà
>    exactement `sigemptyset` + `sigprocmask(SIG_SETMASK, …)` ; sans ça, `git`, `make` et
>    le binaire de tests hériteraient de SIGCHLD bloqué.
> 2. **Rétablir les dispositions de signaux par défaut**, comme `pty.cpp:107`.
> 3. **Rediriger 0/1/2** vers `~/.local/share/sshos/update.log` — la sortie ne doit ni se
>    perdre ni atteindre le terminal.

**Le service ne forke pas lui-même.** Il porte un **lanceur injecté** rendant un `pid_t` ;
en production la session fournit un lanceur qui forke et inscrit le pid auprès du
récolteur unique. Le contrat de récolte est `on_child_exit(pid_t pid, int status)`
(`reap.hpp:12`) — **deux arguments**, comme partout ailleurs dans le projet.

`Session::on_child_exit` route aujourd'hui par fenêtre (`ChildWatch{pid, win}`,
`host.hpp:59-62`) et **ignore silencieusement** un pid absent de la table. Un enfant de
service n'a pas de fenêtre : il faut donc un aiguillage explicite **avant** la recherche
dans `children_`.

### 6.6 Un seul travail à la fois, pour de vrai

Trois choses concourent, et aucune ne suffit seule :

1. `update.sh` prend un **`flock` sur `~/.local/share/sshos/lock`** pour toute la
   séquence — rotation du binaire, pose, écriture d'état. `install.sh` prend le même.
2. Le service refuse de lancer si l'état est `Checking` ou `Applying` avec un pid vivant.
3. **Délais** dans le script : **60 s** pour une vérification, **30 min** pour une
   application. Sans eux, `Checking` est un état dont on ne sort jamais.

> Le verrou n'est pas un ornement. Sans lui, deux applications concurrentes font ceci : A
> copie `sshos` → `sshos.previous`, B fait de même **après** que A a posé le neuf.
> `sshos.previous` contient alors le **nouveau** binaire, et `--rollback` — la seule parade
> au cas « le nouveau démarre mal » — restaure la version cassée.

### 6.7 Ce qui la rend testable

Trois coutures, toutes avec un précédent dans le projet :

| Couture | Défaut | Précédent |
|---|---|---|
| Chemin du fichier d'état | `~/.local/share/sshos/state` | `read_boot_id(boot_id_path)`, `net.hpp:44` |
| Lanceur d'enfant | le `fork`/`exec` du §6.5 | — |
| `const Platform&` | `RealPlatform` | `Session`, `session.hpp:35` |

---

## 7. L'intégration au bureau

### 7.1 Le menu

`menu.cpp:32-54` : `Menu::open()` construit `all_` **lui-même**, sans paramètre, et il
n'existe aucun setter. La révision 1 disait « le menu reçoit un libellé » sans dire par où.

> **Correctif :** `Menu::set_extra_items(std::vector<MenuItem>)`, posée par la session
> **avant** chaque ouverture ; `open()` les ajoute après les entrées fixes. Trois sites
> d'ouverture à couvrir : `session.cpp:249` (clavier), `:766` (bouton du panneau),
> `:828` (clic droit sur le vide).

Le routage réel est dans `Session::run_menu()`, **`session.cpp:150-187`** — et non
`session.cpp:204`, qui n'est que le site d'appel de `menu_.selected()` dans la branche
clavier. Les domaines existants sont `app:` (150), `panel:` (159), `cmd:` (170),
`wm:tile` (177), `session:detach` (181), `session:quit` (187). On ajoute :

- `update:check`
- `update:apply`
- `update:restart`

### 7.2 La pastille — cliquable, sinon elle viole la règle du projet

`PanelHit` (`panel.hpp:13`) n'a aucune valeur pour elle. Une pastille peinte et non
cliquable contredit frontalement *« La souris d'abord. Toute fonction est atteignable sans
raccourci clavier »* — règle que `REPRISE` §10 note avoir **déjà dû être redite une fois**.
Le précédent est explicite dans `session.cpp:768` : *« Le rappel dit quelle touche ouvre
l'aide ; le cliquer l'ouvre aussi. »*

> **Correctif :** `PanelHit::Update`, `Panel::set_update_badge(bool)` posée **avant**
> `layout()` — la discipline maison veut que `layout()` calcule une fois ce que `draw()`
> **et** `hit()` relisent — glyphe `●` en UTF-8, `*` en ASCII, placée avant l'horloge comme
> `hint_`. Le clic déclenche la même action que l'entrée de menu correspondant à l'état.
>
> **Priorité de largeur :** la pastille cède avant l'horloge et après le rappel ; elle fait
> une à deux colonnes, donc le cas est rare mais doit être tranché plutôt que découvert.

### 7.3 Les confirmations — deux questions binaires, pas un dialogue à trois choix

La révision 1 dessinait `[ Installer maintenant ] [ Installer et redemarrer ] [ Annuler ]`
en affirmant que « le mécanisme existe déjà ». **Faux** : `modal.hpp:11` est
`ModalHit { None, Body, Cancel, Confirm }`, les libellés `[ Annuler ]` / `[ Confirmer ]`
sont **compilés en dur** (`modal.cpp:8-9`), `kHeight = 6` avec **une seule** ligne de
question, et `focus_next()` bascule un booléen.

Plutôt qu'élargir `Modal`, son hit-test, son dessin et son clavier, la révision utilise le
mécanisme tel qu'il est, **deux fois de suite** :

**Depuis `Available`** — rien ne se ferme, donc la question est légère :

> `Installer la mise a jour ? Vos fenetres restent ouvertes.`

**Depuis `RestartPending`** — c'est ici, et seulement ici, que quelque chose meurt :

> `Redemarrer maintenant ? 3 fenetres et 2 processus en cours seront fermes.`

Ce découpage a trois vertus : aucun travail sur `Modal`, l'option destructrice est isolée
derrière sa propre question, et l'invariant existant est préservé —
`modal.hpp:26-28` : *« Annuler a le focus par défaut : la réponse sûre à une question
destructrice ne doit jamais être celle qu'on donne par inadvertance, d'un Entrée
réflexe. »*

**« 2 processus en cours », pas « 2 shells ».** La session ne connaît pas les types
d'applications, c'est un principe affiché (`session.cpp:170-176`). Ce qu'elle sait compter,
c'est `children_` (`session.hpp:259`) — un shell comme une copie de fichiers. Le libellé
dit donc ce qui est vrai.

### 7.4 Le redémarrage

**Trois défauts de la révision 1 se corrigent ici.**

**a) Ne pas relancer par `/proc/self/exe`.** `main.cpp:40` fait
`spawn_detached({"/proc/self/exe", "--daemon"})`. `/proc/self/exe` désigne l'**inode**
exécutée, pas un chemin. Le client qui redémarre est **par construction l'ancien
binaire** : après la pose du neuf, son `/proc/self/exe` pointe sur `sshos.previous`.
Le redémarrage relancerait donc l'ancienne version **en silence**, et le garde-fou
`Incompatible` invoqué par la révision 1 ne se déclencherait jamais, les deux `kBuildId`
concordant.

> **Correctif :** relancer par **chemin**. Le lanceur exporte `SSHOS_EXE` (§3.1) ;
> `start_daemon_and_connect()` l'utilise s'il est défini, et retombe sur `/proc/self/exe`
> sinon — ce qui laisse le comportement de l'arbre de développement inchangé.

**b) La raison est une constante, pas une phrase.** `Detached::reason` est du texte libre
partout ailleurs — les quatre raisons existantes sont des littéraux français
(`daemon.cpp:537, 607, 685, 746`), simplement imprimés par le client.
Faire dépendre un comportement d'une comparaison de texte libre casserait à la première
reformulation.

> **Correctif :** `kDetachReasonUpdate`, constante partagée dans `proto.hpp`, comparée par
> **égalité**. Pas de changement de format de trame, donc pas de `kBuildId` à incrémenter.

**c) Fermer l'écouteur AVANT d'émettre le `Detached`.** Une adresse abstraite est libérée à
la fermeture du descripteur, pas à la sortie du processus. Si le démon émet puis sort, le
client — qui réagit en microsecondes — se **reconnecte au démon mourant** : `main.cpp:116`
réussit, `start_daemon_and_connect()` n'est jamais appelé, et `run_client()` se fait fermer
au nez, sur le message trompeur *« le demon a ferme la connexion sans annoncer de
detachement »*.

> **Correctif :** fermer le descripteur d'écoute d'abord, puis émettre, puis sortir. Et
> côté client, **boucler** avec la cadence déjà écrite — `kConnectAttempts` × `kConnectDelayUs`,
> 50 × 20 ms, `main.cpp:17-18` — au lieu d'un essai unique.

**d) Sortir du démon depuis un autre chemin que le clavier.** `wants_quit()` n'est lu qu'à
`daemon.cpp:541`, **dans la branche `EPOLLIN` du client**. Une sortie décidée ailleurs
n'aurait effet qu'à la prochaine frappe.

> **Correctif :** évaluer `session.wants_quit()` en **fin de corps de boucle**,
> inconditionnellement.

**e) Ne redémarrer que sur succès.** L'état `RestartPending` n'est atteint que si
l'installation a réussi. Un `apply-failed` ne ferme donc **rien** — la révision 1 laissait
l'utilisateur perdre ses fenêtres pour une mise à jour qui n'avait pas eu lieu.

**f) Un seul client.** `daemon.cpp:144` porte un unique `std::unique_ptr<Client>` ; une
nouvelle attache évince l'ancienne (`daemon.cpp:607`). La révision 1 disait « à chaque
client » ; c'est « au client ».

**Si le protocole a changé**, le client est l'ancien binaire et reçoit `Incompatible`
(`proto.hpp:29`) — cette fois pour de bon, puisque le démon relancé est le **neuf**.
Message clair, l'utilisateur retape `sshos`.

---

## 8. Ce qui rate, et ce qu'on en fait

| Panne | Conséquence |
|---|---|
| **Pas de réseau** | `check-failed`. Une vérification **automatique** qui échoue ne dit **rien** : ni pastille, ni message. Une vérification **manuelle** affiche la raison dans une `Modal`. |
| **Compilation cassée ou suite rouge** | `apply-failed` + résumé assaini (§4.1). **Rien n'est installé**, rien n'est fermé. |
| **SHA256 non concordant** | Abandon, `apply-failed`. |
| **Binaire qui ne se charge pas** | Détecté avant installation (§5.2), repli sur l'échelon 3 **à la première installation seulement** (§5.5). |
| **`main` réécrit** | `history-rewritten` → `Reinstaller depuis GitHub` (§5.1). |
| **Le nouveau binaire démarre mal** | `sshos.previous` + `tools/update.sh --rollback`. **Pas de retour arrière automatique** : deviner qu'un démon « va mal » est une heuristique qui se trompe. |
| **Démon redémarré pendant une application** | `status=applying` + `pid` mort → `Failed`, message *« interrompu »* (§4.3). |
| **Script bloqué** | Délais du §6.6, puis `Failed`. |
| **`git` disparaît après coup** | `check-failed` avec sa raison. |
| **Disque plein** | `state.tmp` échoue → `rename()` n'a pas lieu → l'ancien état reste valide. C'est la vertu de l'écriture atomique. |

### 8.1 `--rollback` doit dire la vérité

Il restaure `sshos.previous`, **et réécrit `installed_commit` depuis `previous_commit`**,
puis pose `status=available`. Sans cela, l'état continuerait d'annoncer le commit neuf, la
vérification suivante répondrait `up-to-date`, et l'utilisateur ne pourrait **plus jamais**
réappliquer la mise à jour dont il vient de revenir.

**Il n'y a qu'un seul niveau de retour** : `sshos.previous` est écrasé à chaque mise à
jour. Écrit ici pour que personne n'en attende deux.

### 8.2 La pose du binaire — `rename()`, jamais une écriture en place

La révision 1 ne le spécifiait que pour le fichier d'état.

> Écrire en place (`cp`, `install -m755`) sur `~/.local/libexec/sshos` pendant qu'un démon
> l'exécute donne **ETXTBSY à tous les coups** : Linux refuse `open(O_WRONLY)` sur un
> exécutable en cours d'exécution. « Installer maintenant » échouerait dans le seul cas où
> on l'utilise. Deux `mv` successifs, eux, ouvrent une fenêtre pendant laquelle le chemin
> **n'existe pas**.

La séquence, mot pour mot :

```sh
cp    "$exe" "$exe.previous"      # COPIE, pas mv
cp    "$neuf" "$exe.new"          # meme repertoire, donc meme systeme de fichiers
chmod 0755    "$exe.new"
mv -f "$exe.new" "$exe"           # rename() : atomique, l'ancien inode reste vivant
```

Même discipline pour `--rollback`.

---

## 9. `tools/install.sh`

Interactif. Il ne décide rien à la place de l'utilisateur et **ne modifie aucun fichier de
configuration sans un oui explicite**.

**État des lieux d'abord**, avec un message précis sur ce qui manque : compilateur C++20 et
`cmake` ≥ 3.20 (`CMakeLists.txt:1`), `git`, `curl`/`wget`, `tar`.

**Quatre questions :**

1. **Où installer ?** `~/.local` par défaut.
2. **`~/.local/bin` est-il dans le `PATH` ?** Sinon, la ligne exacte à ajouter. Il ne
   touche pas au profil.
3. **Faire survivre le démon à la déconnexion complète ?** `loginctl enable-linger` :
   proposé, expliqué, posé seulement sur un oui.
4. **Nom d'instance**, défaut `local`.

### 9.1 Un préfixe système désactive la mise à jour depuis le bureau

Si l'utilisateur choisit `/usr/local`, l'installation se fait, et `status=updates-disabled`.

> **Pourquoi, et c'est une décision.** La révision 1 présentait `sudo` comme un détail de
> droits d'écriture. En réalité, relancer l'installeur sous `sudo` ferait tourner **en
> root** le `clone`/`curl`, la configuration `cmake` — qui exécute le `CMakeLists.txt`
> distant — la compilation **et la suite complète**, dont des cas qui forkent, ouvrent des
> PTY et envoient des signaux. Tout le §5.0 s'appliquerait alors avec les droits root.
> Pire : si l'arbre de compilation restait dans le home de l'utilisateur, root compilerait
> dans un répertoire que n'importe quel processus de cet utilisateur peut modifier entre
> deux exécutions — une élévation de privilèges offerte.
>
> On ne construit donc pas ce chemin. Installation système = installation seule ; les mises
> à jour se font en relançant l'installeur à la main.

### 9.2 Ce qu'il pose

```
~/.local/bin/sshos               le lanceur (§3.1)
~/.local/libexec/sshos           le vrai binaire
~/.local/libexec/sshos.previous  l'avant-dernier, pour --rollback
~/.local/libexec/sshos-update    tools/update.sh   <-- la revision 1 l'oubliait
~/.local/share/sshos/src/        l'arbre de compilation (echelons 1 et 3)
~/.local/share/sshos/golden/     les references des tests (echelon 2, §5.2)
~/.local/share/sshos/state       le fichier d'etat (§4)
~/.local/share/sshos/lock        le verrou (§6.6)
~/.local/share/sshos/update.log  la sortie des enfants (§6.5)
```

**`sshos-update` est mis à jour par la mise à jour elle-même** — sinon un script figé
piloterait un binaire qui a évolué.

**État initial** : `status=up-to-date`, `checked_at=<instant de l'installation>`.
L'installeur **vient** de vérifier ; écrire `0` ferait partir une vérification 30 s après
la première ouverture, écrire autre chose ferait attendre 24 h.

L'installeur est **idempotent** et prend le `flock` du §6.6.

---

## 10. `.github/workflows/release.yml`

À chaque poussée sur `main` : compilation, **suite complète au vert**, puis publication
d'une release nommée par le commit exact, portant `sshos`, `sshos_tests`,
**`golden.tar.gz`** et `SHA256SUMS`.

**Conteneur épinglé `ubuntu:26.04`**, pas `ubuntu-latest` : la cible est le système de
l'auteur (§2), et `ubuntu-latest` dérive au fil du temps. Rien ne touche `CMakeLists.txt`.

> **Le critère est « zéro échec », jamais un compte de cas.** La révision 1 gravait
> « 1146 tests » six fois ; ce travail même en ajoute, et `REPRISE` §2 avertit que ce total
> *« périme à chaque commit qui ajoute un cas, et il a déjà menti deux fois »*. Aucun
> script, aucun workflow ne compare un nombre.

> **Risque connu et accepté.** Ce projet a des tests sensibles à l'environnement —
> chronologie de `SIGSTOP`, récolte des enfants, survie du démon. Une passe de réglage sera
> probablement nécessaire en conteneur. **La sécurité est intacte pendant ce temps :** tant
> qu'aucune release n'est publiée, l'échelon 2 ne se déclenche pas.

---

## 11. Stratégie de test

Rythme du projet : **tests écrits d'abord et rouge constaté**, commit `wip(...)` **avant**
la campagne de mutation (l'arbre doit être propre pour que la sauvegarde soit fiable),
un cas par survivante ou une équivalence déclarée sur place, un commit par tâche, message
**en français sans accents**.

### 11.1 `UpdateService`, seul

Sans réseau, sans `fork`, via les trois coutures du §6.7 :

- fichier d'état **absent**, **vide**, **tronqué**, `schema` inconnu, clé inconnue, ligne
  sans `=`, **clé en double** (la première gagne), fichier **> 4 Kio** ;
- `checked_at` **dans le futur**, **négatif**, et **`9223372036854775807`** — valide mais
  absurde, donc non couvert par « illisible » ;
- le reliquat borné à `[0, 24 h]`, aux deux bornes ;
- `status=applying` avec un **pid vivant** puis un **pid mort** (§4.3) ;
- le libellé **et le drapeau `enabled`** pour chacun des sept états, plus les deux libellés
  particuliers ;
- l'enfant meurt avec un code non nul **sans avoir modifié l'état** → `Failed` ;
- `message` contenant des octets de contrôle : le service ne les rencontre jamais, mais le
  test le prouve plutôt que de le supposer.

### 11.2 L'intégration à la session

- l'entrée apparaît, change de libellé, et **devient inerte** en `Checking`/`Applying` ;
- `update:check`, `update:apply` et `update:restart` sont routés depuis les **trois** sites
  d'ouverture du menu (`session.cpp:249`, `:766`, `:828`) ;
- la pastille est peinte **et cliquable** (`PanelHit::Update`), seulement en `Available` et
  `RestartPending` ;
- **le récolteur du démon appelle bien `UpdateService::on_child_exit`.** `REPRISE` §9 bis
  n° 10 : *« un test qui appelle la méthode lui-même ne teste jamais son appelant »*. C'est
  exactement le câblage né sans appelant quatorze fois dans ce projet, et celui dont dépend
  toute la sortie de `Applying`.

### 11.3 La sonde bout-en-bout de `tools/update.sh`

C'est la leçon la plus chère du projet : quatorze défauts n'ont été trouvés ni par les
tests unitaires ni par la relecture, seulement par une sonde (`REPRISE` §9 bis). Les trois
inversions du §13 sont d'ailleurs **indétectables** par un test unitaire écrit d'après la
spec : elles demandent un vrai redémarrage.

La sonde monte un faux dépôt git local, fait avancer son `main`, et vérifie : détection,
écriture atomique, **refus d'installer sur suite rouge**, `flock` (deux applications
concurrentes), `sshos.previous` intact, `--rollback` qui réécrit `installed_commit`,
`history-rewritten` sur un `main` réécrit, et la séquence `cp`/`rename` du §8.2 pendant
qu'un binaire s'exécute.

> ⚠️ **Isolation de la sonde — la révision 1 se trompait de mécanisme.** Elle affirmait que
> « la sonde tourne sous son propre `SSHOS_BOOT_ID`, donc elle ne peut structurellement pas
> toucher le bureau installé ». **Faux** : `SSHOS_BOOT_ID` n'isole que le **nom du socket**
> (`net.cpp:118`). Le fichier d'état, `src/`, `libexec/sshos` et `sshos.previous` sont
> adressés par `$HOME` — une sonde menée à son terme **écraserait le vrai binaire de
> l'utilisateur**.
>
> **Correctif :** `update.sh` lit son préfixe d'une **variable**, jamais de `~` en dur, et
> la sonde s'exécute avec `HOME` **et** ce préfixe pointés sur un répertoire temporaire, en
> plus de son `SSHOS_BOOT_ID`.

### 11.4 Ce que les tests ne doivent jamais faire

Toucher au vrai `~/.local`, au vrai dépôt GitHub, au réseau, ni au bureau de
l'utilisateur. Tout passe par les chemins injectés.

*(Invariant à maintenir : `tests/test_net.cpp:36` et `tests/test_tty.cpp:59` génèrent déjà
des noms de socket aléatoires dans un espace séparé, et `EnvVarGuard` neutralise
`SSHOS_BOOT_ID`. C'est ce qui rend sûr de lancer `sshos_tests` depuis un enfant du démon —
à ne pas casser, puisque le §5.2 en fait une porte d'installation.)*

---

## 12. Hors périmètre

- **Préserver les fenêtres et les shells à travers un redémarrage.** Il faudrait sérialiser
  l'état du démon et transmettre les descripteurs de PTY par `exec`. Jalon à part entière ;
  l'utilisateur choisit son moment.
- **Le retour arrière automatique** (§8).
- **Les mises à jour depuis une branche autre que `main`.**
- **Toute compatibilité avec des systèmes plus anciens** (§2).
- **Une signature détachée des releases** (§5.0) — décision, pas oubli.
- **La mise à jour depuis le bureau pour un préfixe système** (§9.1).
- **Un menu contextuel sur la barre des tâches.** `REPRISE` §7.4 et §10 le listent comme
  ouvert ; `PanelHit::Body` tombe encore dans `default: break;` (`session.cpp:815`). Ce
  travail ajoute un élément à la barre sans ouvrir ce chantier.

---

## 13. Ce que la révision 1 affirmait, et qui était faux

Table de correspondance, pour qu'aucune de ces erreurs ne revienne par la bande.

| # | Affirmation de la révision 1 | Réalité vérifiée | Corrigé au |
|---|---|---|---|
| 1 | Le lanceur par défaut est `spawn_detached` | Double fork : rend le pid de l'intermédiaire, le vrai travail n'est **jamais** récoltable (`daemonize.cpp:48-55`) | §6.5 |
| 2 | « Installer et redemarrer » relance le neuf | `/proc/self/exe` désigne l'**inode** : relancerait l'**ancien** binaire, en silence (`main.cpp:40`) | §7.4a |
| 3 | Le `--kill` de dev ne peut **pas** atteindre le bureau installé | `SSHOS_BOOT_ID` est hérité par tous les shells du bureau (`env.cpp:16-36`) | §3.2 |
| 4 | La suite passe au vert en mode binaire | `test_golden.cpp:42-46` grave `__FILE__` : 9 cas échouent par construction | §5.2 |
| 5 | Une vérification quotidienne bat toute seule | Sans client, `epoll_wait` bloque indéfiniment (`daemon.cpp:298-300, 319`) | §6.4 |
| 6 | `SHA256SUMS` vérifie le téléchargement | Intégrité du **transport** seulement ; et le vert s'obtient **après** avoir exécuté la charge | §5.0 |
| 7 | Le repli 2 → 3 est une sûreté | Descente d'un canal vérifié vers un canal non vérifié, déclenchable à volonté | §5.5 |
| 8 | `installed ≠ remote` ⇒ mise à jour | Aucun contrôle de descendance ; ce projet a force-poussé `main` deux fois | §5.1 |
| 9 | *(pose du binaire non spécifiée)* | Écriture en place = **ETXTBSY** garanti | §8.2 |
| 10 | « Un seul enfant à la fois » | Aucun verrou, chemins fixes : la course détruit `sshos.previous` | §6.6 |
| 11 | `message` est du texte libre d'une ligne | Un `\n` **forge une paire clé=valeur** | §4.1 |
| 12 | La confirmation à trois boutons existe déjà | `Modal` est binaire, une ligne, libellés en dur (`modal.hpp:11`, `modal.cpp:8-14`) | §7.3 |
| 13 | Le menu « reçoit un libellé » | `Menu::open()` construit `all_` seul, aucun setter (`menu.cpp:32-54`) | §7.1 |
| 14 | *(la pastille)* | Peinte, non cliquable : viole « la souris d'abord » | §7.2 |
| 15 | *(l'installeur)* | Ne posait jamais `update.sh` : le bouton n'avait rien à lancer | §9.2 |
| 16 | `/usr/local` n'est qu'une question de droits | Compilation **et suite complète en root**, arbre de build non spécifié | §9.1 |
| 17 | `on_child_exit(status)` | Le contrat du projet est `(pid_t, int)` (`reap.hpp:12`) | §6.5 |
| 18 | Le routage du menu est en `session.cpp:204` | C'est le site d'appel ; le routage est en `:150-187`, et il y a un 5ᵉ domaine, `cmd:` | §7.1 |
| 19 | *(l'horloge)* | Aucune couture ; or `Platform` existe déjà et impose `steady_now()` | §6.3 |
| 20 | La sonde de test est isolée par `SSHOS_BOOT_ID` | Il n'isole que le socket : la sonde écraserait le vrai binaire | §11.3 |
| 21 | La sonde du binaire est indépendante de l'environnement | `main.cpp:69-75` sort en **1** avant de lire `argv` | §5.2 |
| 22 | Après « Installer maintenant », tout est cohérent | La pastille s'éteignait alors que l'ancienne version tournait toujours | §6.1 |
| 23 | « à chaque client » | Il n'y en a jamais qu'un (`daemon.cpp:144`) | §7.4f |
