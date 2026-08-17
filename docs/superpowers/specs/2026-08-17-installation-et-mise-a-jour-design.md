# ssh_os 2.0 — installation locale et mise à jour depuis le bureau

> Conception arrêtée le 17 août 2026, avec l'utilisateur, avant toute implémentation.
> Le §2 de ce document liste ce qui n'est pas négociable ; le reste en découle.

---

## 1. Ce qu'on construit, et pourquoi

Aujourd'hui, `ssh_os` ne se lance que depuis l'arbre de développement, par
`./build-release/sshos`. Conséquence vécue : **recompiler ferme le bureau de
l'utilisateur**, puisque c'est le même binaire et le même socket.

L'objectif est d'avoir **deux instances qui s'ignorent** :

- une **installée**, stable, qui sert de poste de travail quotidien — c'est elle qui
  héberge la session Claude qui travaille sur le projet ;
- celle de **développement**, qu'on casse et qu'on recompile sans conséquence.

Et, dans l'instance installée, un mécanisme de mise à jour : une **vérification
automatique une fois par jour**, une **pastille** dans la barre des tâches quand une
version attend, et **une entrée de menu qui change de libellé** selon l'état.

---

## 2. Ce qui n'est pas négociable

Les contraintes du §4 de `docs/REPRISE.md` s'appliquent intégralement. Trois d'entre
elles décident de presque tout ce qui suit, et une quatrième est ajoutée ici.

| Contrainte | Ce qu'elle interdit dans ce travail |
|---|---|
| **`CMakeLists.txt` intouchable** | Pas de cible `install()`, pas de `configure_file`, pas de `-D` pour graver une version dans le binaire. |
| **Un thread, un `epoll`, aucun mutex** | Le démon ne bloque jamais. Ni `git`, ni `cmake`, ni la moindre lecture réseau dans son fil d'exécution. |
| **Zéro dépendance externe (C++)** | Le fichier d'état est en clé=valeur, analysé à la main. Pas de JSON, pas de bibliothèque. |
| **Aucun code de compatibilité** *(ajoutée ici)* | L'application est écrite pour le système de son auteur. Pas de `#ifdef` pour de vieilles glibc ou de vieux noyaux, pas de repli pour un compilateur ancien. Un système plus ancien **compile depuis les sources** (échelon 3, §5) ; ça ne coûte aucune ligne de maintenance. |

> **Pourquoi la quatrième.** L'auteur écrit cette application d'abord pour lui. Choisir
> une image de compilation ancienne pour élargir la compatibilité binaire reviendrait à
> lui interdire les fonctionnalités que son propre système offre. Le compromis est
> refusé explicitement.

**Un point de mécanique qui aide :** `CMakeLists.txt` utilise
`file(GLOB_RECURSE … CONFIGURE_DEPENDS)` sur `src/*.cpp` et `tests/test_*.cpp`. Les
nouveaux fichiers de ce travail sont donc pris **automatiquement**, sans y toucher. La
contrainte est respectée sans effort.

---

## 3. L'isolation entre l'installé et le développement

### Le levier existe déjà

`src/common/net.hpp:29` définit `kBootIdEnvVar = "SSHOS_BOOT_ID"`, consultée **en
priorité** par `read_boot_id()` avant tout accès à `/proc`. Le nom du socket abstrait
est `sshos/<uid>/<boot_id>` (`net.cpp:118`). Changer la variable change le socket, donc
sépare deux instances qui ne se voient plus.

Le commentaire de `net.hpp:22` pose le contrat : *« qui l'exporte doit lui donner la
même valeur pour le démon et pour tous les clients censés pouvoir s'y rattacher »*.

### Comment on le tient

`~/.local/bin/sshos` **n'est pas le binaire** : c'est un lanceur de trois lignes.

```sh
#!/bin/sh
SSHOS_BOOT_ID="${SSHOS_BOOT_ID:-local}"
export SSHOS_BOOT_ID
exec "$HOME/.local/libexec/sshos" "$@"
```

