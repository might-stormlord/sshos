# ssh_os 2.0 — Jalon 4 : le gestionnaire de fichiers

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Une deuxième application native, qui prouve que le contrat applicatif tient pour autre chose qu'un terminal. À la fin de ce jalon, on navigue dans l'arborescence à la souris et au clavier, on renomme, on supprime avec confirmation, et rien de tout cela ne peut peindre hors de sa fenêtre ni bloquer le démon.

**Spec de référence :** `docs/superpowers/specs/2026-08-10-ssh-os-design.md` §9.2 — *« Panneau unique, barre de chemin, liste triée dossiers d'abord. `Entrée` descend ou ouvre dans l'éditeur, `Retour arrière` remonte, `F2` renommer, `Suppr` supprimer avec confirmation, `.` bascule les fichiers cachés, saisie au clavier pour filtrer. Pas de vue en arbre. »* Environ 600 lignes.

**Point de départ :** HEAD `24dc551`, **733 cas, 0 en echec**, en `Release` comme en `Debug`.

## Ce que les trois premiers jalons ont appris, et qui s'applique ici

1. **Le plan liste les fichiers NEUFS, pas ceux qu'il faut brancher.** Quatre tâches du jalon 3 ont débordé de leur périmètre annoncé. Chaque tâche ci-dessous nomme donc explicitement ce qu'elle branche.
2. **Une méthode née sans appelant ne se signale qu'en faisant tourner le vrai logiciel** — `InputParser::timeout()` a rendu `vim` inutilisable pendant tout un jalon. La tâche 5 est une sonde bout-en-bout, et elle n'est pas facultative.
3. **Une mutation survivante est presque toujours un trou de test.** Sur 246 mutations au jalon 3, 2 seulement étaient réellement équivalentes.

## Global Constraints

Identiques aux jalons précédents et non répétées dans chaque tâche : C++20, `-Wall -Wextra -Wpedantic -Werror`, zéro dépendance, `CMakeLists.txt` intouchable, un thread / un `epoll` / aucun mutex, `\033` jamais `\e`, commentaires en français accentué portant le *pourquoi*, messages de commit sans accents.

**Deux contraintes propres à ce jalon :**

- **AUCUN appel bloquant.** Le démon est mono-thread : un `readdir()` sur un montage NFS mort gèlerait toutes les fenêtres et tous les clients. Les entrées sont lues d'un coup à l'ouverture d'un répertoire, jamais pendant le rendu.
- **AUCUNE destruction sans confirmation explicite.** La suppression est le seul geste irréversible du projet ; elle passe par un état de confirmation dans l'application, pas par un raccourci.

## Structure des fichiers

| Fichier | Responsabilité |
|---|---|
| `src/apps/files/dir.hpp/.cpp` | Lire un répertoire, trier, filtrer. Ne connaît ni écran ni clavier |
| `src/apps/files/files.hpp/.cpp` | L'application : état de vue, gestes, rendu |
| `src/app/catalog.cpp` | *(modifié)* l'entrée « Fichiers » |
| `tests/test_files_dir.cpp` | Le modèle |
| `tests/test_files.cpp` | L'application |

---

## Tâche 1 — Le modèle de répertoire

**Fichiers :** `src/apps/files/dir.hpp/.cpp`, `tests/test_files_dir.cpp`

Lire un répertoire en une passe : nom, type (répertoire / fichier / lien / autre), taille. Tri **dossiers d'abord**, puis par nom, insensible à la casse mais **stable** — deux noms qui ne diffèrent que par la casse doivent garder un ordre déterministe, sinon la liste saute d'un rafraîchissement à l'autre.

`..` est toujours présent en tête, **sauf à la racine**. Les fichiers cachés sont exclus par défaut.

Le filtre est une sous-chaîne, insensible à la casse. Il ne relit pas le disque : filtrer est une opération sur ce qui est déjà en mémoire.

**Les pièges :** un répertoire illisible (`EACCES`) doit rendre une liste vide et un message, pas planter ni boucler ; un nom qui n'est pas de l'UTF-8 valide doit s'afficher sans corrompre la grille ; `readdir` ne trie rien et ne garantit aucun ordre.

**Tests :** un répertoire fabriqué de toutes pièces se lit trié dossiers d'abord ; `..` est là, et absent à la racine ; les cachés apparaissent et disparaissent ; le filtre est insensible à la casse et ne relit pas ; un répertoire sans droit de lecture rend une erreur nommée.

- [x] Tâche 1 livrée : tests, mutations, commit — `16e1423`, 21 tests, 23 mutations
  (2 équivalentes)

La comparaison est devenue un **ordre total** en cours de route : le pliage
de casse rendait deux orthographes d'un même nom égales, ce qui laissait
leur ordre à la merci de l'algorithme de tri. Le départage par le nom brut
rend du même coup la stabilité du tri sans objet.

## Tâche 2 — L'application : navigation et sélection

**Fichiers :** `src/apps/files/files.hpp/.cpp`, `tests/test_files.cpp`, `src/app/catalog.cpp` *(modifié)*

