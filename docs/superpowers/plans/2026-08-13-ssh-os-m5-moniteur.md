# ssh_os 2.0 — Jalon 5 : le moniteur système

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development ou superpowers:executing-plans.

**Goal:** Voir ce que fait la machine sans quitter le bureau. `/proc` lu, agrégé, trié, dessiné — et **rien consommé quand la fenêtre n'est pas visible**.

**Spec de référence :** §9.3 — *« `/proc/stat` par cœur, `/proc/meminfo`, `/proc/loadavg`, liste de processus depuis `/proc/[pid]/stat`, triable par CPU ou mémoire. Rafraîchi sur le tick d'une seconde, et uniquement quand la fenêtre est visible : un moniteur minimisé ne consomme rien. »* Environ 500 lignes.

**Point de départ :** HEAD `deb3400`, **818 cas, 0 en echec**.

## Ce que les quatre premiers jalons ont appris

1. **Tests d'abord, toujours.** Chiffré au jalon 4 : la seule tâche écrite code-d'abord a laissé 14 mutations survivantes sur 33, contre 8/23, 7/23 et 4/26 pour les trois écrites en TDD.
2. **Le plan liste les fichiers neufs, pas ceux qu'il faut brancher.**
3. **Une méthode née sans appelant ne se signale qu'en faisant tourner le vrai logiciel.**

## Contraintes propres à ce jalon

- **Le pourcentage de CPU est un DELTA**, jamais une valeur instantanée : `/proc/stat` donne des compteurs cumulés depuis le démarrage. Le premier échantillon ne peut donc rien afficher, et prétendre le contraire donnerait des chiffres faux au premier dessin.
- **La lecture est PURE, séparée du disque.** Les analyseurs prennent le TEXTE ; un seul endroit lit les fichiers. Sans cela, aucun cas de test ne peut décrire une machine à douze cœurs sous charge.
- **Rien ne tourne quand la fenêtre est cachée.** Le rafraîchissement est déclenché par le dessin, qui n'a lieu que sur une fenêtre visible — la règle est structurelle, pas une discipline.

## Structure des fichiers

| Fichier | Responsabilité |
|---|---|
| `src/apps/monitor/procstat.hpp/.cpp` | Analyseurs PURS de `/proc`, et le calcul des deltas |
| `src/apps/monitor/monitor.hpp/.cpp` | L'application : échantillonnage, tri, rendu |
| `src/app/catalog.cpp` | *(modifié)* l'entrée « Moniteur » |
| `tests/test_procstat.cpp`, `tests/test_monitor.cpp` | |

## Tâche 1 — Les analyseurs de `/proc`

**Fichiers :** `src/apps/monitor/procstat.hpp/.cpp`, `tests/test_procstat.cpp`

`/proc/stat` (total et par cœur), `/proc/meminfo`, `/proc/loadavg`, `/proc/[pid]/stat`. Le pourcentage de CPU entre deux échantillons.

**Les pièges :** le nom d'un processus est entre parenthèses et **peut contenir des espaces et des parenthèses** — découper sur les espaces donne n'importe quoi ; un compteur qui recule (processus disparu, cœur ajouté) doit rendre zéro et non un pourcentage négatif ; `MemAvailable` n'existe pas sur les vieux noyaux.

- [ ] Tâche 1 livrée : tests, mutations, commit

## Tâche 2 — L'application

**Fichiers :** `src/apps/monitor/monitor.hpp/.cpp`, `tests/test_monitor.cpp`, `src/app/catalog.cpp` *(modifié)*

En-tête : charge, mémoire, une barre par cœur. Liste de processus triée par CPU (défaut) ou par mémoire, bascule à la touche. Rafraîchissement au plus une fois par seconde, **depuis le dessin**.

- [ ] Tâche 2 livrée : tests, mutations, commit

## Tâche 3 — Vérification manuelle

- [ ] le moniteur s'ouvre depuis le menu et montre de vrais chiffres
- [ ] les pourcentages bougent sous charge
- [ ] le démon reste à 0 jiffie au repos, moniteur MINIMISÉ