Le vrai binaire vit dans `~/.local/libexec/sshos`. Le démon est relancé par
`spawn_detached({"/proc/self/exe", "--daemon"})` (`main.cpp:40`) : `/proc/self/exe`
désigne le vrai binaire, et **l'environnement est hérité**, donc démon et clients
calculent le même nom. Le contrat de `net.hpp:22` est tenu par construction, pas par
discipline.

**Ce que ça achète :** `./build-release/sshos --kill` lancé dans l'arbre de dev calcule
le nom par défaut et **ne peut pas atteindre** le bureau installé. Le piège le plus
coûteux du projet — un outil de dev qui tue la session de travail — devient
structurellement impossible.

`${SSHOS_BOOT_ID:-local}` plutôt qu'une affectation sèche : l'utilisateur peut faire
tourner une seconde instance nommée autrement sans modifier le lanceur.

---

## 4. Le fichier d'état

`~/.local/share/sshos/state`, en clé=valeur, une paire par ligne :

```
schema=1
source=git
installed_commit=4de7722a8f1c9b0e5d3a2f6c8b1e4d7a0c3f6b9e
remote_commit=6849b6bc7f99c06a5a77585ddf008cb83c0a5133
checked_at=1755400000
status=available
message=
```

| Clé | Valeurs / sens |
|---|---|
| `schema` | `1`. Un `schema` inconnu est traité comme un fichier absent — jamais interprété au hasard. |
| `source` | `git` \| `release` \| `archive` \| `local` — le canal retenu à l'installation (§5). |
| `installed_commit` | Empreinte complète, ou `unknown` (§5.5). |
| `remote_commit` | Ce que la dernière vérification a vu, ou vide. |
| `checked_at` | Époque Unix, secondes. |
| `status` | `idle` \| `up-to-date` \| `available` \| `check-failed` \| `apply-failed` \| `updates-disabled` |
| `message` | Texte libre destiné à l'utilisateur, vide si rien à dire. Une seule ligne. |

**Qui écrit quoi.** `tools/update.sh` et `tools/install.sh` **écrivent** ; le C++ ne fait
que **lire**. C'est ce qui garde `git`, le réseau et `cmake` hors du démon.

**L'écriture est atomique** : le script écrit `state.tmp` puis `rename()`. Le démon ne
peut donc jamais lire un fichier à moitié écrit — `rename()` est atomique sur le même
système de fichiers, et le fichier temporaire est créé dans le même répertoire pour le
garantir.

**L'analyse est tolérante mais jamais devineresse** : une clé inconnue est ignorée, une
valeur illisible ramène le champ à son défaut, un fichier absent ou de `schema` inconnu
donne l'état `Idle` sans message. Une ligne sans `=` est ignorée. Aucun cas ne lève.

---

## 5. L'échelle d'acquisition

L'installeur et la mise à jour descendent la même échelle, dans cet ordre. Le premier
échelon praticable gagne, et il est inscrit dans `source=`.

| # | Condition | Ce qu'on fait | `source=` |
|---|---|---|---|
| **1** | `git` disponible | `clone` / `fetch` depuis GitHub, compilation, **1146 tests**, installation | `git` |
| **2** | pas de `git`, mais `curl`/`wget` | binaire de la dernière Release, SHA256 vérifié, **démarrage éprouvé** | `release` |
| **3** | pas de binaire exploitable | archive des sources, compilation, **1146 tests** | `archive` |
| **4** | rien de joignable | l'arbre local d'où l'installeur est lancé (zip déplié compris) | `local` |

### 5.1 Échelon 1 — git

`git clone https://github.com/might-stormlord/sshos.git ~/.local/share/sshos/src`, puis
`git fetch` aux mises à jour suivantes. **Jamais une copie de l'arbre de développement**
de l'utilisateur : celui-ci est justement celui qui est en chantier.

La vérification est `git ls-remote origin main`, qui rend l'empreinte distante sans rien
télécharger — une seconde environ.

### 5.2 Échelon 2 — le binaire publié

