# ssh_os 2.0 — dossier de reprise

> Document destiné à un contexte neuf. Il suppose zéro connaissance préalable de la
> conversation qui a produit le projet. Tout ce qui suit a été vérifié, pas supposé :
> quand un fait vient d'une mesure, la mesure est citée.
>
> **Dernière mise à jour :** 17 août 2026, branche `m1-noyau`. **Le dépôt est publié :**
> <https://github.com/might-stormlord/sshos> — public, sous AGPL-3.0. Le §2 bis dit
> comment, et surtout ce que la publication a changé dans l'historique.
> **1206 tests au vert** en `Release` comme sous ASan/UBSan, 0 avertissement, et
> **aussi dans un conteneur `ubuntu:26.04` nu**. Arbre de travail propre.
> **228 commits** sur `main`, 112 fichiers dans `src/`.
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
> **§3** où l'on en est · **§3 bis** la carte du code ·
> **§4** ce qui n'est pas négociable · **§6 / §6 bis** quel fichier est né à quel
> jalon, les 108 de `src/` · **§8 bis** le rythme de travail et la campagne
> de mutation · **§8 ter** les deux outils (`tools/sonde.py`, `tools/mutation.py`) ·
> **§9 bis** le défaut qui revient quatorze fois dans ce projet, et son balayage ·
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
./build-release/sshos_tests            # 19,6 s
./build-debug/sshos_tests              # 47,3 s (ASan + UBSan, facteur 2,4)
./build-release/sshos_tests files_     # filtre par sous-chaîne du nom
```

**Attendu : `1146 cas, 0 en echec, 0 assertions echouees`,** en Release comme en
Debug, avec 0 avertissement de compilation (`-Wall -Wextra -Wpedantic -Werror`).

> Le binaire de test s'appelle **`sshos_tests`** (pas `sshos-test`). Erreur commise
> plusieurs fois.

> ⚠️ **Ce total périme à chaque commit qui ajoute un cas, et il a déjà menti deux
> fois** — « 189 cas » puis « 1120 cas », chaque fois assez longtemps pour qu'un
> contexte neuf puisse croire avoir tout cassé. Le compter plutôt que le croire :
> `grep -c '^TEST(' tests/*.cpp | awk -F: '{s+=$2} END {print s}'` doit rendre le
> même nombre que la ligne de bilan de `sshos_tests`.

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

- **Pas d'en-têtes de licence par fichier.** L'AGPL les recommande ; il y a 108 fichiers
  dans `src/`. Le `LICENSE` et le README suffisent juridiquement.

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
| `python3 tools/verif_repos.py` | le démon **détaché** ne doit consommer aucun CPU |
| `python3 tools/verif_sortie.py` | « Fermer la session » tue toujours le démon — chemin qu'aucun test unitaire ne couvre |

`SSHOS_PREFIX`, `SSHOS_STATE_DIR` et `SSHOS_REPO_URL` surchargent les chemins et l'URL :
c'est ce qui permet à la sonde de ne pas écraser l'installation réelle. **`SSHOS_BOOT_ID`
n'isole QUE le nom du socket**, jamais les chemins — l'erreur a été commise une fois.

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

16 736 lignes dans `src/` sur 108 fichiers, 24 799 dans `tests/` sur 54. **Le rapport
n'est pas une coquille** : le projet écrit plus de tests que de code, et c'est ce qui rend les
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

> La suite entière prend **19,6 s en Release** et **47,3 s sous ASan/UBSan**
> (re-mesuré le 15 août 2026 sur `e6d013d`, 1146 cas, facteur 2,41). L'essentiel de
> ce temps est de l'attente délibérée : `user+sys` ne fait que **4,2 s** des 19,6 s
> de mur (1,17 s d'utilisateur, 3,03 s de système), donc **79 % du temps est passé à
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
`src/`**, sans trou. La carte se recalcule intégralement :

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

**Cinq entrées de `src/` ne viennent d'aucun plan** — aucun des sept ne les nomme :

- `shell/help.*` (l'aide de découvrabilité, `e8878b8`). Attention au piège : elle naît
  le 12 août, **avant** le plan du jalon 3 (`a8e0bfd`, deux commits plus loin), dans la
  foulée du jalon 2. La dater par le calendrier la rangerait à tort au jalon 3.
- `wm/tile.*` (ranger les fenêtres, `8b7cebd`).
- `shell/sysinfo.*` (le moniteur devenu widget de fond, `444bac6`).
- `shell/snapassist.*` (l'assistance à l'ancrage, `b888ec5`).
- `apps/files/job.cpp` (`0d5bb09`) — le fichier est neuf. Son en-tête `job.hpp`, lui,
  descend du `copy.hpp` du jalon 7 par renommage, et n'est donc pas hors plan.

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

### Deux mutations sur trois ne compilent pas pour rien

`-Werror` refuse une variable devenue inutilisée. Une mutation qui retire le seul
usage d'un paramètre ne compile pas et n'est **pas** une survivante : c'est une
mutation invalide, à compter comme telle et non comme un succès.

---

## 8 ter. Les deux outils, désormais versionnés

Ils vivaient dans un scratchpad de session et disparaissaient avec elle. Ils sont
maintenant dans `tools/`, et leur docstring porte les règles.

```bash
python3 -u tools/sonde.py               # sonde de fumée : le bureau se lève
cp tools/mutation.py /tmp/ma_campagne.py    # puis remplir FILES et M
DRY=1 python3 /tmp/ma_campagne.py           # vérifie chaque motif AVANT
python3 -u /tmp/ma_campagne.py > camp.log   # jamais derrière un tube
```

- **`tools/sonde.py`** — la boîte à outils des sondes bout-en-bout : `spawn()`
  (démon neuf), `screen()` (rejoue une trame en grille), `suivre()`, `clic()`,
  `glisser()`, `trouve()`, `jiffies()`. **Quatre des dix défauts du §9 bis n'ont été
  vus que par une sonde.**
- **`tools/mutation.py`** — le harnais de campagne, avec ses cinq règles en
  docstring et un mode `DRY=1` qui vérifie que chaque motif existe **exactement une
  fois** avant de toucher au code.

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

**Quatorze fois**, du code a existé sans aucun appelant en production. Aucune suite de
tests ne l'a jamais signalé, parce que ce qui manque n'est pas la couverture : c'est
**l'appel**. Un test unitaire ne peut pas le voir. Une campagne de mutation non plus —
muter du code mort ne casse rien, et la mutation se déclare « équivalente ».

Les dix premières sont ci-dessous, chacune ayant coûté quelque chose. Les **quatre
suivantes** sont les accesseurs morts retirés au commit `e32f09c` le 15 août
(`Screen::autowrap()`, `Files::other()`, `LeaderDispatch::phase()`, `Menu::query()`) :
ils ne coûtaient rien à l'exécution, seulement à la lecture. *(Le message de `e32f09c`
les numérote « onzieme a treizieme » — trois ordinaux pour quatre objets ; c'est le
message du commit qui compte mal, pas la liste.)*

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
KW = ("return", "case", "else", "throw", "if", "for", "while", "switch",
      "do", "delete", "new", "goto")          # parade 1 : `return foo(x);`
DECL = re.compile(r"^([A-Za-z_][\w:<>,&\*\s]*?)\s[\*&]*(?:\w+::)*([a-z_][a-z0-9_]*)\s*\(")

def decl(t):                                   # rend le nom declare, ou None
    t = t.split("//")[0].rstrip()              # parade 4 : `void set_tab();  // HTS`
    m = DECL.match(t)
    if not m or m.group(1).split()[0] in KW: return None
    # parade 2 : `T nom() const { ... }` en ligne -- c'est la forme des QUATRE
    # orphelines du 15 aout ; parade 3 : parenthese ouverte = signature etalee
    if t.rstrip().endswith((";", "{", "}")) or t.count("(") > t.count(")"):
        return m.group(2)
    return None

def lignes(rac):
    out = []
    for root, _, fs in os.walk(rac):
        for f in sorted(fs):
            if f.endswith((".cpp", ".hpp")):    # les .hpp AUSSI : beaucoup
                p = os.path.join(root, f)       # d'appels sont en ligne
                for i, l in enumerate(io.open(p, encoding="utf-8"), 1):
                    out.append((p, i, l.rstrip("\n")))
    return out

src, tst = lignes("src"), lignes("tests")
decls = {}
for p, i, l in src:
    if not p.endswith(".hpp"): continue
    t = l.strip()
    if t.startswith(("//", "*", "/*")): continue
    n = decl(t)
    if n: decls.setdefault(n, "%s:%d" % (p, i))

def compte(ls, name):
    pat = re.compile(r"(?<![\w])%s\s*\(" % re.escape(name)); n = 0
    for p, i, l in ls:
        t = l.strip()
        if t.startswith(("//", "*")) or decl(t) == name: continue
        n += len(pat.findall(t.split("//")[0]))
    return n

print("%d noms declares" % len(decls))
for name in sorted(decls):
    if name.endswith("_for_tests"): continue
    if compte(src, name) == 0:                  # zero appelant de PRODUCTION
        print("  %-26s src=0 tests=%-4d %s" % (name, compte(tst, name), decls[name]))
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

**Quatre pièges structurels, et les quatre parades sont DANS le script ci-dessus.**
C'est ce qui le distingue de la version qu'a portée ce dossier jusqu'au 15 août 2026.
**Le premier fabrique un faux positif** — une méthode appelée passe pour orpheline.
**Les trois autres fabriquent un faux négatif** — une vraie orpheline n'est même pas
examinée, et c'est celui-là qui a coûté :

- `return foo(x);` ressemble à une déclaration (`return` passe pour un type de
  retour) et masque un vrai appel. Parade : la liste `KW` de mots-clés d'instruction
  (`return`, `case`, `else`, `throw`…) qui disqualifie le préfixe.
- **Une définition en ligne — `T nom() const { return n_; }` — n'était pas capturée
  du tout**, parce que l'ancien script n'enregistrait que les lignes finissant par
  `;`. C'est **la forme exacte des quatre orphelines du 15 août**, et donc le défaut
  qui comptait. Parade : accepter aussi `{` et `}` en fin de ligne.
- Une signature étalée sur plusieurs lignes échappe au même filtre. Parade :
  parenthèse restée ouverte = signature valide.
- **Un commentaire en fin de ligne masque le `;`** : `void set_tab();  // HTS` n'était
  pas capturé — c'est-à-dire, précisément, le défaut n° 8 du tableau ci-dessus, que le
  script était donc incapable de retrouver. Parade : couper la ligne au `//` avant de
  tester sa fin. Elle fait passer le balayage de 423 à **452 noms analysés**.

**Les constructeurs ne sont pas analysés du tout**, et c'est délibéré : le groupe de
capture exige une initiale minuscule (`[a-z_][a-z0-9_]*`), or tous les types du projet
sont capitalisés. Élargir le motif aux majuscules ferait remonter les constructions
locales, parce que `Type x{args};` est syntaxiquement identique à une déclaration.

> **La sortie du script n'est JAMAIS une conclusion.** Un candidat n'est un défaut
> qu'après un `grep -rn "\bnom\b" src/ tests/` **sans troncature**, lu à la main.
> Un audit adversarial lancé le 15 août a déclaré ces quatre-là inexistantes — il
> les avait cherchées *après* leur retrait, et en concluait que le balayage mentait.
> **Ne jamais prendre un rapport d'agent pour argent comptant : vérifier soi-même.**

> 🔴 **Le script publié ici jusqu'au 15 août 2026 ne pouvait pas trouver ce qu'on lui
> attribuait, et c'est démontré.** Il n'enregistrait une déclaration que si la ligne
> finissait par `;` — or les quatre orphelines retirées ce jour-là sont **toutes** des
> définitions en ligne finissant par `}` :
> `bool autowrap() const { return autowrap_; }`, `Pane& other() { return panes_[1 - active_]; }`,
> `LeaderPhase phase() const { return phase_; }`, `const std::string& query() const { return query_; }`.
> Rejoué sur l'arbre d'avant leur retrait (`git archive e32f09c^ src tests`),
> **l'ancien script rend 13 candidats et n'en trouve aucune** — et 291 noms déclarés,
> chiffre qu'il faut aller chercher en lui ajoutant un `print(len(decls))`, car tel
> qu'il était publié il n'imprimait aucun total. Le script ci-dessus rend, sur ce même
> arbre, **452 noms / 21 candidats, et les trouve toutes les quatre** à
> `src=0 tests=0`. Les chiffres « 464 noms, 27 candidats » qu'annonçait ce dossier
> n'étaient reproductibles par aucune des deux versions. La leçon vaut au-delà de ce
> script : **un outil de vérification doit lui-même être vérifié contre un cas dont on
> connaît la réponse.**

**Passage du 15 août 2026, rejoué sur `e6d013d` avec le script ci-dessus :**
**452 noms déclarés, 17 candidats bruts.** Ils se répartissent ainsi, chacun tranché à
la main :

- **`Pty::saw_eof()`** — aucun appelant, nulle part, ni dans `src/` ni dans `tests/`.
  Déjà documenté sur place et au §7.4 : sous Linux un maître dont le dernier esclave
  s'est fermé rend `EIO`, pas 0.
- **Les 16 autres n'ont aucun appelant de production et sont appelées depuis
  `tests/`** — `ambiguous_wide`, `bound_actions`, `charset`, `choices`, `dirty`,
  `line_text`, `question`, `release`, `scroll_bottom`, `scroll_top`, `selection`,
  `set_cloexec`, `state`, `text_row`, `valid`, `wrap_pending`. **Quatorze sont de
  vraies API de test** et devraient toutes porter le suffixe `_for_tests` — aucune ne
  le porte, et c'est le travail qui reste sur ce point : sans le suffixe, chaque
  passage du balayage les fait remonter et coûte le même tri manuel. **Les deux
  dernières ne sont pas des API de test** et ne doivent pas être renommées :
  `set_cloexec()` est gardée exprès (§7.4), et `ambiguous_wide()` est un getter que la
  production court-circuite en lisant directement le global `g_ambiguous_wide`
  (`src/render/width.cpp:58`).

Les **4 vraies orphelines** trouvées ce jour-là — `Screen::autowrap()`,
`Files::other()` (privée, donc même pas atteignable par un test),
`LeaderDispatch::phase()` et `Menu::query()` — ont été retirées au commit `e32f09c`.

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

Il n'y a **pas de plan en cours** : le travail se fait à la demande, un geste à la fois,
en réaction à l'usage réel.

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
   logiciel — ou en le cherchant exprès. Quatorze fois à ce jour.** La liste, le
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
