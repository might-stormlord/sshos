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
- [ ] Tâche 6 — la vue scindée
- [ ] Tâche 7 — le presse-papiers
- [ ] Tâche 8 — les raccourcis
- [ ] Tâche 9 — la sonde