`https://api.github.com/repos/might-stormlord/sshos/releases/latest` donne les URL des
pièces jointes ; on télécharge `sshos`, `sshos_tests` et `SHA256SUMS`, et **on vérifie
les empreintes avant tout**.

**Puis on éprouve le binaire avant de l'installer.** La sonde est un **appel avec un
drapeau inconnu** : `main.cpp:109-111` répond alors `usage: sshos [--daemon|--status|--kill]`
sur la sortie d'erreur et rend **2**. C'est déterministe et **indépendant de l'état du
système** — succès si et seulement si le code de retour est 2 et que la ligne `usage:`
apparaît.

> **Ne pas sonder avec `--status`.** Il rend **1** quand aucun démon ne tourne et **0**
> quand il y en a un : son code de retour dépend de l'environnement, pas du binaire. Le
> prendre pour preuve de bon chargement donnerait un faux négatif à chaque bureau
> éteint.

Un binaire qui ne se charge pas — glibc plus ancienne que celle de la CI, autre
architecture — se signale par un refus de l'éditeur de liens dynamique (code 126 ou 127,
ou mort par signal) et fait **retomber sur l'échelon 3**. C'est cette garde qui autorise
le §2 à refuser tout code de compatibilité : se tromper de cible ne casse rien, ça
compile.

`sshos_tests` est publié en même temps, ce qui permet de faire tourner les **1146 tests
même en mode binaire**. La propriété de sûreté — *on n'installe qu'après le vert* —
tient donc sur les quatre échelons, pas seulement sur ceux qui compilent.

### 5.3 Échelon 3 — l'archive

`https://codeload.github.com/might-stormlord/sshos/tar.gz/refs/heads/main`, déplié en
absorbant le répertoire de tête `sshos-main/` que GitHub ajoute. Puis compilation et
tests, comme l'échelon 1.

### 5.4 Échelon 4 — l'arbre local

Ni `git` ni HTTPS. L'installeur **installe quand même** depuis l'arbre où on l'a lancé,
mais écrit `status=updates-disabled` avec la raison. L'entrée de menu devient
`Mise a jour indisponible (git absent)`, inerte.

> Faire semblant de vérifier serait le pire comportement possible : l'utilisateur
> croirait être à jour.

### 5.5 Le commit inconnu

Un zip déplié n'a pas de `.git` : `installed_commit=unknown`. Le vérificateur ne peut
alors comparer avec rien, **et ne prétend pas le faire**. L'entrée devient
`Reinstaller depuis GitHub` — une installation propre depuis `main`, qui remplace
l'inconnu par un commit connu. À partir de là, le mécanisme normal reprend.

---

## 6. `UpdateService` — la machine à états

`src/shell/update_service.{hpp,cpp}`. Sans interface, sans réseau, sans `git`. Elle sait
trois choses : quand relancer une vérification, comment lancer un enfant, et comment lire
le fichier d'état.

### 6.1 Les six états

| État | Libellé du menu | Pastille |
|---|---|---|
| `Idle` | `Verifier les mises a jour` | non |
| `Checking` | `Verification en cours...` *(inerte)* | non |
| `UpToDate` | `Verifier les mises a jour` | non |
| `Available` | **`Mettre a jour`** | **oui** |
| `Applying` | `Mise a jour en cours...` *(inerte)* | non |
| `Failed` | `Verifier les mises a jour` | non |

Deux libellés particuliers se substituent au premier : `Reinstaller depuis GitHub` quand
`installed_commit=unknown` (§5.5), et `Mise a jour indisponible (…)` quand
`status=updates-disabled` (§5.4).

**Sans accents**, pour rester cohérent avec les entrées déjà présentes
(`Ranger les fenetres`, `Fermer la session`, `menu.cpp:46-52`).

### 6.2 Les transitions

- **Au démarrage du démon.** Si `checked_at` remonte à plus de 24 h, une vérification est
  planifiée peu après le démarrage ; sinon elle l'est pour le reliquat. On ne vérifie pas
  à chaque lancement : un démon relancé souvent martèlerait le réseau pour rien.