L'état de vue : sélection, décalage de défilement, filtre en cours. `Entrée` descend dans un répertoire, `Retour arrière` remonte, les flèches bougent, `.` bascule les cachés, la saisie ordinaire filtre.

**Les pièges :** la sélection doit rester dans la liste quand le filtre la rétrécit sous elle ; le défilement doit suivre la sélection sans jamais la laisser sortir de la fenêtre ; remonter depuis `/` ne doit rien faire ; descendre dans un répertoire illisible doit laisser l'application où elle est, avec son message.

**Tests :** descendre puis remonter revient au même endroit avec la même sélection ; le filtre qui vide la liste ne laisse pas une sélection hors bornes ; le défilement suit la sélection aux deux bouts ; la racine n'a pas de parent.

- [x] Tâche 2 livrée : tests, mutations, commit — `0bf14a3`, 32 tests, 33 mutations
  (2 équivalentes)

**Le code a précédé les tests pour cette tâche, et la campagne l'a dit :**
14 survivantes sur 33 au premier tour, contre 8 sur 23 à la tâche 1 et
10 sur 41 à la tâche 13 du jalon 3, toutes deux écrites en TDD. À ne pas
refaire aux tâches 3 et 4.

## Tâche 3 — Renommer et supprimer

**Fichiers :** `src/apps/files/files.cpp` *(étendu)*, `tests/test_files.cpp` *(étendu)*

`F2` ouvre une saisie de renommage pré-remplie du nom courant ; `Entrée` valide, `Échap` annule. `Suppr` demande **confirmation** — un état de l'application, pas un raccourci de plus — et seule une réponse explicite détruit.

**Les pièges :** renommer vers un nom existant doit échouer avec un message, pas écraser ; renommer vers une chaîne vide ne fait rien ; supprimer un répertoire non vide échoue avec un message (pas de suppression récursive en v1) ; `..` ne se renomme ni ne se supprime.

**Tests :** un fichier renommé porte son nouveau nom sur le disque ; le nom déjà pris est refusé et l'ancien survit ; la confirmation refusée laisse le fichier ; acceptée, le fichier n'est plus là ; `..` refuse les deux gestes.

- [x] Tâche 3 livrée : tests, mutations, commit — `151dda7`, 19 tests, 26 mutations
  (1 équivalente)

## Tâche 4 — Le rendu

**Fichiers :** `src/apps/files/files.cpp` *(étendu)*, `tests/test_files.cpp` *(étendu)*, goldens

Barre de chemin en haut, liste au milieu, ligne d'état en bas (message d'erreur, filtre en cours, ou invite de confirmation). La sélection est en inverse vidéo ; les répertoires sont d'une couleur, les liens d'une autre.

**Les pièges :** un chemin plus long que la fenêtre doit être **élidé par la gauche** — la fin d'un chemin porte l'information, pas son début ; un nom plus long que la largeur doit être élidé sans casser une pleine chasse en deux ; la ligne d'état ne doit jamais faire disparaître une ligne de la liste sans que la sélection suive.

**Tests :** un chemin trop long garde sa fin ; un nom pleine chasse élidé ne laisse pas de demi-caractère ; la sélection est bien celle qui porte l'inverse vidéo ; goldens d'un écran de gestionnaire.

- [x] Tâche 4 livrée : tests, mutations, commit — `5ba2671`, 13 tests, 23 mutations
  (2 équivalentes)

**Faite AVANT la tâche 3**, délibérément : sans rendu, l'application était au
catalogue et ne peignait rien. Un point d'arrêt doit toujours laisser quelque
chose d'utilisable.

## Tâche 5 — Vérification manuelle

Sonde bout-en-bout du 13 août 2026 : vrai démon, vrai client sous pty,
vraie fenêtre Fichiers ouverte depuis le menu.

- [x] Fichiers s'ouvre depuis le menu, le titre `Fichiers` arrive au cadre
- [x] le contenu de `/` s'affiche, **trié dossiers d'abord**
- [x] on filtre au clavier (`tmp`), la liste se réduit
- [x] `Entrée` descend : la barre de chemin passe à `/tmp`, `..` est en tête
- [x] le démon reste à **0 jiffie / 2 s** au repos, fenêtre ouverte

Le renommage et la suppression n'ont **pas** été exercés par la sonde : ils
le sont par 19 cas sur de vrais fichiers, et les faire détruire dans une
sonde qui pilote un vrai bureau ferait courir un risque sans rien prouver
de plus que ce que la tâche 3 prouve déjà.

- [x] Tâche 5 livrée : sonde, commit

---

## Bilan du jalon

**5 tâches sur 5, 818 cas verts** en Release et sous ASan/UBSan, 0
avertissement. **105 mutations** jouées, 98 mordues, 7 déclarées
équivalentes et documentées sur place.

**La leçon du jalon, et elle est chiffrée.** La tâche 2 a été écrite code
d'abord, tests ensuite : **14 survivantes sur 33** au premier tour. Les
trois autres tâches, écrites en TDD, ont donné 8/23, 7/23 et 4/26. Des
tests écrits après le code vérifient ce qu'on se souvient d'avoir écrit,
pas ce qu'on aurait cherché à casser.
