# termos — dossier de reprise

> ## ⚠️ LE PROJET S'APPELLE `termos` DEPUIS LE 21 AOÛT 2026
>
> Il s'appelait `ssh_os 2.0` / `sshos`. Trois commits ont fait la bascule :
> le contrat d'installation (`c3c6c63`), la construction et la CI (`2e622b2`),
> les chaînes vues par l'utilisateur (`b09639e`).
>
> **Ce qui a changé :** la commande (`termos`), le binaire, le nom du socket
> abstrait (`\0termos/<uid>/<boot_id>`), les variables `TERMOS_*`, les chemins
> d'installation (`~/.local/bin/termos`, `~/.local/share/termos/`), les URL du
> dépôt, le libellé du panneau, la signature de fond, et les deux cibles
> exécutables de CMake (`termos`, `termos_tests`).
>
> **Ce qui N'A PAS changé, et c'est délibéré :** le namespace C++ `sshos`
> (1 978 occurrences dans 129 fichiers, invisible hors du dépôt), la cible
> `sshos_core`, `project(sshos)`, les sockets abstraits et chemins temporaires
> des tests, et **les plans et specs datés de `docs/superpowers/`** — ce sont
> des documents d'archive qui décrivent ce qui a été fait sous le nom de
> l'époque. Les réécrire falsifierait l'histoire du projet et casserait
> quatorze références croisées.
>
> ⚠️ **`kBanned` (`src/pty/env.cpp`) liste les QUATRE noms**, anciens compris.
> Un lanceur `~/.local/bin/sshos` survivant dans un `PATH` suffirait sinon à
> faire descendre `SSHOS_BOOT_ID` dans chaque shell du bureau, où un binaire de
> l'époque recalculerait l'ancien nom de socket : `sshos --kill` tapé là
> tuerait la session. Gardé par
> `child_env_still_bans_the_pre_rename_desktop_identity` — **ne pas « nettoyer »
> ces deux entrées en les croyant mortes.**
>
> ⚠️ **Le nom d'un socket UNIX abstrait est figé au `bind()`, dans le noyau.**
> Il n'existe donc AUCUN chemin de migration qui préserve une session vivante à
> travers ce renommage : une installation d'avant garde son démon sur
> `\0sshos/…`, invisible au binaire neuf, qui en démarrerait un second.
> `tools/install.sh` arrête l'ancien démon puis retire ses cinq fichiers —
> l'arrêt AVANT le retrait, un démon dont on efface le binaire ne peut plus se
> relancer.