- **Toutes les 24 h.** L'horloge est celle du démon, réveillée par `on_refresh()`.
- **`Verifier les mises a jour`** → `Checking` immédiatement.
- **`Mettre a jour`** → la confirmation (§7.3), puis `Applying`.
- **Mort de l'enfant** → relecture du fichier d'état, puis l'état qui en découle.

### 6.3 Comment elle lance un enfant sans bloquer

**`UpdateService` n'est pas une `App`** : c'est un service de session, il n'a donc ni
`Host` ni `on_child_exit`. Il ne peut pas non plus récolter lui-même — le commentaire de
`app.hpp:49` dit pourquoi : *« `waitpid` est global au processus, et deux applications
qui appelleraient `waitpid(-1)` chacune de leur côté se voleraient mutuellement leurs
enfants »*. La règle vaut identiquement ici.

Le service porte donc **un lanceur injecté** (§6.4), une fonction qui rend un `pid_t`.
En production, la session fournit un lanceur qui appelle `spawn_detached` **et inscrit
le pid auprès du récolteur unique du démon** — celui qui sert déjà `Host::watch_child`
(`app.hpp:52`). Quand l'enfant meurt, la session appelle
`UpdateService::on_child_exit(status)`. Le service ne connaît donc ni `fork` ni
`waitpid` ; il ne connaît qu'un `pid_t` qu'on lui rend et un statut qu'on lui annonce.

La compilation et les 1146 tests — une à deux minutes de calcul — vivent donc **dans un
processus enfant**, jamais dans le démon.

### 6.4 Ce qui la rend testable

Deux points d'injection, sur le modèle de `read_boot_id(boot_id_path)` qui a une valeur
par défaut pointant sur le vrai chemin *« pour permettre aux tests de substituer un
chemin absent sans dépendre de l'état réel de la machine »* (`net.hpp:44`) :

- **le chemin du fichier d'état**, par défaut `~/.local/share/sshos/state` ;
- **le lanceur d'enfant**, une fonction rendant un `pid_t`, par défaut le vrai
  `spawn_detached`.

Un test fabrique donc un fichier d'état arbitraire et simule la mort d'un enfant, sans
réseau, sans `fork`, sans toucher au vrai `~/.local`.

---

## 7. L'intégration au bureau

### 7.1 Le menu

`menu.cpp:40-52` construit ses entrées avec des identifiants préfixés par domaine :
`app:`, `panel:`, `wm:`, `session:`. On ajoute le domaine `update:` :

- `update:check` — lancer une vérification ;
- `update:apply` — ouvrir la confirmation.

`session.cpp:204` interprète déjà l'identifiant rendu par `menu_.selected()` ; les deux
nouveaux cas s'y ajoutent à côté de `session:*` et `wm:*`. **Le menu n'apprend rien du
service** : il reçoit un libellé et rend un identifiant, exactement comme le
commentaire de `menu.hpp:23` le décrit — *« il ne fait rien lui-même : il rend un
identifiant, et la session sait ce qu'il veut dire »*.

Une seule entrée est présente à la fois ; c'est son libellé qui change (§6.1).

### 7.2 La pastille

Dans la barre des tâches, à l'état `Available` uniquement. Elle disparaît dès que la
mise à jour est appliquée ou que l'état retombe. **Un échec de vérification automatique
n'affiche rien** (§8).

### 7.3 La confirmation

Le clic sur `Mettre a jour` ouvre une confirmation — le mécanisme existe déjà pour
« Fermer la session ». Elle **compte ce qui va mourir** :

```
Mise a jour disponible.
3 fenetres, 2 shells actifs seront fermes si vous redemarrez.

[ Installer maintenant ]  [ Installer et redemarrer ]  [ Annuler ]
```

- **Installer maintenant** — le nouveau binaire est posé, rien n'est fermé ; il prendra
  effet au prochain démarrage du démon.
- **Installer et redemarrer** — installation puis redémarrage (§7.4).

C'est ici que vit le choix du redémarrage, et pas dans le menu : il n'a de sens qu'une
fois qu'une version attend.

### 7.4 Le redémarrage

