# ssh_os 2.0 — Jalon 7 : le gestionnaire de fichiers, façon Dolphin

**Goal :** faire du panneau unique du jalon 4 un vrai gestionnaire de fichiers —
vue scindée, sélection multiple, copier/couper/coller, colonnes triables,
historique de navigation, raccourcis. En mode texte, à la souris comme au
clavier, et **sans jamais bloquer le démon**.

**Point de départ :** HEAD `0f9dece`, **1010 cas verts** en `Release` comme sous
ASan/UBSan.

## La contrainte qui décide de tout

Le démon est **mono-thread**. Copier un fichier de deux gigaoctets avec un
`read`/`write` en boucle gèlerait toutes les fenêtres et tous les clients
pendant la copie. La tâche 7 copie donc **par tranches, dans la boucle
d'événements** : une tranche bornée par réveil, un état d'avancement affiché,
et le bureau reste vivant. C'est la seule façon honnête de le faire ici, et
c'est aussi ce qui rend la copie interruptible.

Corollaire : `read_dir()` reste la seule lecture disque, et elle reste faite
à l'ouverture d'un répertoire, jamais pendant le rendu.

## Les tâches

| # | Ce qu'elle livre | Fichiers |
|---|---|---|
| 1 | **Le modèle s'enrichit** : `mtime`, et un tri paramétrable (nom / taille / date, croissant ou décroissant, dossiers toujours d'abord) | `dir.hpp/.cpp` |
| 2 | **La sélection multiple** : `Espace` bascule, `Ctrl+A` tout, `Ctrl+D` rien, `Maj+flèches` étend, `Ctrl`/`Maj`+clic | `files.hpp/.cpp` |
| 3 | **Les colonnes** : nom, taille, date, avec un en-tête **cliquable** qui trie et inverse | `files.cpp` |
| 4 | **L'historique et le fil d'Ariane** : `Alt+←`/`Alt+→`, et une barre de chemin dont chaque segment se clique | `files.cpp` |
| 5 | **Créer** : `F7` un dossier, `Maj+F7` un fichier vide | `files.cpp` |
| 6 | **La vue scindée** : `F3` ouvre et ferme le second panneau, `Tab` change de panneau ; chacun a son chemin, sa sélection, son historique | `files.hpp/.cpp` |
| 7 | **Le presse-papiers** : `Ctrl+C` / `Ctrl+X` / `Ctrl+V`, copie **par tranches**, destination par défaut = l'autre panneau | `copy.hpp/.cpp`, `files.cpp` |
| 8 | **Les raccourcis** : un liseré cliquable — Racine, Maison, `/tmp` | `files.cpp` |
| 9 | **Sonde bout-en-bout** : vrai démon, vrais fichiers, vraie copie | sonde |

## Le rythme, inchangé depuis le jalon 1

Tests écrits d'abord et rouge constaté, campagne de mutation par tâche, chaque
survivante devient un cas ou une équivalence déclarée sur place, commit par
tâche.

---

- [x] Tâche 1 — le modèle s'enrichit : mtime lu dans la même passe, tri par nom/taille/date, croissant ou décroissant, ordre total sous tous les critères. 9 cas, 7 mutations, toutes mordues.
- [x] Tâche 2 — la sélection multiple : Espace marque et descend, Ctrl+A bascule tout/rien, Maj+flèches étend, Ctrl/Maj+clic, ligne d'état qui compte et pèse, suppression groupée avec une seule question. 15 cas, 15 mutations, toutes mordues.
- [x] Tâche 3 — les colonnes : nom, taille, date ; en-tête cliquable qui trie et inverse, flèche de sens, chiffres calés à droite, colonnes qui cèdent la place avant les noms. 14 cas, 14 mutations, toutes mordues.
- [x] Tâche 4 — l'historique et le fil d'Ariane : Alt+flèches, tout déplacement passe par go_to(), segments cliquables, élision par la gauche. 8 cas, 14 mutations, 12 mordues, 1 invalide, 1 équivalence déclarée.
- [x] Tâche 5 — créer : F7 un dossier, Maj+F7 un fichier vide, le neuf sous le curseur, jamais d'écrasement, ni de nom qui sort du répertoire. 9 cas, 11 mutations, 10 mordues, 1 invalide.
- [x] Tâche 6 — la vue scindée : F3 ouvre et referme, Tab change de panneau, chacun a son chemin, sa sélection, son historique et son tri ; le clic donne la main au panneau visé, en ses propres coordonnées. 13 cas, 12 mutations, 11 mordues, 1 invalide.
- [x] Tâche 7 — le presse-papiers : Ctrl+C/X/V, copie PAR TRANCHES dans la boucle d'événements, arborescence parcourue paresseusement, rename() quand c'est possible, jamais d'écrasement. 21 cas, 19 mutations, toutes mordues.
- [x] Tâche 8 — les raccourcis : F9 ouvre et referme un liseré cliquable (Racine, Maison, Temporaire, Etc) ; il décale les panneaux, il ne les recouvre pas. 8 cas, 9 mutations, toutes mordues.
- [x] Tâche 9 — la sonde

## La sonde du 14 août 2026

Vrai démon, vrai client sous pty, vrais fichiers dans `/tmp/sonde-dolphin`
(un binaire de 6 Mo et un fichier de 7 octets).

- [x] `F9` : le liseré s'ouvre, et cliquer **Temporaire** va dans `/tmp`
- [x] filtrer au clavier puis `Entrée` ouvre le résultat
- [x] `F3` : deux panneaux côte à côte, chacun son chemin, ses colonnes
- [x] `Tab` change de panneau ; chacun descend de son côté
- [x] `Ctrl+clic` marque deux fichiers, la ligne d'état dit « 2 sélectionnés »
- [x] `Ctrl+C` puis `Tab` puis `Ctrl+V` : la copie s'annonce et se fait
- [x] les 6 Mo arrivent, taille pour taille, et l'original reste
- [x] **relancée sur une cible déjà pleine, la copie REFUSE** (`O_EXCL`) et
      le dit : « 2 sur 2 ont echoue »

**Le chiffre qui compte : 39 ms.** C'est le temps de réponse du bureau
mesuré **pendant** la copie des 6 Mo — délai jusqu'au premier octet d'un
repeint complet demandé au clavier. La copie par tranches tient sa
promesse : le démon mono-thread ne se fige pas.

---

## Bilan du jalon

**9 tâches sur 9, 1104 cas verts** en `Release` comme sous ASan/UBSan.
**120 mutations** jouées sur les huit tâches de code, toutes mordues sauf
deux invalides et une équivalence déclarée sur place.

**Deux défauts que seule la sonde a vus**, et aucun test ne les avait :

1. Après un filtre, le curseur restait sur `..` — qui survit toujours au
   filtre. Chercher un dossier puis appuyer sur `Entrée` **remontait d'un
   cran** au lieu de l'ouvrir. C'est le geste le plus naturel du monde, et
   il faisait exactement le contraire de ce qu'on demandait.
2. `F3` s'encode `\033OR` (SS3), pas `\033[13~` : la première sonde
   croyait scinder et ne scindait rien.

**Et un que seule la campagne a vu :** la pile de copie traitait les
fichiers dans l'ordre **inverse** de la sélection, alors que son propre
commentaire prétendait le contraire.