> Document destiné à un contexte neuf. Il suppose zéro connaissance préalable de la
> conversation qui a produit le projet. Tout ce qui suit a été vérifié, pas supposé :
> quand un fait vient d'une mesure, la mesure est citée.
>
> **Dernière mise à jour :** 20 août 2026, branche `m1-noyau`. **Le dépôt est publié :**
> <https://github.com/might-stormlord/sshos> — public, sous AGPL-3.0. Le §2 bis dit
> comment, et surtout ce que la publication a changé dans l'historique.
> **1311 tests au vert** en `Release` comme sous ASan/UBSan, 0 avertissement, et
> **aussi dans un conteneur `ubuntu:26.04` nu** (mesure du conteneur : 15 août,
> elle portait alors sur 1146 cas). Arbre de travail propre.
> **275 commits** sur `m1-noyau`, 124 fichiers dans `src/`, version **1.39**.
> ⚠️ **Ces quatre nombres périment à chaque commit. Les recompter, jamais les
> citer** — c'est la seule discipline qui tienne, et ce dossier a déjà menti trois
> fois sur des totaux :
> ```bash
> git rev-list --count m1-noyau                              # commits
> grep -ch '^TEST(' tests/*.cpp | awk '{s+=$1} END {print s}' # cas declares
> git ls-files 'src/*' | wc -l                               # fichiers
> sh tools/version.sh . HEAD                                 # version
> ```
> **L'installation locale et la mise à jour depuis le bureau sont livrées** — §2 ter.
> ⚠️ Les autres mesures de ce dossier datent du 15 août, sur le commit alors nommé
> `e6d013d` : elles restent justes, mais **toutes les empreintes de commit ont changé
> depuis** (§2 bis). 41 535 lignes sur 162 fichiers.
> **Les SEPT jalons sont livrés**, et le travail qui a suivi est demandé au fil de
> l'usage par l'utilisateur. Le §3 donne la position exacte.
>
> **Par où commencer, dans cet ordre :**
> **§2** compiler et lancer · **§2 bis** ⚠️ *la publication sur GitHub, faite — et la
> réécriture d'historique qu'elle a entraînée : à lire avant toute manipulation de
> git* · **§2 ter** ⚠️ *l'installation locale et la mise à jour depuis le bureau — à
> lire avant de lancer un démon ou de toucher aux chemins* ·
> **§2 quater** *la mise à jour du 20 août, faite — et la règle permanente qu'elle
> laisse : une mise à jour ne bénéficie JAMAIS du correctif qu'elle installe* ·
> **§2 quinquies** *le fait contre la conclusion : comment se lit `state`, et
> pourquoi `status` et `restart_pending` disent parfois la même chose* ·
> **§3** où l'on en est · **§3 bis** la carte du code ·
> **§4** ce qui n'est pas négociable · **§6 / §6 bis** quel fichier est né à quel
> jalon, les 124 de `src/` · **§8 bis** le rythme de travail et la campagne
> de mutation · **§8 ter** les trois outils (`tools/sonde.py`, `tools/mutation.py`,
> `tools/balayage.py`) ·
> **§9 bis** le défaut qui revient **vingt-quatre** fois dans ce projet, et son balayage ·
> **§9 ter** ce qu'un audit adversarial a trouvé — et ce qu'il a manqué ·
> **§10** le carnet de ce qui reste à faire.
> Le reste (§5 à §7, §8, §9) se lit à la demande.
>
> ⚠️ Les §4, §8 et §9 (contraintes, méthode, pièges d'environnement) ont été écrits au
> jalon 1 et restent **entièrement valides**. Le **§6** ne décrit que le jalon 1 ; le
> **§6 bis** couvre les jalons 2 à 7, fichier par fichier. Les plans de
> `docs/superpowers/plans/` restent la source de vérité de chaque tâche — sauf celui du
> jalon 2, qui n'a jamais été annoté (§6 bis).
>
> ✅ **Passe de vérification du 15 août 2026.** Tous les chiffres de ce dossier ont été
> re-mesurés sur `e6d013d` et les écarts corrigés — trois comptes de commits divergents,
> un total de tests périmé au §2 et au §5, la rétention mémoire par client, et surtout
> **le script de balayage du §9 bis, qui était incapable de trouver ce qu'on lui
> attribuait** (la démonstration est dans le §9 bis).
>
> ✅ **Passe du 20 août 2026 au soir.** Les cinq fils laissés en l'air le matin sont
> soldés, et les vérifier a sorti **trois défauts que le carnet ne connaissait pas** :
> un cadre de modale qui ne se dimensionnait que pour ses boutons par défaut — donc un
> bouton peint sur le bureau et incliquable —, une CI qui nommait « version 1.0 »
> **toutes** les releases publiées, et un verrou de mise à jour qui pouvait rester pris
> pour toujours. Deux des cinq fils étaient aussi plus larges que leur description :
> **quatre** sondes tuaient le bureau vivant, pas deux. Le détail est au §10.

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
  **Distant :** <https://github.com/might-stormlord/sshos>, public, AGPL-3.0.

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

> 🟢 **LE PROJET EST PUBLIÉ.** Il a vécu ses 205 premiers commits **sur ce seul
> disque**, sans aucun dépôt distant ; ce n'est plus le cas depuis le 17 août 2026.
> Mais la publication a **réécrit tout l'historique** (les empreintes de commit ont
> toutes changé) et a laissé **23 branches locales sur l'ancien historique, qui ne
> doivent jamais être poussées**. Le §2 bis explique — **le lire avant toute
> manipulation de git**.

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
./build-release/sshos_tests            # 36,4 s (mesure du 20 août au soir)
./build-debug/sshos_tests              # 65,0 s (ASan + UBSan ; 47,3 s le 15 août)
./build-release/sshos_tests files_     # filtre par sous-chaîne du nom
```

**Attendu : `1311 cas, 0 en echec, 0 assertions echouees`,** en Release comme en
Debug, avec 0 avertissement de compilation (`-Wall -Wextra -Wpedantic -Werror`).

> Le binaire de test s'appelle **`sshos_tests`** (pas `sshos-test`). Erreur commise
> plusieurs fois.

> ⚠️ **Ce total périme à chaque commit qui ajoute un cas, et il a déjà menti deux
> fois** — « 189 cas » puis « 1120 cas », chaque fois assez longtemps pour qu'un
> contexte neuf puisse croire avoir tout cassé. Le compter plutôt que le croire :
> `grep -ch '^TEST(' tests/*.cpp | awk '{s+=$1} END {print s}'` doit rendre le
> même nombre que la ligne de bilan de `sshos_tests`. Au 21 août : **1311**.
>
> ⚠️ **`-h`, et pas un découpage sur `:`.** La version qu'a portée ce dossier
> jusqu'au 20 août 2026 faisait `grep -c ... | awk -F: '{s+=$2}'`. Or `grep -c`
> ne préfixe le nom du fichier **que lorsqu'il en reçoit plusieurs** : sur un
> seul, il imprime le compte nu, `$2` est vide, et le total tombe
> silencieusement à **zéro**. Ça ne se voyait pas ici — `tests/` porte 57
> fichiers `test_*.cpp` — mais la même ligne, recopiée dans `tools/update.sh` pour
> dessiner une barre de progression, a rendu 0 et effacé la barre sans rien dire.
> Deuxième outil de vérification de ce dossier pris en défaut, après le balayage
> du §9 bis — qui l'a d'ailleurs été **une seconde fois** le 20 août, pour une
> raison toute différente (§9 bis). **Trois outils de vérification faux sur
> trois : c'est le motif, pas l'accident.**

### Le geste de vérification à la main

1. `./build-release/sshos`
2. `Ctrl+A` puis `Espace` ouvre le menu ; filtrer au clavier, `Entrée` lance.
3. Dans un Terminal, poser une marque : `MARQUE=persiste`.
4. **Fermer la fenêtre du terminal** — ou `Ctrl+Q`, qui détache explicitement.
5. Relancer `./build-release/sshos` → `echo $MARQUE` répond `persiste`.

`Ctrl+Q` **détache** et ne détruit rien ; détruire la session pour de bon se demande
par l'entrée « Fermer la session » du menu, qui pose une confirmation.

---

## 2 bis. La publication sur GitHub — faite le 17 août 2026

Le projet est en ligne : **<https://github.com/might-stormlord/sshos>**, dépôt
**public**, sous **GNU AGPL-3.0**. Une seule branche a été poussée, `main`, avec ses
**210 commits** et 197 fichiers suivis. Ce disque n'est plus l'unique copie.

> Le §2 bis d'avant décrivait une publication *arrêtée en cours*, bloquée sur
> l'authentification et visant `gtix2/sshos` en privé. Rien de tout cela n'est resté
> vrai : le compte, la visibilité et la licence ont changé au moment de publier.

### Ce que la publication a changé dans l'historique — le point important

**Toutes les empreintes de commit ont changé, et deux fois plutôt qu'une.** L'identité
d'auteur a été entièrement remplacée : le courriel personnel puis, dans une seconde
passe, le nom civil. Les commits concernés portent désormais tous
`might-stormlord <317721292+might-stormlord@users.noreply.github.com>`, une identité qui
rattache les commits au compte GitHub sans exposer ni courriel ni état civil.

**Première passe — le courriel.** `git filter-branch --env-filter` sur `main` et
`m1-noyau`, 205 commits en 17 s.

- L'adresse était aussi dans **un blob** (`docs/REPRISE.md`, uniquement dans le commit
  de tête). Un seul sur les 835 blobs de l'historique, ouverts un par un pour le
  vérifier. Il a été purgé par un `commit --amend` du sommet.
- **Vérifié après coup :** 205 commits toujours présents, dates d'auteur intactes, et
  `git diff <ancien sommet> main` **ne rend rien** — contenu strictement identique,
  seules les métadonnées avaient bougé.

**Seconde passe — le nom.** Même outil, avec en plus un `--tree-filter` qui balaie le
prénom du contenu : il subsistait dans **25 blobs**, tous des exemples (versions
successives du README, du titre OSC de `tests/test_terminal.cpp`, de l'invite fictive de
la spec de conception, et d'un commentaire de `src/apps/files/files.cpp` qui illustrait
l'élision par un chemin personnel). Le sommet avait été nettoyé à la main juste avant,
donc le `sed` ne l'a pas touché.

- **Vérifié après coup :** les 842 blobs atteignables depuis `main` ouverts un par un,
  **zéro** contient encore le prénom ou l'ancienne adresse ; côté GitHub, les seuls noms
  d'auteur sont `might-stormlord` et `Claude`. L'arbre du sommet publié est **strictement
  identique** à celui sur lequel les 1146 tests étaient passés au vert.
- ⚠️ Cette passe **écrase l'historique publié** : elle s'est terminée par un
  `git push --force-with-lease origin main`.

Les 2 commits d'auteur `Claude <noreply@anthropic.com>` n'ont été touchés par aucune des
deux passes, et les 112 lignes `Co-Authored-By` non plus.

**Sauvegardes, une par passe** — hors du dépôt, et elles ne doivent pas y entrer :
`/root/sshos-backup-avant-reecriture.bundle` (1014 Ko) et
`/root/sshos-backup-avant-retrait-du-nom.bundle` (1,1 Mo), tous les refs à chaque fois.
⚠️ **Elles contiennent l'ancienne identité**, courriel compris.

### Le piège à connaître avant de toucher aux branches

`git branch` en montre 25. **23 d'entre elles portent encore l'ancien historique**, donc
l'ancienne identité **entière — courriel et nom** : les 14 `worktree-agent-*` et 9 autres
(`net-fixes-round2`, `task13-round1-fix`, `feat/client-tty-guard-loop`…). Seules `main`
et `m1-noyau` ont été réécrites, et elles seules.

> **Ne jamais pousser avec `--all` ni `--mirror`.** Ce n'était déjà pas souhaitable
> (aucune de ces branches n'a le moindre commit unique — `git branch --no-merged
> m1-noyau` ne rend rien) ; c'est désormais **une fuite d'identité**, qui annulerait
> d'un coup les deux réécritures. Pousser explicitement : `git push origin main`.

Pour faire le ménage, chacune est verrouillée par un worktree : `git worktree remove
<chemin>` **avant** `git branch -D`. Attention, `.claude/worktrees/agent-aba4275accf38f581`
porte 51 lignes non commitées dans `src/daemon/daemon.cpp` — un état très ancien et
largement dépassé par `m1-noyau`, mais à regarder avant tout `--force`.

### Comment on pousse, maintenant

`gh` est installé (2.46.0) et authentifié en `https` sur le compte `might-stormlord` ;
le *credential helper* est en place, `git push` ne demande plus rien.

```bash
git push origin main
```

**`main` et `m1-noyau` sont deux branches distinctes qu'il faut réaligner à la main.**
Le travail se fait sur `m1-noyau` ; `main` est ce qui part sur GitHub. Après une série
de commits :

```bash
git branch -f main m1-noyau && git push origin main
```

### Ce qui a été vérifié avant que ça parte

- **Aucun secret dans l'historique** : les 835 blobs de la base d'objets ouverts un par
  un ; aucun blob de plus de 0,17 Mo.
- **`docs/sessions/` est exclu par `.gitignore`** et absent du dépôt distant — vérifié
  sur l'arbre poussé. Il contient 930 Ko de transcriptions de conversation brutes, et il
  n'était protégé jusqu'au 15 août que par `.git/info/exclude`, un fichier **local, ni
  poussé ni cloné**, dont le motif ne couvrait même que `_autosnapshot-*`. C'était le
  vrai risque de ce dépôt.
- **`tools/__pycache__/sonde.cpython-314.pyc` avait été purgé de l'historique** le
  15 août ; il gravait `/home/storm/dev/ssh_os_2.0` dans un blob binaire. Ne pas
  relancer cette purge : elle est faite, et le fichier ne peut plus revenir
  (`__pycache__/` et `*.pyc` sont dans `.gitignore`).
- **Côté GitHub, après le push :** dépôt `PUBLIC`, branche par défaut `main`, **une
  seule branche distante**, licence détectée `AGPL-3.0`, et l'unique adresse de courriel
  visible dans les 100 derniers commits est l'adresse de non-réponse.
- **1146 tests au vert** en `Release`, relancés juste avant le commit de publication.

### Ce qui reste ouvert

- **Pas d'en-têtes de licence par fichier.** L'AGPL les recommande ; il y a 124 fichiers
  dans `src/` (108 au jour de la publication). Le `LICENSE` et le README suffisent juridiquement.

---

## 2 ter. L'installation locale, et la mise à jour depuis le bureau

Livrée le 17 août 2026. Conception :
[`docs/superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md`](superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md).
Plan :
[`docs/superpowers/plans/2026-08-17-installation-et-mise-a-jour.md`](superpowers/plans/2026-08-17-installation-et-mise-a-jour.md).

### Le problème que ça règle

Recompiler fermait le bureau : c'était le même binaire et le même socket. Il y a
désormais **deux instances qui s'ignorent** — l'installée, stable, qui sert de poste de
travail, et celle de développement, qu'on casse sans conséquence.

```bash
sh tools/install.sh                    # interactif, quatre questions
sh tools/install.sh --yes --source git # sans rien demander
~/.local/bin/sshos                     # le bureau installé
```

### Ce qu'il faut savoir avant d'y toucher

| | |
|---|---|
| **L'isolation** | `~/.local/bin/sshos` est un **lanceur de quatre lignes**, pas le binaire. Il pose `SSHOS_BOOT_ID` (défaut `bureau01`) et `SSHOS_EXE` (le chemin de relance). Le vrai binaire est dans `~/.local/libexec/`. L'adresse du socket devient `sshos/<uid>/bureau01` ; l'arbre de dev, lui, garde l'uuid du noyau — **c'est la seule chose qui les sépare**. ⚠️ Le fichier d'état, lui, n'est **pas** séparé par instance : deux bureaux partageraient leur état de mise à jour. |
| **Les deux variables ne descendent PAS dans les shells** | Elles sont dans `kBanned` (`src/pty/env.cpp`). Sans ça, `./build-release/sshos --kill` tapé dans un terminal du bureau installé **tuait ce bureau**. Conséquence assumée : `sshos` tapé dans une fenêtre vise l'instance de développement. |
| **Le démon se relance par CHEMIN** | `daemon_exe_path()` (`src/daemon/daemonize.hpp`) préfère `SSHOS_EXE` à `/proc/self/exe`, qui désigne une **inode** : après une mise à jour, celle du client est l'ancienne version. |
| **`sshos --daemon` NE REND PAS LA MAIN** | `become_daemon()` ne forke pas ; il exécute la boucle dans le processus appelant. Le détachement vient de `spawn_detached`, côté client. Pour lancer un démon dans un script, passer par le chemin client (`pty.fork()` + exec du binaire **sans** drapeau), comme `tools/sonde.py`. |
| **L'interface de l'installeur est écrite à la main** | `tools/tui.sh`, du `sh` POSIX et des séquences ANSI : l'installeur tourne **avant** que quoi que ce soit soit compilé, il ne peut pas emprunter le moteur du projet. ⚠️ `${#s}` de dash compte des **octets** : tout texte dessiné doit être en **ASCII pur**, guillemets français compris. |
| **Les versions : majeur déclaré, mineur compté** | Le fichier **`VERSION`** à la racine porte le **majeur** — c'est le seul chiffre que quelqu'un décide, à incrémenter le jour où le bureau change assez pour que ça se dise. Le **mineur** est le nombre de commits depuis que `VERSION` a pris sa valeur, donc il **repart à zéro** à chaque majeur et avance seul le reste du temps. `tools/version.sh` est la seule définition ; l'installeur la pose en `libexec/sshos-version` et `update.sh` l'appelle. Le C++ ne calcule rien : il lit `installed_version` et `remote_version` dans l'état, et **refuse tout ce qui n'est pas chiffres et points** — la valeur vient d'un script et finit dans une modale. |
| **Le C++ n'écrit jamais l'état** | `~/.local/share/sshos/state`, clé=valeur, écrit par les scripts avec un `rename()` atomique et lu par `src/shell/update_state.cpp`. C'est ce qui garde `git`, `cmake` et le réseau hors du fil unique du démon. |
| **Le lanceur d'enfant est un `fork()` SIMPLE** | Jamais `spawn_detached` : son double fork rend le pid de l'intermédiaire, et le vrai travail, réparenté à init, ne serait **jamais** récoltable. Voir `launch_updater` dans `src/daemon/session.cpp`. |

### Les outils

| Commande | Ce qu'elle fait |
|---|---|
| `sh tools/install.sh` | installe. **Assistant en mode texte** sur un terminal (flèches, clic, `←` pour revenir) ; questions simples sinon — c'est ce que la CI et les sondes empruntent. Échelle git → binaire publié → archive → arbre local |
| `python3 tools/verif_installeur.py` | pilote l'assistant par un PTY et **mesure la largeur de chaque ligne du cadre** |
| `python3 tools/verif_bureau_ouvert.py` | réinstaller pendant qu'un bureau tourne : prévenu, arrêté, installé — et intact si l'on refuse |
| `sh tools/version.sh . HEAD` | le numéro de version d'un commit : **`MAJEUR.MINEUR`** |
| `sh tools/update.sh --check\|--apply\|--rollback` | posé à l'installation sous `~/.local/libexec/sshos-update` |
| `python3 tools/verif_isolation.py <HOME>` | **le test qui juge tout** : le binaire de dev ne doit ni voir ni tuer le bureau installé |
| `python3 tools/sonde_update.py` | la sonde bout-en-bout, sur un faux dépôt git — 26 vérifications |
| `python3 tools/verif_redemarrage.py` | un vrai redémarrage : l'ancien démon sort, un neuf prend sa place, le client se rattache seul |
| `python3 tools/verif_redemarrage_lent.py` | le même, mais le démon neuf met **3 s** à écouter. C'est le cas qui a coûté un bureau le 19 août : `RETARD=3`, et `SSHOS_ANCIEN=` pour rejouer contre une version antérieure |
| `python3 tools/verif_progression.py` | la barre de progression **avance** pendant un vrai `--apply` : le fichier d'état est échantillonné pendant le travail, et la suite vérifie qu'elle bouge, ne recule pas, et ne déborde pas de [0, 100] |
| `python3 tools/verif_repos.py` | le démon **détaché** ne doit consommer aucun CPU |
| `python3 tools/verif_sortie.py` | « Fermer la session » tue toujours le démon — chemin qu'aucun test unitaire ne couvre |
| `python3 tools/balayage.py --strict` | les fonctions déclarées sans appelant de production (§9 bis) |

`SSHOS_PREFIX`, `SSHOS_STATE_DIR` et `SSHOS_REPO_URL` surchargent les chemins et l'URL :
c'est ce qui permet à la sonde de ne pas écraser l'installation réelle. **`SSHOS_BOOT_ID`
n'isole QUE le nom du socket**, jamais les chemins — l'erreur a été commise une fois.

> 🔴 **AVANT DE LANCER UNE SONDE, savoir laquelle peut tuer votre bureau.** Le
> bureau installé porte `--daemon` dans sa ligne de commande et votre uid : toute
> sonde qui énumère les démons par ce motif le trouve, le tue, et emporte avec lui
> **la session de travail qui tourne dedans**. Quatre d'entre elles le faisaient
> jusqu'au 20 août au soir, dont `tools/sonde.py` elle-même (§10).
>
> Depuis, la marque est `SSHOS_BOOT_ID`, posée par `spawn()` et relue dans
> `/proc/PID/environ`, et `verif_isolation.py` **refuse** de tourner si l'instance
> qu'il vise est déjà vivante. Le geste de vérification, à faire pour toute sonde
> neuve ou modifiée :
>
> ```bash
> ~/.local/libexec/sshos --status          # relever le pid du bureau
> python3 -u tools/<la sonde>.py
> ~/.local/libexec/sshos --status          # MEME pid, sinon la sonde tue
> ```

### La CI

`.github/workflows/release.yml` publie `sshos`, `sshos_tests`, `golden.tar.gz` et
`SHA256SUMS` à chaque poussée sur `main`, **seulement si la suite est verte**. Conteneur
épinglé `ubuntu:26.04` ; seule la compilation y tourne, `checkout` et `gh` restent sur le
runner.

> **Deux pièges de portabilité trouvés en la mettant en place**, et corrigés à la source :
> une image nue n'a pas de `tzdata`, donc `localtime_r` retombait sur UTC ; et les
> scénarios golden ne figeaient pas leur fuseau alors que leurs références contiennent
> une heure rendue — elles n'étaient vraies que sur la machine qui les avait
> enregistrées. Neuf échecs, zéro depuis.

### Le modèle de menace, dit une fois

La racine de confiance est **le compte GitHub**. `SHA256SUMS` vient de la même release que
le binaire : il prouve l'intégrité **du transport**, pas l'authenticité. Et « on n'installe
qu'après le vert » **exécute déjà la charge utile**. La propriété réelle est donc : *on
n'installe pas du code cassé*. Pas de signature détachée — c'est une décision, notée comme
telle dans la spec §5.0.

---

## 2 quater. La mise à jour du 20 août 2026 — faite, et la règle qu'elle laisse

> Cette section décrivait au futur une manœuvre qui est désormais **accomplie**.
> Elle est conservée pour la règle permanente qu'elle a servi à établir, pour la
> table de lecture du journal, et pour le chemin de réinstallation locale — trois
> choses qui ne se trouvent nulle part ailleurs dans ce dossier.

**Ce qui s'est passé.** `main` a été poussée, la mise à jour appliquée, et le
redémarrage redouté est passé sans incident : le journal porte
`arret pour terminer une mise a jour` puis `demarrage pid=1603147` à la **même
seconde** (2026-08-20 04:22:57). L'installation tourne depuis sur le commit
qu'elle annonce — `installed_commit` = `remote_commit`, `status=up-to-date`.

### La règle permanente : une mise à jour ne bénéficie jamais du correctif qu'elle installe

Ce n'est pas une circonstance, c'est mécanique, et ça se reproduira à chaque fois
qu'un correctif touchera le parcours de mise à jour lui-même :

| Ce qui est corrigé | Pourquoi la mise à jour qui l'installe n'en profite pas |
|---|---|
| Tout ce que fait **le client** (le budget de redémarrage, par exemple) | Le redémarrage est piloté par le client **déjà lancé**, donc l'ancien binaire. Le neuf est posé, mais il ne tourne pas encore. |
| Tout ce que fait **le script** (la barre de progression, par exemple) | `--apply` remplace `sshos-update` à l'étape « installation », c'est-à-dire **après** avoir compilé et passé la suite. Le script qui travaille est donc l'ancien. |

**Le corollaire à retenir :** pour éprouver un correctif du parcours de mise à
jour, il faut la mise à jour **suivante**, ou le chemin d'installation locale
ci-dessous. Un essai fait sur celle qui l'installe ne prouve rien.

### Le raccourci : réinstaller depuis l'arbre local

Le chemin d'installation pose le binaire **et** les scripts d'un coup — il n'a pas
à s'auto-remplacer en cours de route.

```bash
cd /home/storm/dev/ssh_os_2.0
cmake --build build-release -j"$(nproc)" && ./build-release/sshos_tests
sh tools/install.sh --yes --source local --local-tree .
```

> 🔴 **L'installeur arrête le bureau en cours** — après l'avoir dit, et avec
> « non » par défaut. **Toute session de travail qui tourne DANS le bureau meurt
> avec lui**, y compris une session d'assistant. À faire depuis une console qui ne
> vit pas dans `sshos`, ou en acceptant de tout rouvrir.
>
> ⚠️ `--source local` **n'installe pas ce que GitHub contient**, mais ce que
> l'arbre contient. Pousser reste nécessaire pour que la vérification suivante ne
> redescende pas sur une version antérieure.

### Avant de cliquer « Mettre a jour » : pousser

`--apply` tire de **GitHub**, pas du disque. Le vérifier plutôt que le supposer :

```bash
git rev-list --count origin/main..m1-noyau   # 0 = rien à pousser
git branch -f main m1-noyau && git push origin main
```

⚠️ `git push origin main`, **jamais `--all` ni `--mirror`** : 23 branches locales
portent encore l'ancienne identité (§2 bis).

### Si un redémarrage se perd quand même

**Les deux messages ne disent PAS la même chose. Les lire séparément.**

```
sshos: mise a jour installee, redemarrage...
sshos: le demon n'a pas repondu
```

Le client a bel et bien relancé un démon, mais celui-ci n'a pas pris l'adresse
dans les 30 s du budget (`LaunchBudget`, `src/client/launch.hpp`). **Le bureau
n'est pas perdu** : le démon finit par se lever seul, **retaper `sshos`** s'y
rattache.

```
sshos: mise a jour installee, redemarrage...
sshos: le redemarrage n'a pas abouti
```

Celui-là est **tout autre** : le client a rendu la main **sans même essayer** de
relancer quoi que ce soit. Voir §2 sexies — c'était, jusqu'au 21 août 2026, le
défaut du compteur, et c'est la seule raison pour laquelle ce message pouvait
apparaître dans un parcours normal. Retaper `sshos` remonte un bureau neuf, mais
la session d'avant est perdue.

Vérifier au passage :

```bash
tail -5 ~/.local/share/sshos/journal.log
grep -E 'installed_commit|installed_version|status|restart_pending' \
     ~/.local/share/sshos/state
```

**Lire ces deux dernières lignes ensemble, et savoir laquelle dit quoi :**

| Clé | Ce qu'elle porte |
|---|---|
| `status` | la **conclusion** de la dernière vérification — `up-to-date`, `available`, `check-failed`… |
| `restart_pending` | un **fait** : un binaire est posé et ce n'est pas celui qui tourne. `1` ou vide |

Les deux ont longtemps vécu dans la seule clé `status`, et c'est ce qui a
coûté : une conclusion pouvait effacer un fait qu'elle n'avait même pas
observé. ⚠️ **Tant que le fait tient, `status` le redit aussi** (`restart-pending`
dans les deux) — c'est voulu, pour les démons antérieurs à la clé, qui ne lisent
que `status`. Voir §2 quinquies.

### Ce que le journal sait dire

| Ligne | Ce qu'elle veut dire |
|---|---|
| `demarrage pid=N socket=sshos/<uid>/<boot>` | un démon a pris l'adresse et vit |
| `arret pour terminer une mise a jour` | il s'est retiré pour laisser la place au binaire neuf |
| `demarrage refuse : adresse deja prise, socket=…` | un démon a été lancé alors qu'un autre tenait déjà l'adresse. **Normal** après une double relance ; anormal juste après un « arret » |
| `demarrage impossible : <raison>` | le `bind` a échoué pour autre chose. Rend 1 |
| *(une ligne `demarrage` suivie de RIEN)* | **SIGKILL** — le tueur de mémoire, typiquement. C'est la signature d'une mort brutale |

**Un « arret pour terminer une mise a jour » suivi d'un trou de plusieurs
secondes a DEUX lectures, et longtemps une seule était écrite ici.**

| Lecture | Ce qui s'est passé pendant le trou | Ce que l'utilisateur a vu |
|---|---|---|
| Le démon traîne | le client attend, puis abandonne | `le demon n'a pas repondu` |
| **Le client est mort sur place** | **personne n'attend : le trou, c'est l'utilisateur qui retape `sshos`** | `le redemarrage n'a pas abouti` |

La première seule était documentée — c'est le défaut du 19 août, budget passé de
1 s à 30 s, gardé par `tools/verif_redemarrage_lent.py`. La seconde est le défaut
du compteur (§2 sexies), et **le trou du 21 août 2026 à 11:24 était celui-là** :

```
2026-08-21 11:24:37 arret pour terminer une mise a jour
                    (rien — ni « demarrage », ni « demarrage refuse »,
                     ni « demarrage impossible »)
2026-08-21 11:24:47 demarrage pid=456995 socket=sshos/0/bureau01
```

Relevé dans `/proc` au même moment : le client `456993` avait démarré à **11:24:47**
lui aussi, avec pour parent un **shell** — ce n'est pas celui qui avait cliqué à
11:24:37, c'est un `sshos` retapé à la main. Le premier est mort sans rien lancer.

**Le discriminant sûr est le MESSAGE**, pas la durée du trou : un trou de
plusieurs secondes ne dit pas à lui seul laquelle des deux lectures s'applique.
Le journal ne porte aucune ligne dans les deux cas.

---

## 2 quinquies. Le fait et la conclusion — comment se lit l'état de mise à jour

Le fichier `<données>/state` porte deux choses de nature différente, et les avoir
confondues a produit deux défauts successifs. La distinction vaut d'être tenue.

**`status` est une CONCLUSION.** C'est ce qu'a trouvé la dernière vérification :
`up-to-date`, `available`, `check-failed`, `history-rewritten`… Elle se refait à
chaque `--check`.

**`restart_pending` est un FAIT.** « Un binaire est posé et ce n'est pas celui qui
tourne. » Une vérification ne regarde que le dépôt distant — elle n'observe pas ce
fait, **donc elle ne peut pas le démentir**.

| Qui | Peut armer le fait | Peut l'effacer |
|---|---|---|
| `--apply` | ✅ il pose un binaire | — |
| `--rollback` | ✅ il pose un binaire (inode neuve) | — |
| `--check` | ❌ jamais | ✅ **seulement** s'il constate le redémarrage |
| le démon | ❌ le C++ n'écrit jamais ce fichier | il **cesse d'y croire**, sans l'effacer |

### Comment `--check` peut constater un redémarrage

Le démon lance le script par un `fork()` **simple** suivi d'un `execv`
(`launch_updater`, `src/daemon/session.cpp`) : **notre parent EST le démon**.
Comparer l'inode de son binaire à celle du binaire posé dit donc si c'est bien lui
qui tourne.

```sh
stat -Lc '%d:%i' /proc/$PPID/exe     # -L : sans lui on mesure le lien magique,
stat -c  '%d:%i' "$EXE"              #      qui vit sur procfs (§9)
```

Tapé à la main depuis un shell, le parent n'est pas un démon : **on ne conclut
pas, et le fait est conservé.** C'est le sens sûr de l'incertitude — garder un
redémarrage en attente ne coûte qu'une proposition de trop ; le perdre coûte un
bureau qui tourne sur l'ancien binaire sans le dire.

### Les deux défauts que cette séparation a soldés

1. **Le cul-de-sac** (20 août). `--check` écrasait `restart-pending` par
   `up-to-date` : la pastille s'éteignait, l'entrée redevenait « Verifier les mises
   a jour », et plus rien ne proposait le redémarrage. La vérification automatique
   tombe une fois par jour, sans que personne ait rien demandé : le trou se
   refermait tout seul, en silence.
2. **Le fichier qui ment** (21 août). Le premier correctif préservait
   `restart-pending` dès que c'était l'état d'avant — ce qui fermait le cul-de-sac,
   mais le fichier ne se redressait alors **plus jamais**. Or le §2 quater dit
   littéralement de lire ce champ pour diagnostiquer. On avait échangé un défaut
   fonctionnel contre un fichier trompeur.

### Deux détails qui surprennent, et qui sont voulus

- **`status` redit `restart-pending` tant que le fait tient.** Un démon *antérieur*
  à la clé ne lit que `status` ; lui écrire « à jour » pendant qu'un binaire posé
  attend son redémarrage lui ferait éteindre la pastille pour toujours —
  exactement le défaut n° 1. La redondance est le prix de la compatibilité.
- **Un fichier sans la clé mais avec `status=restart-pending` est migré** vers
  `restart_pending=1`, côté script **et** côté analyseur. Le perdre coûterait un
  bureau au moment précis de la mise à jour qui introduit la clé. Mais la clé
  **présente** fait autorité, même contre le statut : sans quoi la migration
  deviendrait un « toujours vrai » et le fait ne pourrait plus jamais retomber.

### Côté C++

`restart_needed_` est la **seule** source du « faut-il redémarrer ? » — l'entrée de
menu, la pastille, `needs_restart()` et la commande de redémarrage la lisent toutes.
Elle vaut `state_.restart_pending && !running_is_installed()`.

**Le redémarrage passe avant une nouveauté**, et c'est un choix : il coûte trois
secondes contre deux minutes de compilation, l'utilisateur l'a déjà payé, et il
maintient l'invariant « au plus un binaire posé jamais exécuté ». La nouveauté sera
encore là au tour suivant.

---

## 2 sexies. Le redémarrage qui échouait une fois sur deux — soldé le 21 août 2026

**Le symptôme, tel qu'il était rapporté.** « Quand on clique sur Redémarrer après
une mise à jour, une fois sur deux on a `sshos: le redemarrage n'a pas abouti` » —
et le bureau était perdu, retour au shell.

**Ce que le message exigeait, mécaniquement.** `src/main.cpp` n'imprime cette
ligne qu'en sortie de sa boucle d'attache ; `run_client()` ne rend
`kClientRestartRequested` (`src/client/client.cpp`) que sur un `Detached` dont la
raison est `kDetachReasonUpdate` ; et le démon ne l'envoie
(`src/daemon/daemon.cpp`) que sur `session.wants_update_restart()`. Il fallait
donc **deux détachements pour mise à jour de suite** — ce qui a d'abord fait
chercher un deuxième démon anormal, une course, un binaire mal reconnu. Le
journal, lui, ne montrait que des redémarrages propres : aucune de ces pistes ne
tenait.

**La cause.** La boucle s'écrivait :

```cpp
for (int attempt = 0; attempt < 2; ++attempt) {   // src/main.cpp, avant le correctif
```

Elle croyait borner un emballement. Elle bornait en réalité **les redémarrages de
toute la vie du processus client**. Le premier passait ; le second était refusé
**sans même tenter de relancer un démon**. Comme un `sshos` retapé repartait avec
un compteur neuf, l'utilisateur voyait exactement « une fois sur deux ». Et les
trois lignes `mise a jour installee, redemarrage...` de son rapport étaient la
somme d'un redémarrage réussi resté dans le défilement et des deux de la séance
qui a échoué.

**Pourquoi le compte était faux dans son principe.** Un redémarrage pour mise à
jour ne s'arme QUE sur une confirmation explicite (`answer_modal(true)` avec
`ModalKind::RestartForUpdate` → `UpdateService::run("update:restart")`). Aucun
chemin ne l'arme tout seul : **chaque tour coûte un geste de l'utilisateur**, et
rationner ces gestes n'a aucun sens. Ce qu'il fallait borner, c'est
l'aller-retour **stérile** — un démon qui se détache sans avoir jamais servi de
bureau.

**Le correctif.** `src/client/restart.hpp` — `RestartBudget` — ne compte que les
allers-retours stériles consécutifs, et repart de zéro dès qu'une session a servi.
« Avoir servi » se lit sur deux témoins que `run_client()` remplit désormais
(`SessionTrace`, `src/client/client.hpp`) : **une trame reçue** (le bureau s'est
affiché) et **une entrée envoyée** (l'utilisateur a agi).

> ⚠️ **Encore la même leçon que le budget d'attente du 19 août** : le compte
> vivait dans `src/main.cpp`, que `CMakeLists.txt` retire de `sshos_core`. Aucun
> test ne pouvait l'atteindre. **C'est le deuxième défaut de redémarrage de suite
> à s'être caché exactement là.** Tout ce qui décide quelque chose doit sortir de
> `main.cpp` — voir `src/client/launch.hpp` pour le précédent.

**Pris sur le fait, sur l'installation vivante.** Le 21 août 2026 à 11:24, le
journal porte `arret pour terminer une mise a jour` à 11:24:37 puis `demarrage
pid=456995` à 11:24:47, **sans aucune ligne entre les deux**. Or `/proc` montre
que le client `456993` a lui aussi démarré à 11:24:47, avec un **shell** pour
parent, et que c'est LUI qui a engendré `456995`. Le client qui avait cliqué à
11:24:37 n'a donc jamais atteint son tour 1 : il est sorti sur `main.cpp:146`
sans tenter quoi que ce soit, et les dix secondes sont le temps qu'il a fallu à
l'utilisateur pour retaper `sshos`. C'est exactement ce que ce défaut prédit, et
rien d'autre ne le produit.

**Ce qui le garde.** `tests/test_restart.cpp` couvre `RestartBudget` **et** le
remplissage de `SessionTrace` par un vrai `run_client()` face à un faux démon —
sans ce second cas, un témoin né mort réintroduirait le défaut en silence (§9
bis). Et `tools/verif_redemarrage.py` enchaîne désormais **deux** redémarrages :
un seul ne voyait rien. Éprouvé contre un binaire reconstruit sur l'ancien
`main.cpp` — il sort bien non nul.

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
| **7** | Le gestionnaire de fichiers, façon Dolphin | Vue scindée, sélection multiple, copier/coller sans bloquer, colonnes triables, historique, raccourcis | ✅ **livré** |

### Après la v1 — ce que l'utilisateur a demandé depuis, et qui est livré

Ces travaux ne figurent dans aucun plan : ils sont venus un par un, en réaction à
l'usage réel. Ils sont tous dans l'historique de `m1-noyau`.

> ⚠️ **Ne pas confondre avec le plan d'installation.** Ses **quatorze tâches** sont les
> commits `1c64f19` → `387bd24` du 17 août, et elles se lisent au §2 ter. Tout ce qui
> les suit — l'assistant, les versions, les défauts du parcours de mise à jour — est
> venu de l'usage, après la clôture du plan, et se lit donc ici.

| Demande | Ce qui a été fait | Commit |
|---|---|---|
| Retirer `Bloc` et `Battement` | Ils deviennent des doublures de test (`tests/fake_apps.hpp`) ; le catalogue ne les propose plus. La session amorce désormais par une **fabrique** injectable (`set_seed_factory_for_tests`) | `f119b26` |
| Repositionnement intelligent | Entrée de menu « ranger les fenêtres » : grille en colonnes, reste aux premières (`src/wm/tile.cpp`) | `8b7cebd` |
| Deux sorties qu'on ne confond plus | `Ctrl+Q` détache (la session survit) ; « Fermer la session » détruit, **avec confirmation** | `8b7cebd` |
| Le moniteur devient un widget | L'application disparaît ; le fond d'écran porte **cinq** sections encadrées (CPU, mémoire, réseau, charge, processus — `counter_box()` en dessine deux, puis trois appels directs à `frame()` aux l. 261, 265 et 282 de `src/shell/sysinfo.cpp` ; `grep -c 'frame('` en rend 5 parce qu'il compte aussi la définition l. 57), jauges vertes/jaunes/rouges, signature « SSH OS » centrée | `444bac6`, `3919486`, `1600142` |
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
| **Les sondes rangées** | Quatre vérifications bout-en-bout passent dans `tools/` : l'isolation dev/installé, le redémarrage complet, le démon détaché au repos, la sortie par « Fermer la session ». Chacune couvre un chemin que la suite unitaire ne peut **pas** atteindre — il y faut un vrai démon, un vrai PTY, une vraie mesure. La dernière mérite d'exister : `wants_quit()` n'était testé qu'au niveau de la `Session`, jamais à travers la boucle du démon — exactement le défaut du §9 bis. `SSHOS_DEV_BIN` vise un autre binaire que celui de l'arbre | `e673c24` |
| **`sshos` se tape** | L'installeur se contentait d'**afficher** la ligne de `PATH`, par prudence (le §4 interdit de toucher à la configuration sans accord). L'accord donné, il la pose — dans **`~/.profile`, pas `~/.bashrc`**, et les deux raisons ont été vérifiées sur un HOME jetable : un shell de **connexion** ne lit `.bashrc` que si `.profile` le source (vrai sur Ubuntu, pas une règle — le premier essai du correctif l'a fait, et le test l'a montré), et `.bashrc` commence presque toujours par `[ -z $PS1 ] && return`, donc `ssh machine sshos` ne l'aurait jamais vue. Idempotent, avec un marqueur qui dit d'où vient la ligne | `7b25b48` |
| **Un assistant d'installation, souris comprise** | Un installeur en `read` nu pour un projet dont la règle est « la souris d'abord » était incohérent. Un seul cadre du début à la fin, les réponses déjà données restant visibles, barre de progression, `Gauche` pour revenir — flèches, `Entrée`, et **clic**. Écrit à la main dans `tools/tui.sh` : l'installeur tourne **avant** que quoi que ce soit soit compilé, il ne peut pas emprunter le moteur de rendu du projet. Repli ASCII complet, et `--yes` ou l'absence de terminal ramènent aux questions simples — la CI et les sondes ne doivent jamais rester bloquées devant un assistant. **Défaut trouvé par son propre test :** `${#chaîne}` de `dash` compte des **octets**, donc une description contenant des guillemets français décalait le cadre de deux colonnes ; `verif_installeur.py` pilote l'assistant par un PTY et mesure la largeur de chaque ligne. Trois réglages, pas quatre : la question sur `loginctl enable-linger` est retirée — le démon avertit déjà lui-même, **et seulement quand le cas se présente**. Installer arrête le bureau en cours après l'avoir dit (**« non » par défaut** — la réponse sûre à une question destructrice ne doit jamais être celle qu'on donne par inadvertance), sans quoi le démon continuait sur l'**ancien** binaire en écrivant « à jour ». Et le nom par défaut devient `bureau01`, l'adresse du socket s'affichant **en direct** pendant la frappe | `4389862`, `02aedee`, `98c5bdf`, `cce9d11` |
| **Une vérification demandée répond** | Le mécanisme interrogeait GitHub, comparait, concluait — et ne **disait rien**. Un pop-up, pas une ligne de menu : le menu s'est refermé au moment du clic, et personne ne va le rouvrir pour voir si un libellé a changé. `Modal` gagne un mode information (un seul `[ OK ]`, parce qu'il n'y a rien à décider). **La règle du silence est verrouillée par un test :** une vérification **automatique** ne dit rien — elle tombe une fois par jour sans qu'on ait rien demandé, et un pop-up quotidien serait du harcèlement ; elle allume la pastille, c'est tout. Au passage, un défaut préalable de `Modal` : `rect()` bornait le cadre, mais `draw()` reçoit une `View` pleine largeur — une question longue débordait par-dessus le bureau | `3512ffe` |
| **Des versions qui se lisent** | « `cce9d11 -> 3512ffe` » ne dit rien à personne. Le fichier **`VERSION`** porte le **majeur** — le seul chiffre que quelqu'un décide — et le **mineur** compte les commits depuis que ce fichier a pris sa valeur : il repart à zéro à chaque majeur et avance seul le reste du temps, sans rien à maintenir et sans pouvoir reculer. `tools/version.sh` est la **seule** définition. Le C++ ne calcule rien, il lit, et **refuse tout ce qui n'est pas chiffres et points** — la valeur vient d'un script et finit dessinée dans une modale. Deux manques constatés ensuite sur l'installation réelle : `--apply` remplaçait `sshos-update` mais **pas** `sshos-version`, donc `installed_version` restait vide après chaque mise à jour ; et les numéros n'étaient calculés que dans la branche « une mise à jour existe », alors que « vous êtes à jour, en version 1.2 » vaut mieux que « vous êtes à jour » tout court | `d6e6712`, `80ded99` |
| **Le parcours de mise à jour, trois culs-de-sac** | (1) **Rien n'effaçait `restart-pending`** : le script l'écrit et ne peut pas savoir quand le redémarrage a eu lieu ; après un redémarrage réussi la pastille restait allumée et cliquer redemandait de redémarrer, **indéfiniment**. Le démon ne connaît pas son propre commit — `CMakeLists` est intouchable — mais il compare l'**inode** de `/proc/self/exe` à celle du binaire posé, et cesse simplement de croire un état que la réalité a dépassé (il ne réécrit pas le fichier : le C++ ne l'écrit jamais). (2) **La fenêtre se refermait pendant le travail** — une compilation et une suite complète prennent une à deux minutes : `Modal` gagne trois styles (question, information, progression) et raconte son étape, avec un rafraîchissement branché sur les **deux** réveils, l'échéance du service disparaissant justement pendant qu'un enfant travaille. (3) **Un binaire posé défaillant piégeait le bureau** : l'entrée unique ne proposait plus que « Redémarrer pour terminer », qui relançait le même binaire — aucune sortie sans taper une commande à la main. La vérification reste joignable en seconde ligne, et seulement dans cet état. Enfin le pop-up de vérification devient une **question** (« Plus tard » / « Mettre à jour ») au lieu d'un `[ OK ]` qui laissait croire l'affaire close, « Plus tard » gardant le focus pour qu'un `Entrée` réflexe ne lance pas deux minutes de compilation | `0ce5869`, `060248f`, `8e1edc9` |
| **La molette** | `Session::on_mouse` posait un invariant pour le glisser-déposer — « au-delà, tout est un appui » — et rejetait tout le reste **avant** la distribution aux fenêtres. Le terminal savait pourtant déjà faire défiler son historique, et Fichiers sa liste : deux fonctions complètes **sans en recevoir une seule**. La molette va désormais à la fenêtre **sous le pointeur**, sans lui donner le focus et sans prendre la souris. Même famille que le §9 bis, vue depuis le chemin d'ENTRÉE : les tests unitaires appelaient `Terminal::on_mouse` en direct, sans passer par le bureau | `2b4dfc0` |
| **Le démon survit au tueur de mémoire, et se dit** | Une session de travail a disparu sans laisser de trace : ni `dmesg` dans le conteneur, ni image mémoire, ni journal. `run_daemon()` se pose donc à `oom_score_adj = -1000` (mesuré avant : 6,4 Mo pour un `oom_score` de 666, contre 704 pour un processus de 543 Mo). **Le réglage s'hérite et survit à `execve`** : les trois endroits qui donnent naissance à un processus étranger le rendent (`Pty`, le lanceur de mise à jour, `spawn_detached`), sans quoi un `make -j12` lancé dans une fenêtre deviendrait immortel. Et un journal — `<données>/journal.log` — dit `demarrage pid=…` puis la raison de l'arrêt. **Ce qu'il ne peut pas dire est ce qui le rend utile :** un SIGKILL n'écrit rien, donc une vie qui commence et ne se termine par aucune ligne EST la signature d'une mort brutale | `2f4232d` |
| **Coller, et pouvoir copier** | Coller ne faisait **rien**, et n'avait jamais rien fait : `Session::on_input` démontait `InputEvent` par une chaîne de `get_if` et oubliait le collage (§9 bis, n° 16). Le terminal l'écrit maintenant à l'invité, encadré si et seulement si l'invité a demandé le mode 2004, l'encadrement s'ouvrant au premier morceau et ne se fermant qu'au dernier. **Tout octet de contrôle est retiré** sauf tabulation et fins de ligne : un `\033]0;` collé renommerait la fenêtre, un `\033[201~` fabriqué refermerait l'encadrement et rendrait exécutable tout ce qui suit. Pendant une saisie du bureau, le collage va dans le **champ** — coller un chemin dans la roue plutôt que le retaper. **Le garde permanent est au §9 bis** | `6828642` |
| **Copier : la question reste ouverte** | Le bureau capte la souris (`?1002h`), ce qui neutralise la sélection native du terminal — on ne peut pas sélectionner pour copier ailleurs. La bascule `^A m` (« Souris : bureau ou terminal ») rend la souris au terminal, et elle avait été ajoutée au menu ; **l'utilisateur l'a refusée** : *« j'ai toujours une souris donc… »* — il ne veut pas troquer la souris contre la sélection, il veut les deux. L'entrée de menu a été retirée, **puis la bascule elle-même** : sous Konsole, `Maj`+glisser rend la sélection native **sans** couper le tracking, donc le troc qu'elle propose n'a aucune contrepartie ici — et il enferme (« je suis coincé dans le mode pas de souris »). Sont partis avec elle : `Action::ToggleMouse`, `^A m`, sa ligne d'aide, et **tout le mécanisme hors-bande** (`take_out_of_band`, `out_of_band_`, la concaténation devant la trame) dont elle était le SEUL producteur — le garder aurait fabriqué le code sans appelant du §9 bis. La spec §7.2, qui la prescrivait depuis le 10 août, porte désormais une annotation datée plutôt qu'une réécriture. Une conception complète a été explorée (sélection possédée par le bureau, presse-papiers interne, OSC 52, fenêtre « texte nu ») et **délibérément NON construite** : l'utilisateur est sous **Konsole**, dont le `Maj`+glisser rend la sélection native sans coûter la souris du bureau, et il a confirmé que « le copier collé fonctionne ». Onze fichiers pour une question qui ne se pose plus. ⚠️ **Si elle revient**, le point qui décide est que `Maj`+glisser sélectionne la trame COMPOSÉE — bordures, panneau, fenêtres voisines — parce que nos trames positionnent le curseur au lieu d'émettre des lignes ; le bureau doit alors posséder la sélection lui-même, et le créneau existe déjà, tout fait, là où `Terminal::on_mouse` jette les évènements quand l'invité ne suit pas la souris | `666de42` |
| **Un demon lent ne coute plus le bureau** | Redemarrer apres une mise a jour rendait la main au shell : le client accordait au demon neuf **50 tentatives a 20 ms, soit une seconde pile**, alors que le journal montre un retour a **13 secondes**. Deux choses rendaient le defaut introuvable. Le geste vivait dans `src/main.cpp`, que `CMakeLists` retire de `sshos_core` — **aucun test ne pouvait l'atteindre** ; il vit desormais dans `src/client/launch.cpp`, avec une couture pour le lanceur, un budget de **30 s** et un mot a l'utilisateur passe une seconde (« le demon met du temps a demarrer, patience... »). Et le journal s'ouvrait **apres** le `bind`, si bien qu'un demon qui n'arrivait pas a demarrer sortait sans rien ecrire nulle part — `spawn_detached` redirige deja ses 0/1/2 vers `/dev/null`. Il s'ouvre avant, et une adresse deja prise laisse une ligne. ⚠️ **Le correctif ne sauve pas LA mise a jour qui l'installe** : c'est le client d'AVANT qui redemarre, avec son budget d'une seconde. Sonde : `tools/verif_redemarrage_lent.py` | `5b792e6`, `843919a` |
| **La mise a jour dit ou elle en est** | Cinq libelles couvraient une a deux minutes : « compilation... » restait fige, et rien ne distinguait un travail qui avance d'un travail bloque. **Aucun chiffre n'est invente** : `cmake --build` ecrit deja « [ 57%] » et le lanceur de tests une ligne par cas, le total se comptant sur l'arbre sorti. Un surveillant de fond replie les deux sur l'echelle du travail entier et ecrit `progress=` dans l'etat ; le C++ **lit** et refuse tout ce qui n'est pas un entier de 0 a 100, comme pour les numeros de version. La barre prend **la place des boutons**, qu'une progression n'a pas : la boite ne change donc pas de hauteur, et le chiffre est calibre sur « 100% » pour qu'elle ne se decale pas de 9 a 10 pour cent. Sans chiffre, **aucune barre**. La jauge de `shell/sysinfo.cpp` devient `render/gauge.cpp`, partagee. Sonde : `tools/verif_progression.py` | `abd6ae4`, `40c8a1f` |
| **Le terminal s'ouvre chez vous** | Il s'ouvrait dans `/` : `become_daemon()` fait `chdir("/")` et le répertoire courant s'hérite. `PtySpawn` gagne un `cwd` (échec ignoré — un dossier effacé ne doit pas coûter la fenêtre), le défaut est `home_dir()` lu dans `getpwuid()`, et une case **`[*]`** dans la barre d'onglets, juste avant le `+`, ouvre une saisie en place qui prend toute la barre. ASCII assumé : `U+2699` est un emoji, rendu sur une ou deux colonnes selon le terminal, et la barre se décalerait. Persisté dans `<données>/config` (`cle = valeur`, écrit par temporaire + `rename()`) | `d40a1ba` |

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

**Volume réel au 15 août 2026 : 41 535 lignes sur 162 fichiers** — `src/` en compte
16 736 sur 108, `tests/` 24 799 sur 54. L'estimation d'origine (12 000 à 15 000) est
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

19 583 lignes dans `src/` sur 124 fichiers, 28 963 dans `tests/` sur 78 — dont 57
fichiers `test_*.cpp` (mesuré le 20 août 2026 au soir ; ces nombres périment, voir
l'en-tête pour les recompter). **Le rapport n'est pas une coquille** : le projet écrit plus de tests que de code, et c'est ce qui rend les
campagnes de mutation possibles.

| Module | Ce qu'il fait | À savoir avant d'y toucher |
|---|---|---|
| `src/common/` | Descripteurs, sockets UNIX abstraits, file de sortie, protocole, UTF-8, horloge de trame, **protection contre le tueur de mémoire** (`oom.hpp`), **chemin des données de l'utilisateur** (`paths.hpp`) | `OutQueue` a un plafond ; son dépassement se classe *Clean* ou *Dirty* et la réaction diffère (A7). `oom.hpp` : le réglage s'HÉRITE et survit à `execve` — tout endroit qui donne naissance à un processus étranger doit appeler `drop_oom_protection()` |
| `src/render/` | `Surface` (grille de cellules), `View` (sous-rectangle clippé), `Differ` (trames ANSI), thème, largeurs Unicode, **jauge** (`gauge.hpp`) | Une application ne reçoit **jamais** autre chose qu'une `View` — elle ne peut pas peindre hors de sa fenêtre. `gauge_bar()` est la SEULE barre du projet : le fond d'écran et la fenêtre de mise à jour s'en servent toutes deux, et le glyphe suit la `Border` — c'est elle qui porte la réponse à « ce client accepte-t-il l'UTF-8 ? » |
| `src/client/` | Mode brut et filet de crash (`tty_guard`), boucle client (`client`), **lancement du démon et attente qu'il écoute** (`launch`) | `launch.cpp` existe parce que ce geste vivait dans `main.cpp`, **que `CMakeLists` retire de `sshos_core`** : aucun test ne pouvait l'atteindre, et c'est là qu'un budget d'une seconde a coûté un bureau. Tout ce qui doit être testable doit vivre hors de `main.cpp` |
| `src/input/` | Machine à états du clavier et de la souris, table des accords `<leader>` | `\033` seul est ambigu : le démon arme un délai de 50 ms. Sans lui, `vim` est inutilisable |
| `src/vt/` | Émulation VT : parseur DEC, écran, historique, SGR, modes, réponses, jeux de caractères | `Screen` est pure : ni descripteur, ni horloge. C'est ce qui la rend fuzzable |
| `src/pty/` | Pseudo-terminal, environnement de l'enfant | `Pty::shutdown()` porte **toute** la politique de fermeture (SIGHUP, maître, SIGKILL) : le destructeur et la fermeture d'onglet l'appellent tous deux |
| `src/wm/` | Fenêtres, pile, décorations, hit-test, ancrage, rangement | `hit_window()` est l'inverse exact de `draw_decor()` ; les deux lisent la **même** géométrie |
| `src/shell/` | Panneau, menu, modale, aide, horloge, moniteur de fond, assistance à l'ancrage | Chaque composant calcule sa géométrie **une fois** dans `layout()`, et `draw()` comme `hit()` la relisent |
| `src/daemon/` | Boucle `epoll`, session, hôte applicatif, démonisation, récolte, **journal** (`journal.hpp`), **réglages de l'utilisateur** (`config.hpp`) | `Session::render()` compose **toute** la géométrie ; `Session::on_mouse()` route les gestes. `config.hpp` est l'inverse de `shell/update_state.hpp` : ici le C++ ÉCRIT, et le fichier reste modifiable à la main |
| `src/app/` | Le contrat `App` / `Host`, le catalogue du menu | Tout est virtuel avec un défaut utilisable : une application qui ne sait que dessiner n'écrit qu'une méthode |
| `src/apps/` | Terminal (à onglets), Fichiers (façon Dolphin), Éditeur | Chacune ne connaît que `View`, `Host` et les événements |

### Ce qu'une application a le droit de demander (`src/app/app.hpp`)

`set_title`, `request_close`, `invalidate`, `watch`/`unwatch` (un jeton opaque, jamais
l'epoll), `watch_child` (la récolte est globale au démon), **`open_app`** — « ouvre
ça dans sa propre fenêtre », par quoi Fichiers ouvre l'Éditeur sans rien savoir du
bureau — et **`start_dir` / `configured_start_dir` / `set_start_dir`**, le dossier où
s'ouvre un nouveau terminal : un réglage de l'utilisateur, écrit sur disque, qu'une
application ne doit ni localiser ni relire elle-même. Le premier rend le chemin
EFFECTIF (`~` développé, dossier de l'utilisateur à défaut), le second ce qu'il avait
TAPÉ — c'est ce qu'on lui remontre quand il rouvre la saisie.

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

> La suite entière prend **36,4 s en Release** et **65,0 s sous ASan/UBSan**
> (mesuré le 20 août 2026 au soir, 1303 cas). L'essentiel de ce temps est de
> l'attente délibérée : `user+sys` ne fait que **4,5 s** des 36,4 s de mur (1,11 s
> d'utilisateur, 3,36 s de système), donc **88 % du temps est passé à attendre des
> sous-processus** — pseudo-terminaux, démons, copies — et non à calculer.
>
> ⚠️ **Ces chiffres bougent avec la charge de la machine**, pas seulement avec le
> nombre de cas : la mesure du 20 août a été prise pendant qu'une session de
> travail tournait dans le bureau installé. Pour référence, la même suite tenait
> en **19,6 s / 47,3 s** le 15 août avec 1146 cas. Les **dix** cas de
> `tests/test_launch.cpp` — six écrits d'abord, quatre nés de campagnes de
> mutation — ajoutent à eux seuls quelques secondes, et c'est assumé : ils
> attendent de vrais démons, ce qui est précisément ce qu'aucun test purement
> unitaire ne pouvait faire.

**Ajouter un fichier de tests ne demande rien** : `CMakeLists.txt` fait un
`GLOB tests/test_*.cpp`. Le nom du fichier n'a pas d'importance, mais le **préfixe
des `TEST(...)` sert de filtre** en ligne de commande — d'où `files_`, `copy_`,
`terminal_`, `session_`, `daemon_`, `dir_`, `snapassist_`…

> ⚠️ **UNE FIXTURE NE DOIT PAS NETTOYER SEULEMENT CE QU'ELLE A CRÉÉ.** Le code
> testé crée AUSSI — c'est même tout l'objet de Fichiers et de `FileJob` : ils
> copient, déplacent, fabriquent des dossiers, et un travail arrêté en cours de
> route laisse un reste à mi-chemin. Deux `Tree` ne retiraient que leur propre
> liste : le `rmdir` de la racine échouait alors sur `ENOTEMPTY`, **en silence**,
> et l'arbre entier fuyait. Mesure du 21 août 2026 : **1943 répertoires** oubliés
> dans `/tmp` — un tmpfs de 2,7 Go qu'une compilation d'essai a déjà rempli une
> fois. Le nettoyage se fait par `nftw(..., FTW_DEPTH | FTW_PHYS)` ; **`FTW_PHYS`
> n'est pas une précaution** : sans lui, `nftw` suit les liens symboliques, et
> cette suite en fabrique — effacer la cible d'un lien est le pire dégât qu'un
> gestionnaire de fichiers puisse faire.
>
> Et le répertoire d'isolation `XDG_DATA_HOME` du harnais n'était jamais repris :
> un par **lancement** du binaire. ⚠️ **Pas d'`atexit()` pour le faire** — le
> lanceur forke un ouvrier qui sort par `std::exit(0)`, lequel déroule les
> `atexit` : il effacerait le répertoire sous les pieds des lots suivants. Seul le
> superviseur atteint la fin de `main()`.
>
> ⚠️ **Tous les cas tournent dans le MÊME processus**, et ceux de `test_daemon.cpp`
> appellent `reap_children()`, qui fait `waitpid(-1, WNOHANG)`. Un cas qui `fork()`
> doit donc **récolter ses propres pids avant de rendre la main**, même ceux qu'il
> vient de tuer : un zombie oublié est ramassé par le premier cas de démon qui passe,
> et le `try_reap()` qui l'attendait reçoit `ECHILD` pour toujours. Mesuré : un échec
> **sur quatre lancements**, sur un cas que personne n'avait touché.

---

## 6. Ce que contient le jalon 1

13 tâches, toutes livrées, relues et fusionnées. **70 commits** depuis `main` —
*à l'époque du jalon 1 seul*, et le chiffre se recalcule :
`git rev-list --count main..cfce3cf^` rend bien 70, `cfce3cf` étant le commit qui a
écrit cette phrase. La branche en porte **200** au 15 août 2026.

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
| `src/client/client.*` | Boucle client |
| `src/daemon/daemonize.*` | Détachement (double `fork`, `setsid`) |
| `src/daemon/session.*` | État de session, composition de l'écran |
| `src/daemon/daemon.*` | Boucle `epoll` du démon |
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

## 6 bis. Ce qu'ont ajouté les jalons 2 à 7

Le §6 ci-dessus ne couvre que le jalon 1 — ses 36 fichiers. Voici les 72 autres,
**fichier par fichier** : les deux sections réunies couvrent les **108 fichiers de
`src/`** tels qu'ils étaient au 15 août, sans trou. Les 16 venus depuis sont
au §3 (le tableau « après la v1 ») et dans la liste des entrées hors plan
juste sous le tableau ci-dessous. La carte se recalcule intégralement :

```bash
for f in $(git ls-files 'src/*'); do
  git log --diff-filter=A --format="%h %ad $f" --date=short -1 -- "$f"
done | sort -k2
```

Elle ne rend que les fichiers **présents à `HEAD`** : rien pour le `copy.*` du jalon 7,
renommé et supprimé le 15 août, ni pour les six fichiers retirés — voir les deux
paragraphes sous le tableau. Elle rend aussi les 36 fichiers du jalon 1, qui sont au
§6. Et elle date `shell/help.*` du 12 août, ce qui ne veut pas dire « jalon 3 » : lire
le piège signalé plus bas.

Les totaux de cas et de mutations sont ceux du **« Bilan du jalon »** de chaque plan,
qui reste la source de vérité déclarée. Lire d'abord l'avertissement du §6 ter : deux
de ces bilans ne s'accordent pas avec les cases du plan qui les porte.

| Jalon | Plan | Tâches | Cas au bilan | Mutations | Fichiers de `src/` nés pendant |
|---|---|---|---|---|---|
| **2** — WM, panneau, menu | `2026-08-11-…-m2-wm.md` | 11 | — | — | `render/theme.*` · `app/app.hpp` · `app/catalog.*` · `wm/window.*` · `wm/decor.*` · `wm/hittest.*` · `wm/manager.*` · `wm/layout.*` · `daemon/host.*` · `shell/panel.*` · `shell/clock.*` · `shell/menu.*` · `shell/modal.*` · `input/shortcuts.*` |
| **3** — Terminal | `2026-08-12-…-m3-terminal.md` | 14 | 733 | 246 | `pty/pty.*` · `pty/env.*` · `vt/parser.*` · `vt/sink.hpp` · `vt/screen.*` · `vt/attrs.*` · `vt/modes.*` · `vt/scrollback.*` · `vt/reply.*` · `vt/charset.*` · `input/encode.*` · `daemon/reap.*` · `apps/terminal.*` |
| **4** — Fichiers | `2026-08-13-…-m4-fichiers.md` | 5 | 818 | 105 | `apps/files/dir.*` · `apps/files/files.*` |
| **5** — Moniteur | `2026-08-13-…-m5-moniteur.md` | 3 | 858 | 47 | `apps/monitor/procstat.*` |
| **6** — Éditeur | `2026-08-13-…-m6-editeur.md` | 3 | 909 | 52 | `apps/editor/buffer.*` · `apps/editor/editor.*` |
| **7** — Dolphin | `2026-08-14-…-m7-dolphin.md` | 9 | 1104 | 120 | `apps/files/copy.*` — le 15 août (`0d5bb09`), `copy.hpp` est **renommé `apps/files/job.hpp`** (git le détecte à 63 % de similarité) tandis que `copy.cpp` est **supprimé** et remplacé par un `apps/files/job.cpp` neuf |

**Sept entrées de `src/` ne viennent d'aucun plan** — aucun des sept ne les nomme :

- `shell/help.*` (l'aide de découvrabilité, `e8878b8`). Attention au piège : elle naît
  le 12 août, **avant** le plan du jalon 3 (`a8e0bfd`, deux commits plus loin), dans la
  foulée du jalon 2. La dater par le calendrier la rangerait à tort au jalon 3.
- `wm/tile.*` (ranger les fenêtres, `8b7cebd`).
- `shell/sysinfo.*` (le moniteur devenu widget de fond, `444bac6`).
- `shell/snapassist.*` (l'assistance à l'ancrage, `b888ec5`).
- `apps/files/job.cpp` (`0d5bb09`) — le fichier est neuf. Son en-tête `job.hpp`, lui,
  descend du `copy.hpp` du jalon 7 par renommage, et n'est donc pas hors plan.
- `client/launch.*` (le lancement du démon sorti de `main.cpp`, `5b792e6`).
- `render/gauge.*` (la barre, extraite de `shell/sysinfo.cpp`, `abd6ae4`).

**Trois fichiers ont été retirés du produit**, et il faut le savoir avant de les
chercher : `apps/bloc.*` et `apps/battement.*`, les deux applications factices du
jalon 2, sont devenues des doublures de test dans `tests/fake_apps.hpp` au commit
`f119b26` ; `apps/monitor/monitor.*` a disparu au commit `444bac6`, quand le moniteur
est devenu un widget du fond d'écran.

### 6 ter. Trois choses que ces plans ne disent pas d'eux-mêmes

1. **Le plan du jalon 2 n'a jamais été annoté.** Il porte 11 `### Task` et **82 cases,
   toutes vides** — c'est le seul des six dans ce cas. Ses tâches n'ont ni hash, ni
   nombre de tests, ni nombre de mutations. Le rattachement de ses fichiers au plan est
   donc **reconstruit depuis `git log`** — où le 11 août porte **douze** commits
   `feat(...)`, et non onze : les onze premiers, de `6418e61` à `c75f8ee`,
   correspondent un pour un et dans l'ordre aux onze tâches ; le douzième, `3c4190e`,
   arrive après la revue de jalon. Aucune case ne le confirme. C'est aussi cohérent
   avec le §8 bis : **la campagne de mutation systématique ne commence qu'au jalon 3.**
2. **Deux bilans ne s'accordent pas avec les cases de leur propre plan — mais lire
   d'abord le périmètre qu'ils annoncent.** Le jalon 3 écrit « **246 mutations** jouées
   **sur les tâches 5 à 13** » : la somme des cases de ces neuf tâches-là fait **267**
   (celle des quatorze fait 411, hors périmètre). Le jalon 7 écrit « **120 mutations**
   jouées sur les huit tâches de code », et la somme de ces huit cases fait **101**.
   Restent deux écarts réels, **21** et **19**, non reconstituables après coup — une
   campagne ne se rejoue pas sur un code qui a changé. Ce dossier cite les **bilans**,
   et cette note existe pour qu'un contexte neuf qui referait l'addition ne croie pas
   avoir trouvé un mensonge.
3. **Le total des mutations des jalons 3 à 6 est cohérent, lui :** 246 + 105 + 47 + 52
   = **450**, et le plan du jalon 6 écrit « plus de 450 mutations sur les jalons 3
   à 6 » (l. 64). Les bilans s'accordent donc entre eux ; ce sont les cases internes
   des plans 3 et 7 qui divergent.

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

  Sortie exacte contre le code d'avant, **telle qu'elle a été relevée le 11 août
  2026** — les numéros de ligne et le total de cas sont ceux d'alors ; les trois cas
  vivent aujourd'hui aux lignes **2609, 2683 et 2831** de `tests/test_session.cpp` :

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
  `b8964e6ed7d59eb66217258573af5a24dc1d0dcfce8507b7fa9d880adf463909`. **Cette
  empreinte se recalcule** — c'est celle de `src/daemon/daemon.cpp` au commit du
  round : `git show 4aa774f:src/daemon/daemon.cpp | sha256sum`.
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
| **Rétention mémoire par connexion** | **~34 Mio par client en régime nominal, ~67 Mio au pire — dans les deux cas, pas les ~41 Mio qu'annonçait ce dossier.** Le `Decoder` ne rend sa capacité que si son tampon est entièrement drainé *et* que la capacité atteint `kReleaseCapacityThreshold = kMaxBufferBytes` = 32 Mio + 5 o + 1 Mio ≈ **33 Mio** (`src/common/proto.cpp:123`). Mais `feed()` ne borne que les octets **non consommés**, et `compact()` ne décale qu'à partir de `pos_ * 2 >= buf_.size()` : `buf_.size()` peut donc atteindre **~2× `kMaxBufferBytes` ≈ 66 Mio** avant le premier décalage. Ce facteur 2 n'est pas une fuite, c'est le prix assumé d'une compaction à coût amorti linéaire — le commentaire de `kMaxBufferBytes` (`src/common/proto.hpp:91-99`) l'écrit noir sur blanc, et `proto_decoder_releases_capacity_once_fully_drained_past_threshold` (`tests/test_proto.cpp:394`) le couvre. L'`OutQueue`, elle, ne retient qu'environ **1 Mio** : son seuil de libération de 8 Mio (`src/common/outqueue.cpp:41`) est une garde de la classe réutilisable, **hors d'atteinte dans le démon**, où chaque client construit sa file avec `kBackpressureCeiling = 1 Mo` (`src/daemon/daemon.cpp:33`) et où `push()` rejette *avant* de faire grossir le tampon. Acceptable à un client ; **c'est le pire cas, pas le nominal, qui dimensionne le multi-client.** |
| **« Le fuseau de qui ? »** | Le démon affiche *son* fuseau. Avec un client distant dans un autre fuseau, c'est faux. Le message `Hello` devra porter le fuseau du client. |

### 7.3 — Points mineurs connus

- **Garde A2 non discriminable.** Une garde du code est conservée mais aucun test ne la
  distingue. Constaté honnêtement à deux reprises plutôt que maquillé par un test complaisant.
- **`~DaemonHandle`** tue un pid qui pourrait avoir été recyclé (*Minor*).
- Le plan `docs/superpowers/plans/2026-08-10-ssh-os-m1-noyau.md` porte **6 marqueurs
  `PERIME`** sur des blocs dont le code a divergé — l. **1890** (`class Decoder`),
  2199, 2284, 4314 (signatures de `net.hpp`), **4320** (l'éviction à l'`accept`) et
  4465 (`read_boot_id()` peut désormais lever). Les lire comme tels.

---

### 7.4 — L'état au 15 août 2026

Ce qui suit remplace toute lecture d'avancement faite ailleurs. Les §7.1 à 7.3
ci-dessus datent du jalon 1 : 7.1 est **soldé**, 7.2 et 7.3 restent **vrais**.

| Point | État | Ce qu'il coûte aujourd'hui |
|---|---|---|
| Rétention mémoire par client (**~34 Mio nominal, ~67 Mio au pire** — pas 41) | ouvert, §7.2 | Acceptable à un client ; le détail du calcul est au §7.2. Deux choses y sont contre-intuitives : le seuil de 8 Mio de l'`OutQueue` n'est jamais atteint dans le démon, et le tampon du `Decoder` peut doubler avant sa première compaction |
| Fuseau horaire du démon, pas du client | ouvert, §7.2 | L'horloge du panneau **et** la colonne « Date » de Fichiers mentent pour un client distant. Le `Hello` devra porter le fuseau |
| Garde A2 non discriminable | ouvert, §7.3 | Conservée et déclarée ; aucun cas ne la distingue |
| `~DaemonHandle` tue un pid recyclable | ouvert, §7.3 | *Minor* |
| 6 marqueurs `PERIME` dans le plan du jalon 1 | ouvert, §7.3 | À lire comme tels |
| Menu contextuel du clic droit sur la **barre des tâches** | ouvert | Il agit directement (nouvelle instance) sans rien proposer. Le gestionnaire de fichiers, lui, en a un depuis le 15 août |
| Semis de points du fond d'écran | retiré volontairement | Il rendait chaque repeint complet **27 % plus gros** (mesuré), ce qui faisait basculer `daemon_dirty_overflow_closes_the_connection` du rejet *Dirty* au rejet *Clean*. Le commentaire qui dit pourquoi est resté là où il se rebrancherait |
| `Pty::saw_eof()` sans lecteur | documenté sur place | Sous Linux un maître dont le dernier esclave s'est fermé rend `EIO`, pas 0 : `note_eof()` n'est atteinte que dans des cas de bord |
| `set_cloexec()` sans appelant | documenté sur place | Chaque descripteur naît déjà `CLOEXEC` en un seul appel système, ce qui est le motif **sûr** |

**Cette table est la dette d'ARCHITECTURE.** Ce qui manque au gestionnaire de fichiers
est listé à part, au §10 (« Le carnet du gestionnaire de fichiers ») : le conflit de
noms, les permissions, la corbeille, et surtout l'annulation, que rien n'offre
aujourd'hui. **Les quatre défauts fonctionnels** qu'un audit y avait trouvés le 15 août
— dossier non vide insupprimable, lien symbolique qui part dans l'Éditeur, bascule des
cachés qui n'atteint qu'un panneau, travail qu'aucun geste n'arrête — **ont tous été
corrigés le jour même** ; le §10 en porte le détail. **Ne pas lire cette section comme
« il n'y a rien d'autre ».**

Le balayage des méthodes sans appelant (§9 bis) a été passé le 15 août : quatre
orphelines retirées, les candidates restantes toutes vérifiées à la main.

---

## 8. Méthode de travail

L'utilisateur a choisi le **mode 1** : *un sous-agent frais par tâche, revue entre chaque*
(SDD — subagent-driven development).

> ⚠️ **`.superpowers/` est ignoré par git** (`.gitignore:3`). Le ledger, les 20 briefs,
> les 9 rapports de tâche et les **17** diffs de revue **n'existent que sur le disque de
> cette machine** et disparaissent à un `clone`. Ce qu'un contexte neuf doit absolument avoir a donc été recopié dans `docs/`.

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
   (`test_daemonize.cpp:519`) discriminait ; `CHECK(elapsed_ms >= 3500)` (`:498`) passait
   même sous la mutation.

---

## 8 bis. Le rythme réel depuis le jalon 3, et la campagne de mutation

Le mode « un sous-agent frais par tâche » du §8 est celui du jalon 1. **Depuis le
jalon 3, le rythme est direct et invariant :**

1. **Les tests d'abord, et le rouge est CONSTATÉ**, pas supposé. Un cas qui passe du
   premier coup ne prouve rien : il faut l'avoir vu échouer pour la bonne raison.
2. Le code minimal qui le fait passer.
3. **Campagne de mutation** sur ce que la tâche vient d'écrire.
4. **Chaque survivante devient un cas**, ou une équivalence **déclarée sur place**
   dans un commentaire qui dit pourquoi elle est inobservable.
5. Un commit par tâche, message en français **sans accents** (le corps du code, lui,
   est accentué — cf. `ssh-os-2-conventions-de-code`).

### La campagne de mutation — la recette et ses pièges

Un script Python remplace une ligne du code de production par une variante fausse,
recompile, relance la suite filtrée, restaure, recommence. Une mutation « morte » est
une mutation qu'un test a mordue ; une « survivante » est presque toujours **un trou
de test**, pas une équivalence — 2 sur 246 seulement l'étaient au jalon 3.

Les exemplaires de ces campagnes sont dans le scratchpad de session (`mutate_tNN.py`).
Cinq règles, toutes payées comptant :

1. **Commiter AVANT.** La sauvegarde n'est fiable que si l'arbre est propre.
2. **Sauvegarde fraîche et complète** de tous les fichiers mutés, refaite à chaque
   campagne — pas celle de la campagne d'avant.
3. **Restaurer par `shutil.copyfile` + `os.utime`, jamais `copy2`** : `copy2`
   préserve la `mtime`, `make` ne recompile pas, et le binaire testé **reste muté**.
4. **Ne rien lire d'autre dans `src/` pendant la campagne** : le fichier y est faux.
5. **Vérifier le filtre de tests.** Une campagne dont `FILTERS` ne couvre pas les cas
   qui mordent rend « 8 survivantes » qui n'en sont pas — arrivé le 14 août avec un
   filtre `["files_"]` qui ne voyait pas `copy_`.

> Et : **tuer les campagnes orphelines après tout redémarrage.** Un travail de fond
> survit à une coupure et continue de muter `src/` sous les doigts. Vérifier avec
> `pgrep -af mutate` **avant** de croire un échec de test.

### Les deux campagnes du 20 août 2026

| Campagne | Fichiers mutés | Filtres | Bilan |
|---|---|---|---|
| **Le redémarrage** | `client/launch.{cpp,hpp}`, `daemon/daemon.cpp` | `launch_`, `daemon_`, `session_` | 11 mutations : **6 mordues d'emblée**, 3 survivantes devenues des cas, 2 invalides |
| **La progression** | `shell/update_state.cpp`, `shell/update_service.cpp`, `shell/modal.{cpp,hpp}`, `render/gauge.cpp`, `daemon/session.cpp` | `update_state`, `update_service`, `modal`, `session_`, `sysinfo`, `golden` | 13 mutations : **10 mordues d'emblée**, 1 survivante devenue un cas, 2 invalides |

Les quatre survivantes, et le trou qu'elles ont montré :

- **la patience tombée à zéro** — aucun cas ne pinait le seuil d'annonce ;
  une attente courte doit rester **muette**, sinon le message ne veut plus
  rien dire le jour où il compte ;
- **la récolte de l'intermédiaire retirée** — un zombie ne se voit nulle
  part, jusqu'à ce qu'il casse un cas de `test_daemon.cpp` une fois sur dix
  (son `reap_children()` fait `waitpid(-1)`) ;
- **un échec de `bind` autre qu'une adresse prise** ne laissait pas de
  ligne — c'est pourtant le seul qui rende 1, donc celui dont on veut la
  raison ;
- **une boîte de progression rouverte** gardait le pourcentage du travail
  précédent : un travail neuf pouvait démarrer à 93 %.

**Et une mutation invalide qui vaut d'être notée :** remettre l'ouverture du
journal **après** le `bind` **ne compile plus**, les blocs `catch` s'en
servant. Comme pour le `std::visit` exhaustif du §9 bis, le garde est le
compilateur, pas une relecture.

### Deux mutations sur trois ne compilent pas pour rien

`-Werror` refuse une variable devenue inutilisée. Une mutation qui retire le seul
usage d'un paramètre ne compile pas et n'est **pas** une survivante : c'est une
mutation invalide, à compter comme telle et non comme un succès.

---

## 8 ter. Les trois outils, désormais versionnés

Ils vivaient dans un scratchpad de session — ou, pour le troisième, dans un bloc de
markdown de ce dossier — et n'étaient donc ni lançables, ni éprouvables, ni
versionnés. Ils sont maintenant dans `tools/`, et leur docstring porte les règles.

```bash
python3 -u tools/sonde.py                    # sonde de fumée : le bureau se lève
python3 tools/balayage.py --strict           # les fonctions sans appelant
cp tools/mutation.py /var/tmp/ma_campagne.py # puis remplir FILES et M
DRY=1 python3 /var/tmp/ma_campagne.py        # vérifie chaque motif AVANT
python3 -u /var/tmp/ma_campagne.py > camp.log  # jamais derrière un tube
```

- **`tools/sonde.py`** — la boîte à outils des sondes bout-en-bout : `spawn()`
  (démon neuf), `screen()` (rejoue une trame en grille), `suivre()`, `clic()`,
  `glisser()`, `trouve()`, `jiffies()`. **Quatre des dix défauts du §9 bis n'ont été
  vus que par une sonde.**
  ⚠️ `spawn()` POSE `SSHOS_BOOT_ID`, et `demons()` ne reconnaît QUE ce qui le
  porte : c'est la quatrième règle dure de sa docstring, et elle est là parce que
  l'énumération « `--daemon` + même uid » qui la précédait **tuait le bureau
  installé de la machine** — donc la session de travail qui tourne dedans.
- **`tools/mutation.py`** — le harnais de campagne, avec ses cinq règles en
  docstring et un mode `DRY=1` qui vérifie que chaque motif existe **exactement une
  fois** avant de toucher au code.
- **`tools/verif_redemarrage.py`** — « Redemarrer pour terminer » doit redémarrer
  **deux fois de suite**. Le deuxième tour est là depuis le 21 août 2026 : un seul
  redémarrage vérifié ne voyait rien du défaut du compteur (§2 sexies), qui ne se
  manifeste qu'au second d'une même session cliente.
- **`tools/balayage.py`** — les fonctions déclarées dans `src/**.hpp` sans appelant
  de production, avec ses **cinq** pièges en docstring, une liste d'exemption
  nommée, un `--strict` qui sort non nul, et un `--racine` pour l'éprouver contre
  un arbre dont on connaît la réponse. **C'est le seul des trois qui ait déjà menti
  deux fois** (§9 bis).

> ⚠️ **`/var/tmp`, pas `/tmp`.** `/tmp` est un tmpfs de 2,7 Go sur cette machine :
> une compilation d'essai l'a rempli, et cmake a annoncé un compilateur cassé.

---

## 9. Pièges d'environnement — faux positifs récurrents

Tous ont été rencontrés pour de vrai, plusieurs fois. Ils font perdre des heures.

| Piège | Parade |
|---|---|
| **`ps` / `pgrep -f` matche sa propre ligne de commande.** Survenu **3 fois**, dont un « défaut reproduit » entièrement faux. | **Ne jamais identifier un processus par correspondance de nom.** Utiliser `/proc/PID/cwd`, ou **`~/.local/bin/termos --status`** (qui s'appuie sur `SO_PEERCRED`). ⚠️ **Le LANCEUR, jamais le binaire nu** : `~/.local/libexec/termos --status` répond « aucun demon » alors que le bureau tourne. Seul le lanceur pose `TERMOS_BOOT_ID` ; le binaire appelé directement compose un autre nom de socket et conclut à juste titre qu'il n'y a personne — sur un nom qui n'a jamais existé. Même famille que le piège ci-dessus, et il a menti au premier essai le 21 août. |
| **`grep -i FAIL`** matche le nom de test `..._after_failed_explicit_release`. | Filtrer sur la ligne de bilan, pas sur une sous-chaîne. |
| **`$PPID` est figé à l'initialisation du shell.** Mesuré `$PPID=2757136` (parent déjà mort) contre un ppid réel de `1`. A produit un test instable à 2/30. | `$(cut -d' ' -f4 /proc/$$/stat)`. |
| **`dash` réinitialise le masque de signaux hérité**, mais **pas** les dispositions `SIG_IGN`. | Tester avec `/bin/cp`, pas `sh -c grep`. |
| **`redirect_std_to_devnull()`** écrase les fd 0/1/2 avant `execv`. | En tenir compte dans toute sonde qui espère lire une sortie. |
| **`stat` en shell ne déréférence PAS un lien ; `::stat()` en C, si.** `stat -c '%d %i' /proc/PID/exe` rend le périphérique et l'inode **du lien magique** (dev 25 = procfs), pas de sa cible. A produit, le 20 août 2026, la conclusion entièrement fausse que `running_is_installed()` (`src/shell/update_service.cpp`) ne pouvait jamais être vrai sur cette machine. | `stat -L`, ou `os.stat` en python. Et pour trancher **sans dépendre d'aucun lien** : `/proc/PID/maps` montre directement le périphérique et l'inode des pages de code mappées. |
| **Injecter des octets dans un pty :** écrire sur `/dev/tty` depuis son propre shell n'atteint **pas** le pty du programme testé. | Alimenter l'entrée via un tube nommé : `script -qc "$S" /dev/null < fifo`, en gardant le tube ouvert (`exec 3>fifo`). |

---

## 9 bis. Le défaut signature du projet — et comment le trouver en deux minutes

**Vingt-quatre fois**, du code a existé sans aucun appelant en production. Aucune
suite de tests ne l'a jamais signalé, parce que ce qui manque n'est pas la
couverture : c'est **l'appel**. Un test unitaire ne peut pas le voir. Une campagne
de mutation non plus — muter du code mort ne casse rien, et la mutation se déclare
« équivalente ».

**Le compte, pour qu'il soit vérifiable plutôt que cité** — c'est la seule liste de
ce dossier qui ne se recalcule pas, d'où l'arithmétique posée à plat :

| Rangs | Quoi | Quand |
|---|---|---|
| 1 – 10 | le tableau ci-dessous, chacune ayant coûté quelque chose | jalons 1 à 7 |
| 11 – 14 | les accesseurs morts retirés au commit `e32f09c` — `Screen::autowrap()`, `Files::other()`, `LeaderDispatch::phase()`, `Menu::query()` | 15 août |
| 15 | la molette (`2b4dfc0`) | 18 août |
| 16 | le collage (`6828642`) | 19 août |
| 17 | `restart_done_` — un *membre* écrit et jamais lu, plutôt qu'une méthode | 20 août |
| 18 – 24 | `Modal::style()`, `Modal::is_info()`, `Panel::update_badge()`, `Terminal::modes_for_tests()`, `SysInfo::tx_rate_for_tests()`, le champ `Cell::cluster` et la queue de `Session::run_update_command` | 20 août au soir |

> ⚠️ **Deux comptes faux traînent dans l'historique, et il faut le savoir avant de
> refaire l'addition.** Le message de `e32f09c` numérote ses quatre objets
> « onzieme a treizieme » — trois ordinaux pour quatre objets ; c'est le message
> qui compte mal, pas la liste. Et ce dossier a longtemps affiché « seize », en
> traînant ce décalage. Par ailleurs le commit du 20 août au soir qui annonce
> « sept objets » n'en porte que **cinq** : `Modal::style()` et `Modal::is_info()`
> ont voyagé avec le commit du cadre de modale, juste avant. Le contenu est le
> même, seule l'attribution entre deux commits voisins diffère.

Les **rangs 18 à 24** ne coûtaient rien à l'exécution — sauf `Cell::cluster`, quatre
octets sur **chaque cellule de chaque `Surface`** pour un réservoir de grappes qui
n'a jamais été construit — mais tous coûtaient à la lecture, et deux d'entre eux
faisaient remonter du bruit à chaque passage du balayage.

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
| 16 | **Le collage** | `Session::on_input` démontait `InputEvent` par une chaîne de `get_if` : touches, souris, focus — et **pas** le collage. Le parseur fabriquait des `PasteEvent` que personne ne lisait, donc **coller dans le bureau ne faisait rien**, jamais. La molette (n° 15, `2b4dfc0`) est la même histoire, côté souris |

**Le n° 10 est le plus instructif.** Ses six cas unitaires appelaient
`files.on_mouse(Motion…)` **directement**. Ils prouvaient que le gestionnaire réagit
bien à un mouvement ; ils ne prouvaient rien sur le fait que quelqu'un lui en envoie.
**Un test qui appelle la méthode lui-même ne teste jamais son appelant.**

### ⛔ Le garde permanent — le compilateur, pas un script

**Depuis `6828642`, la seizième ne peut plus se reproduire à cette porte-là.**
`Session::on_input` ne démonte plus `InputEvent` à la main : il fait un
`std::visit` sur un ensemble de surcharges **exhaustif** (l'idiome `overloaded`,
en tête de `session.cpp`).

Ajouter une alternative au `variant` sans la traiter **ne compile plus** — vérifié en
ajoutant un `EssaiEvent` de test, qui produit `no matching function ... const
sshos::EssaiEvent&`. Le garde n'est ni une relecture, ni le balayage ci-dessous — il
tombe sur **toutes** les machines, à **chaque** compilation, et il nomme le type oublié.

> **Ne jamais** ajouter de `default`, de surcharge générique `auto&&` ni de fourre-tout
> dans ce `visit` : chacun de ces trois gestes rend le silence à la porte d'entrée du
> bureau, et le vingt-cinquième arrivera par là.

Ce que le garde ne couvre PAS, et qu'il faut donc continuer à surveiller : les
**virtuelles** de `App` et de `Host`. Une méthode virtuelle est référencée par la
vtable même si personne ne l'appelle — ni le compilateur ni l'éditeur de liens ne
peuvent la dire morte. Les n° 4, 5 et 10 étaient de celles-là. Pour elles, la seule
parade reste un test qui entre par la **vraie** porte (`Session::on_input`) plutôt
que d'appeler la méthode lui-même, plus le balayage ci-dessous.

### Le balayage, à repasser après tout gros ajout

**Il ne vit plus dans ce document : c'est `tools/balayage.py`.**

```bash
python3 tools/balayage.py            # la liste des candidats
python3 tools/balayage.py --strict   # sort non nul si un candidat n'est pas exempté
```

Il vivait ici, dans un bloc de markdown, comme y vivaient `sonde.py` et
`mutation.py` avant le §8 ter. **Un outil de vérification qu'on ne peut ni lancer
ni éprouver dérive** — et celui-ci l'a fait deux fois, sans que personne ne puisse
s'en apercevoir (les deux démonstrations sont plus bas). Le sortir d'ici est le
même geste que pour les deux autres, pour la même raison.

Sa docstring porte les **cinq pièges** et les cinq parades. Les quatre premiers
sont d'anciens acquis ; le cinquième a été trouvé en le rejouant le 20 août :

> **Un appel précédé d'un opérateur passait pour une déclaration**, et l'appel
> était alors **jeté** — donc une fonction bel et bien appelée ressortait comme
> orpheline, à chaque passage, et coûtait le même tri manuel. Deux formes réelles
> dans ce dépôt :
>
> ```cpp
> out << render_config(c);                  // config.cpp:89
> name, sshos::daemon_exe_path(), [] {      // main.cpp:45
> ```
>
> Parade : une fois les chevrons appariés retirés du préfixe, il ne doit plus
> rester ni virgule, ni chevron, ni signe d'égalité — les chevrons d'un type
> générique gardent le droit de porter des virgules.

Et un **second passage** que la boucle d'origine ne pouvait pas faire : elle saute
les `_for_tests` par construction, donc une API de test que **plus aucun test
n'appelle** lui était invisible. Deux s'y cachaient (`modes_for_tests`,
`tx_rate_for_tests`).

> **La sortie n'est JAMAIS une conclusion.** Un candidat n'est un défaut qu'après
> un `grep -rn "\bnom\b" src/ tests/` **sans troncature**, lu à la main. Un audit
> adversarial a un jour déclaré quatre orphelines inexistantes — il les avait
> cherchées *après* leur retrait. Le rapport était affirmatif, sourcé, et faux.

**Un outil de vérification doit lui-même être vérifié contre un cas dont on connaît
la réponse**, et `--racine` existe pour ça :

```bash
git archive <commit>^ src tests | tar x -C /var/tmp/essai
python3 tools/balayage.py --racine /var/tmp/essai
```

Épreuve du 20 août, sur `0b7fdf7` — l'arbre d'avant la passe : **512 noms
déclarés**, et les cinq objets retirés ce soir-là ressortent tous
(`style`, `is_info`, `update_badge` à `src=0 tests=0`, plus les deux
`_for_tests` au second passage). Sur l'arbre d'après : **509 noms**, aucun des
cinq, et les deux faux positifs du cinquième piège ont disparu eux aussi.

**Les constructeurs ne sont pas analysés du tout**, et c'est délibéré : le groupe
de capture exige une initiale minuscule, or tous les types du projet sont
capitalisés. Élargir le motif aux majuscules ferait remonter les constructions
locales, `Type x{args};` étant syntaxiquement identique à une déclaration.

Les accesseurs suffixés `_for_tests` sont des faux positifs légitimes — **d'où le
suffixe, à mettre systématiquement** sur toute méthode qui n'existe que pour les
tests. C'est ce qui rend le balayage exploitable. **Seize API de test ne le portent
toujours pas** (`text_row`, `line_text`, `wrap_pending`, `scroll_top`,
`scroll_bottom`, `charset`, `state`, `dirty`, `choices`, `selection`, `question`,
`release`, `bound_actions`, `message`…), ce qui fait remonter quatorze candidats à
chaque passage et coûte le même tri manuel : **c'est le travail qui reste sur ce
point**, environ 255 sites d'appel, purement mécanique. ⚠️ Ne pas le faire au `sed`
global : `valid(`, `state(`, `release(`, `message(` et `question(` existent aussi
sur d'autres classes.

**Quatre candidats sont EXEMPTÉS**, nommés dans le script et documentés sur place
dans le code — `--strict` ne les compte pas :

| Exempté | Pourquoi |
|---|---|
| `Pty::saw_eof()` | Sous Linux, un maître dont le dernier esclave s'est fermé rend `EIO` et non 0 : `note_eof()` n'est atteinte que dans des cas de bord |
| `set_cloexec()` | Chaque descripteur naît déjà `CLOEXEC` en un seul appel système, ce qui est le motif **sûr** |
| `ambiguous_wide()` | Un getter que la production court-circuite : `width.cpp` lit directement le global `g_ambiguous_wide` |
| `Fd::valid()` | Le vocabulaire d'un type RAII d'usage général, contrepartie de `get()` et `reset()`. La production lève à la construction plutôt que de rendre un `Fd` invalide, d'où l'absence d'appelant ; le suffixer serait laid et se retournerait contre le premier site de production qui en aura besoin |

> 🔴 **Les deux fois où cet outil a menti, et c'est démontré.**
>
> **La version publiée jusqu'au 15 août 2026** n'enregistrait une déclaration que
> si la ligne finissait par `;` — or les quatre orphelines retirées ce jour-là sont
> **toutes** des définitions en ligne finissant par `}` :
> `bool autowrap() const { return autowrap_; }`, `Pane& other() { … }`,
> `LeaderPhase phase() const { … }`, `const std::string& query() const { … }`.
> Rejouée sur l'arbre d'avant leur retrait, elle rend **13 candidats et n'en trouve
> aucune**.
>
> **La version publiée jusqu'au 20 août** jetait les deux appels du cinquième piège
> ci-dessus : elle accusait donc `render_config` et `daemon_exe_path`, deux
> fonctions parfaitement appelées, et se taisait sur `modes_for_tests` et
> `tx_rate_for_tests`, deux API de test réellement mortes. **La leçon vaut au-delà
> de ce script** : un outil de vérification doit être éprouvé contre un cas dont on
> connaît la réponse, sinon il devient l'endroit exact où l'erreur se cache.

**Et le filet qui attrape ce que le balayage ne voit pas :** une sonde bout-en-bout
qui pilote le **vrai démon** sous pty et lance de **vrais programmes**. Les défauts
3, 4, 5 et 10 n'ont été vus que comme ça.

---

## 9 ter. Ce qu'un audit adversarial a trouvé — et ce qu'il a manqué

Le 15 août 2026, sept lecteurs indépendants ont été lâchés sur le dépôt pour écrire
ce dossier : l'historique, la suite de tests, le balayage des orphelines, la
comparaison à Dolphin, la carte d'architecture, l'audit de ce document, puis une
critique de l'ensemble. **Ils ont trouvé neuf choses que je n'avais pas vues**, dont
cinq étaient du code à réparer et non de la documentation à écrire :

| Trouvé par | Quoi |
|---|---|
| balayage | Quatre accesseurs morts (`Screen::autowrap()`, `Files::other()` — **privée**, donc pas même atteignable par un test —, `LeaderDispatch::phase()`, `Menu::query()`) |
| carte d'archi | Deux **commentaires en capitales qui mentaient**, dans un projet où ils passent pour des invariants testés |
| audit Dolphin | Le caret posé colonne 0 de la fenêtre, donc sur le liseré ; et quatre défauts fonctionnels du gestionnaire de fichiers |
| audit du doc | « 189 cas » au §2 — un contexte neuf aurait cru avoir tout cassé |
| critique | **Le projet n'existe que sur ce disque** ; les outils vivaient dans un scratchpad ; le tableau des gestes était entièrement au clavier dans un projet « souris d'abord » |

**Et ce que la critique a manqué, ce qui vaut autant :** elle a déclaré les quatre
orphelines inexistantes — elle les avait cherchées **après** leur retrait, et en
concluait que le balayage mentait. Le rapport était affirmatif, sourcé, et faux.

> **La règle qui en sort, et elle s'applique à tout rapport, humain ou non :** un
> constat n'est acquis qu'après l'avoir vérifié soi-même, sur l'état courant, sans
> troncature. Prendre un audit pour argent comptant coûte exactement ce que coûte
> de ne pas en faire.

---

## 10. Où l'on en est, et ce qui reste

**Les sept jalons sont livrés**, et depuis le 17 août 2026 **l'installation locale et
la mise à jour depuis le bureau** le sont aussi — quatorze tâches, plan clos, §2 ter.

Les 18 et 19 août 2026 s'y ajoutent six demandes venues de l'usage : **la molette**,
qui n'atteignait aucune application ; **la survie du démon** au tueur de mémoire, avec
le journal qui dit pourquoi il s'arrête ; **le dossier de départ du terminal**, qui
s'ouvrait à la racine ; **le collage**, qui n'atteignait personne non plus, et le garde
du compilateur qui ferme cette famille de défaut ; **les notes de version** dans la
fenêtre de mise à jour ; et **le retrait de la bascule souris**. Le détail est dans le
tableau du §3.

Il n'y a **pas de plan en cours** : le travail se fait à la demande, un geste à la fois,
en réaction à l'usage réel.

> **Avant toute mise à jour, le §2 quater :** il faut pousser `main` d'abord, et
> une mise à jour ne bénéficie **jamais** du correctif qu'elle installe — c'est
> mécanique, et ça vaut pour toutes les suivantes.

### La passe du 20 août 2026 au soir — les cinq fils, et trois défauts qu'ils cachaient

Les cinq fils laissés en l'air le matin du 20 août sont **soldés**. En les
vérifiant un par un — la règle du §9 ter, *un constat n'est acquis qu'après
l'avoir vérifié soi-même* — trois d'entre eux se sont révélés plus larges que
leur description, et **deux défauts que personne n'avait vus** sont sortis.

| Le fil, tel qu'il était noté | Ce qu'il était vraiment | État |
|---|---|---|
| `--check` efface `restart-pending` | Juste, et plus large : `check-failed` — le réseau qui tombe, cas le plus probable — faisait le même cul-de-sac **sans même avoir comparé**. Et `--check` écrase le fait dès sa **première** écriture, pas seulement à la dernière | ✅ soldé |
| `restart_done_` écrit sans lecteur | Juste, tel quel | ✅ retiré |
| La queue de `run_update_command` inatteignable | Juste, tel quel | ✅ retiré |
| **Deux** sondes non isolées | **Quatre**, plus une cinquième par un tout autre mécanisme (§ ci-dessous) | ✅ soldé |
| Le correctif du redémarrage ne sauve pas sa propre mise à jour | Juste — et sans objet : cette mise à jour est passée. La leçon est promue au §2 quater, où elle vaut pour toutes les suivantes | ✅ clos |

**Les trois découvertes de la passe**, aucune n'étant dans le carnet :

1. **`Modal` ne dimensionnait son cadre que pour les boutons PAR DÉFAUT.** Le
   plancher de largeur était figé sur « Annuler / Confirmer », alors que
   `cancel_rect()` et `confirm_rect()` se posent à partir des libellés réels — et
   la session en pose deux autres couples. Sur un corps court, le bouton de gauche
   sortait du cadre : peint sur le bureau, et **incliquable**, `hit()` exigeant
   d'abord d'être dans `rect_`. Mesuré sur « Plus tard / Reinstaller depuis
   GitHub » : `[ Plus tard ]` **entièrement dehors, zéro cellule cliquable**, et
   `hit()` rendant `Confirm` sur presque toute la largeur du cadre — un clic à côté
   lançait la réinstallation. C'est le défaut de `3512ffe` revenu par la porte des
   **libellés** au lieu du corps. Aucun test ne le voyait : `test_modal` n'appelait
   jamais la surcharge à quatre arguments, et le seul cas de `test_session` qui la
   traverse a un corps assez long pour que le cadre tienne **par accident**.
2. **La CI nommait « version 1.0 » toutes les releases publiées.**
   `actions/checkout` prend `fetch-depth: 1` par défaut ; la tête est alors greffée
   sans parent, `git log -1 -- VERSION` la rend elle-même, et le mineur tombe à
   zéro. Mesuré : `tools/version.sh` rend **1.29** sur l'arbre complet et **1.0**
   sur un `clone --depth 1` du même dépôt. Le titre de la release est le seul
   endroit public où le numéro s'affiche.
3. **Le surveillant de progression pouvait garder le verrou pour toujours.** Il
   hérite du descripteur du verrou, et un `flock` appartient à la description de
   fichier **partagée par le fork** : un signal externe visant le seul pid du
   script principal laissait le fils vivant, verrou pris, et toute invocation
   ultérieure répondait « un autre travail est en cours » sans qu'aucun travail ne
   coure. Aucun chemin interne n'y menait — tous les échecs appellent déjà
   `arreter_surveillance`. Un `trap` sur HUP/INT/TERM ferme le cas.

#### Les sondes : quatre tueuses, pas deux

Le carnet en signalait deux. Le motif fautif — « `--daemon` dans `cmdline` + même
uid » — vivait en **quatre copies manuelles**, dont `tools/sonde.py`, la boîte à
outils que le §8 ter documente comme **directement lançable**, et dont `spawn()`
appelle `kill_daemon()` en première instruction. Le bureau installé de la machine
porte exactement ces deux marques.

Et une cinquième, `verif_bureau_ouvert.py`, tuait par un mécanisme tout autre :
son propre énumérateur était bien isolé par `HOME`, mais elle installait avec
l'instance par défaut **`bureau01`** — celle du bureau réel — si bien que c'est
`install.sh --kill` qui tuait, et qu'aucun filtre Python ne pouvait le rattraper.

**L'étiquette est `SSHOS_BOOT_ID`, et elle a deux moitiés** : `spawn()` la POSE
dans l'enfant, `demons()` la RELIT dans `/proc/PID/environ`. L'une sans l'autre ne
vaut rien. Et c'est une marque **positive**, jamais « différent du mien » : un
démon lancé à la main depuis l'arbre de dev n'a **aucun** `SSHOS_BOOT_ID` dans son
environnement — `net.cpp` retombe alors sur l'uuid du noyau, calculé dans le
processus — donc « différent du mien » serait vrai pour lui, et on le tuerait.

`verif_isolation.py` ne balaie plus rien : il lit l'instance gravée dans le lanceur
testé et **refuse bruyamment** de tourner si un démon la porte déjà. *On ne tue pas
un bureau qu'on n'a pas levé.*

> **Comment vérifier qu'une sonde est sûre, avant de la lancer :** relever le pid
> du bureau installé (`~/.local/libexec/sshos --status`), lancer la sonde, et
> vérifier que le pid vit toujours. C'est ce qui a été fait pour chacune des cinq.

### Les fils laissés en l'air le 19 août au soir

Rien d'entamé, rien de cassé — mais quatre choses qu'un contexte neuf ne devinerait pas.

1. **Un échec unique, sous ASan, non identifié.** Apparu une fois pendant la journée du
   19, jamais reproduit sur les cinq passages suivants ni dans le conteneur. La machine
   compilait et faisait tourner des conteneurs en parallèle. Si un cas isolé tombe sans
   raison, ce n'est probablement pas le code — le relancer avant de creuser.
2. **La CI annote un Node 20 déprécié.** `actions/checkout@v4` tourne sur Node 24 par
   compatibilité. Sans effet aujourd'hui ; à passer en `@v5` un jour.
3. **Un défaut connu, délibérément NON corrigé** : sous prise de souris, les coordonnées
   locales livrées à une application ne sont pas bornées (`Session::on_mouse`), donc un
   glissement qui sort par le haut d'une fenêtre cesse d'atteindre son contenu **en
   silence**. Sans conséquence observable aujourd'hui — les deux sites d'indexation de
   Fichiers sont gardés, et le terminal n'a pas de geste de glissement. Ça ne deviendrait
   un vrai défaut qu'avec une sélection à la souris, qui n'est pas construite (§3).
4. **L'installation de l'utilisateur peut être en retard sur `main`.** Vérifier
   `installed_commit` dans `<données>/state` avant de conclure qu'un correctif « ne marche
   pas » : le 19 août, il tournait `f32161f` pendant que `main` était cinq commits plus
   loin.

### Le jalon 7 — ce qu'il change, et la contrainte qui l'a décidé

Plan : `docs/superpowers/plans/2026-08-14-ssh-os-m7-dolphin.md`. Neuf tâches,
120 mutations pour le jalon, **21 de plus** pour le lot souris qui a suivi.

**LA SOURIS D'ABORD — c'est la règle du projet, et elle a dû être redite.** Le
jalon a d'abord été livré au clavier ; l'utilisateur a demandé « **toutes** les
fonctions au bouton droit ». Le tableau ci-dessous se lit donc dans cet ordre : le
geste souris est le chemin principal, le raccourci en est le doublon.

| À la souris | Au clavier | Ce que ça fait |
|---|---|---|
| **Bouton droit dans la liste, sur le vide ou sur la ligne d'état** | — | **Le menu contextuel : 18 entrées** — 19 pendant un travail, « Arreter le travail » passant en tête — chacune avec son raccourci écrit en face. Il s'ouvre bien sur le vide et sur la ligne d'état, c'est justement là qu'on veut « Nouveau dossier » ou « Coller ». **Mais pas sur les deux lignes du haut :** dans `Files::on_mouse`, la branche du fil d'Ariane (`e.y == 0`) et celle de l'en-tête de colonnes (`e.y == 1`) rendent la main avant le test `e.button == 2` |
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
les clients pendant la copie. `FileJob` avance donc **par tranches d'un
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
   15 août, avec 18 entrées — 19 pendant un travail.
2. **Le semis de points du fond d'écran.** Il fonctionnait, mais rendait chaque
   repeint complet **27 % plus gros** (mesuré), ce qui faisait basculer
   `daemon_dirty_overflow_closes_the_connection` du rejet *Dirty* au rejet
   *Clean*. Retiré ; le commentaire qui dit pourquoi est resté à l'endroit où il
   se rebrancherait.
3. **Rétention mémoire par connexion** (~34 Mio en nominal, ~67 Mio au pire — le
   détail du calcul est au §7.2) et **le fuseau horaire du démon plutôt que celui du
   client** : §7.2, inchangés depuis le jalon 1.

**Soldé les 14 et 15 août 2026 :** l'assistance à l'ancrage (elle donne enfin un
appelant à `snap_opposite()`), le caret, le SIGKILL au groupe à la fermeture, la
documentation des gestes sans accord, les taquets de tabulation `HTS`/`TBC`, le
menu contextuel du gestionnaire de fichiers, **l'Éditeur enfin branché** — le
message « l'editeur arrive au jalon 6 » traînait depuis *avant* la livraison du
jalon 6 — le glisser-déposer, et la prise de souris qui le rendait possible.

### Le carnet du gestionnaire de fichiers — audité le 15 août 2026

Établi en relisant `src/apps/files/` face à Dolphin, et filtré : ce qui n'a pas de
sens en mode texte (vignettes, aperçus graphiques) est écarté.

**Il ne reste aucun DÉFAUT connu dans cette famille.** Les quatre qu'un audit
adversarial avait trouvés le 15 août ont tous été corrigés le jour même — la liste
est juste en dessous. Ce qui suit est du MANQUE, pas de la casse.

**Soldé le 15 août 2026, les quatre défauts du carnet :**

1. **La suppression récursive** — un dossier non vide était insupprimable depuis
   l'application. Elle est récursive, **mais** par tranches, **mais** arrêtable
   d'une touche (`Échap`, et une entrée de menu qui n'apparaît que pendant),
   **mais** la question prévient (« + contenu »), **et** fermer la fenêtre pendant
   le travail demande confirmation. La prudence a changé de place, pas de camp.
2. **L'arrêt d'un travail** — `cancel()` existait sans aucun geste pour l'appeler.
3. **Les liens symboliques** — ils partaient dans l'Éditeur. Ils se descendent, se
   rangent avec les dossiers, disent leur cible en ligne d'état, montrent la taille
   de ce qu'ils désignent, et **s'effacent sans emporter la cible** (`lstat`,
   jamais `stat` : le pire dégât qu'un gestionnaire de fichiers puisse faire).
4. **Les deux panneaux se relisent ensemble** — la bascule des cachés et les
   créations ne prenaient que d'un côté.

Le moteur s'appelle désormais **`FileJob`** : il fait les trois opérations, et
`CopyJob` mentait sur ce qu'il faisait. `refilter(Pane&)` et `settle_pane(Pane&)`
prennent un panneau — les surcharges nues `refilter()` et `settle()` agissent sur le
panneau actif ; `reload()` boucle sur les deux.

**Puis, par valeur pour un bureau en mode texte :**

| Manque | Pourquoi ça compte | Taille |
|---|---|---|
| **Conflit de noms** : `O_EXCL` refuse, et le travail ne sait que compter — « N sur M ont echoue : … File exists », posé en rouge sur la ligne d'état du panneau actif, et **seulement à la fin** du travail | Il manque Écraser / Renommer / Ignorer / Tout, et un signalement au moment du conflit | moyen |
| **Permissions et propriétaire** — le `stat()` est **déjà payé**, `DirEntry` jette `st_mode`, `uid`, `gid` | C'est l'information la plus attendue en mode texte (`ls -l`) | petit |
| **Copier au glisser** (`Ctrl` enfoncé) | Le lâcher déplace toujours | petit |
| **Rafraîchir** (`F5`) | La liste ne se relit que sur un geste de l'application — changement de répertoire, création, renommage, bascule des cachés, fin ou arrêt d'un travail. **Rien ne la relit quand le disque bouge tout seul**, et `Key::F5` n'est branchée nulle part dans `src/apps/files/` | petit |
| **Chemin éditable** (`Ctrl+L`) | On n'atteint un chemin qu'en cliquant niveau par niveau | petit |
| **Corbeille** (`~/.local/share/Trash`) | Tout est irréversible aujourd'hui | moyen |
| **Ouvrir un terminal ici** (`F4`) | `PtySpawn` n'a pas de champ « répertoire de travail » | moyen |
| Onglets, recherche récursive, signets modifiables, propriétés | Confort | moyen |
| Annuler (`Ctrl+Z`) sur copie / déplacement / renommage | Le plus gros, et le plus rentable à long terme | gros |

---

Ce que les sept jalons ont appris, et qui vaut pour la suite :

1. **Du code né sans appelant ne se signale qu'en faisant tourner le vrai
   logiciel — ou en le cherchant exprès. Vingt-quatre fois à ce jour.** La liste,
   l'outil de balayage (`tools/balayage.py`) et ses cinq pièges sont au **§9 bis**,
   qui est la section la plus rentable de ce dossier.
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