`proto.hpp:30` définit déjà `Detached { std::string reason; }`. Le démon envoie à chaque
client un `Detached` dont la raison marque une mise à jour, puis sort.

Le client, en voyant **cette raison-là**, ne rend pas la main au shell : il **rejoue le
chemin de démarrage** de `main.cpp:114` — tenter la connexion, relancer le démon s'il
n'y en a pas, se rattacher. Une seule fois : si ça échoue, il sort avec un message.

**Si le protocole a changé entre-temps**, le client de l'utilisateur est l'ancien binaire
et reçoit `Incompatible` (`proto.hpp:29`) — comportement déjà implémenté, message clair,
l'utilisateur retape `sshos`. Rien à inventer pour ce cas.

---

## 8. Ce qui rate, et ce qu'on en fait

| Panne | Conséquence |
|---|---|
| **Pas de réseau** | `status=check-failed`. Une vérification **automatique** qui échoue ne dit **rien** : ni pastille, ni message. Elle ne doit pas harceler. Une vérification **manuelle** affiche la raison. |
| **Compilation cassée ou tests rouges** | `status=apply-failed` avec le résumé. **Rien n'est installé** ; le binaire en place n'est pas touché. C'est la propriété de sûreté centrale. |
| **Binaire téléchargé qui ne démarre pas** | Détecté avant installation (§5.2), repli sur l'échelon 3. |
| **SHA256 qui ne correspond pas** | Abandon immédiat, `apply-failed`. On n'installe pas un téléchargement qu'on n'a pas pu vérifier. |
| **Le nouveau binaire démarre mal malgré tout** | L'ancien est conservé en `~/.local/libexec/sshos.previous` ; `tools/update.sh --rollback` le remet. **Pas de retour arrière automatique** : deviner qu'un démon « va mal » est exactement le genre d'heuristique qui se trompe. |
| **`git` disparaît après l'installation** | `check-failed` avec sa raison. La mise à jour reste possible en repassant par l'installeur. |
| **Deux mises à jour lancées coup sur coup** | Les états `Checking` et `Applying` rendent l'entrée inerte ; un seul enfant à la fois. |

---

## 9. `tools/install.sh`

Interactif. Il ne décide rien à la place de l'utilisateur et **ne modifie aucun fichier
de configuration sans un oui explicite** — le §4 de `REPRISE.md` l'interdit.

**L'état des lieux d'abord**, avec un message précis sur ce qui manque plutôt qu'un
échec au milieu d'une compilation : compilateur C++20 et `cmake` ≥ 3.20 (requis sauf si
l'échelon 2 aboutit), `git`, `curl`/`wget`, `tar`.

**Ses quatre questions :**

1. **Où installer ?** `~/.local` par défaut. Sur `/usr/local`, il prévient que le bouton
   de mise à jour, qui tourne avec les droits de l'utilisateur, **n'aura pas le droit
   d'y écrire**, et que chaque mise à jour se fera en relançant l'installeur avec `sudo`.
2. **`~/.local/bin` est-il dans le `PATH` ?** Sinon il donne la ligne exacte à ajouter.
   Il ne touche pas au profil.
3. **Faire survivre le démon à la déconnexion complète ?** C'est `loginctl
   enable-linger`, donc de la configuration système : proposé, expliqué, posé seulement
   sur un oui.
4. **Nom d'instance**, défaut `local` — la valeur de `SSHOS_BOOT_ID`.

**Ce qu'il pose :**

```
~/.local/bin/sshos              le lanceur (§3)
~/.local/libexec/sshos          le vrai binaire
~/.local/libexec/sshos.previous l'avant-dernier, pour --rollback
~/.local/share/sshos/src/       l'arbre de compilation (échelons 1 et 3)
~/.local/share/sshos/state      le fichier d'état (§4)
```

Relancer l'installeur met simplement à jour. Il est **idempotent**.

---

## 10. `.github/workflows/release.yml`

À chaque poussée sur `main` : compilation, **1146 tests**, et publication **seulement si
tout est vert**, d'une release nommée par le commit exact, portant `sshos`,
`sshos_tests` et `SHA256SUMS`.

**Le conteneur est épinglé à `ubuntu:26.04`**, pas `ubuntu-latest`. Deux raisons :

1. **La cible est le système de l'auteur**, pas un plus petit dénominateur commun. Une
   image ancienne signifierait un compilateur et des en-têtes anciens, donc des
   fonctionnalités interdites dans sa propre application — ce que le §2 refuse.
2. **`ubuntu-latest` dérive.** GitHub le fait pointer ailleurs au fil du temps ;
   l'épinglage garde la cible stable sans surveillance.

Rien de tout cela ne touche `CMakeLists.txt`.

> **Risque connu et accepté.** Ce projet a des tests sensibles à l'environnement —
> chronologie de `SIGSTOP`, récolte des enfants, survie du démon. Ils passent sur la
> machine de l'auteur ; rien ne garantit qu'ils passent du premier coup dans un
> conteneur de CI. Une passe de réglage sera probablement nécessaire. **La sécurité est
> intacte pendant ce temps :** tant qu'aucune release n'est publiée, l'échelon 2 ne se
> déclenche pas et l'installeur compile (échelon 1 ou 3).

---

## 11. Stratégie de test

Le rythme du projet s'applique : **tests écrits d'abord et rouge constaté**, campagne de
mutation, un cas ajouté par survivante ou une équivalence déclarée sur place, commit par
tâche.

### 11.1 `UpdateService`, seul

Sans réseau, sans `fork`, via les deux injections du §6.4 :

- fichier d'état **absent**, **vide**, **tronqué au milieu d'une ligne**, `schema`
  inconnu, clé inconnue, ligne sans `=`, valeur numérique illisible ;
- `checked_at` **dans le futur** (horloge reculée) — ne doit pas produire une attente
  négative ni une vérification en boucle ;
- l'arithmétique des 24 h, aux bornes ;
- le libellé rendu pour **chacun des six états**, plus les deux libellés particuliers
  du §6.1 ;
- la transition sur mort d'enfant pour chaque `status` possible ;
- l'entrée reste **inerte** en `Checking` et `Applying`.

### 11.2 L'intégration à la session

Comme le menu est déjà testé : l'entrée apparaît, disparaît et change de libellé selon
l'état ; `update:check` et `update:apply` sont routés ; la pastille est peinte au bon
endroit et seulement en `Available`.

### 11.3 La sonde bout-en-bout de `tools/update.sh`

C'est la leçon la plus chère du projet : **quatorze défauts n'ont été trouvés ni par les
tests unitaires ni par la relecture**, seulement par une sonde bout-en-bout (§9 bis de
`REPRISE.md`). Le script en mérite une.

Elle monte un **faux dépôt git local**, fait avancer son `main`, et vérifie la chaîne
entière : détection d'une nouvelle version, écriture atomique de l'état, **refus
d'installer sur tests rouges**, conservation de `sshos.previous`, et `--rollback`.

**La sonde tourne sous son propre `SSHOS_BOOT_ID`**, donc elle ne peut structurellement
pas toucher le bureau installé de l'utilisateur ni celui de développement.

### 11.4 Ce que les tests ne doivent jamais faire

Toucher au vrai `~/.local`, au vrai dépôt GitHub, ou au réseau. Tout passe par les
chemins injectés.

---

## 12. Hors périmètre

Nommé pour éviter qu'il y revienne par la bande :

- **Préserver les fenêtres et les shells à travers un redémarrage.** Il faudrait
  sérialiser tout l'état du démon et transmettre les descripteurs de PTY par `exec`.
  C'est un jalon à part entière, et l'utilisateur a explicitement dit ne pas en avoir
  besoin : il choisit son moment.
- **Le retour arrière automatique.** Voir §8.
- **Les mises à jour depuis une branche autre que `main`.** `main` est la ligne stable ;
  c'est ce qui donne son sens à la vérification.
- **Toute forme de compatibilité avec des systèmes plus anciens.** Voir §2.
