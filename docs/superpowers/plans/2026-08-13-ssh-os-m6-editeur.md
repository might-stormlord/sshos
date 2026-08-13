# ssh_os 2.0 — Jalon 6 : l'éditeur

**Goal:** La dernière application de la v1. Ouvrir un fichier, le modifier, l'enregistrer — sans jamais perdre ce que l'utilisateur a tapé.

**Spec :** §9.4 — *« Buffer en vecteur de lignes — pas de gap buffer : pour les tailles de fichiers concernées, c'est plus simple et suffisant. Navigation aux flèches, `Ctrl+S` enregistrer, `Ctrl+X` quitter avec confirmation si modifié, recherche simple. **Pas `Ctrl+Q`** : le bureau l'intercepte pour détacher et l'éditeur ne le verrait jamais. **Pas de coloration syntaxique.** »* Environ 800 lignes.

**Point de départ :** HEAD `19917ca`, **858 cas, 0 en echec**.

## Contraintes propres à ce jalon

- **`Ctrl+Q` est pris par le bureau** (§7.4) : l'éditeur ne le verra jamais. C'est `Ctrl+X` qui quitte. Contrepartie assumée d'un geste de détachement sans accord.
- **Rien ne se perd sans question.** Quitter avec des modifications non enregistrées passe par une confirmation, comme la suppression du gestionnaire de fichiers.
- **L'enregistrement est ATOMIQUE** : on écrit à côté puis on renomme. Écrire en place et mourir au milieu laisse un fichier tronqué — et c'est le fichier de l'utilisateur.

## Tâche 1 — Le tampon

**Fichiers :** `src/apps/editor/buffer.hpp/.cpp`, `tests/test_editor_buffer.cpp`

Vecteur de lignes. Insertion et suppression de caractère, retour à la ligne, fusion de lignes, drapeau « modifié ». Chargement depuis du texte, sérialisation vers du texte. Recherche simple.

**Les pièges :** un fichier sans saut de ligne final ne doit pas en gagner un à l'enregistrement ; un fichier vide vaut UNE ligne vide, pas zéro ; les positions doivent rester valides après chaque édition.

- [x] Tâche 1 livrée : tests, mutations, commit — `101a540`, 23 tests, 24 mutations (1 équivalente)

## Tâche 2 — L'application

**Fichiers :** `src/apps/editor/editor.hpp/.cpp`, `src/app/catalog.cpp` *(modifié)*, `tests/test_editor.cpp`

Curseur, défilement, saisie, `Ctrl+S`, `Ctrl+X` avec confirmation, recherche. Rendu avec numéro de ligne et indicateur de modification.

- [x] Tâche 2 livrée : tests, mutations, commit — `324cfd3`, 29 tests, 28 mutations (2 non discriminables)

## Tâche 3 — Vérification manuelle

Sonde bout-en-bout du 13 août 2026 : vrai démon, vraie fenêtre Éditeur
ouverte depuis le menu.

- [x] l'éditeur s'ouvre, la ligne d'état montre `(sans nom)`
- [x] on tape, le texte s'affiche, la **marque `*`** apparaît
- [x] `Ctrl+X` sur un tampon modifié **pose la question** `(o/n)`
- [x] le démon reste à **0 jiffie / 3 s** au repos

- [x] Tâche 3 livrée : sonde, commit

---

## Bilan du jalon

**3 tâches sur 3, 909 cas verts** en Release et sous ASan/UBSan. **52
mutations** jouées, 49 mordues, 3 déclarées non discriminables et
documentées sur place — dont l'atomicité de l'enregistrement, qui ne se
mesure qu'en mourant au milieu de l'écriture.

---

# La v1 est complète

Les **six jalons** sont livrés. Le bureau fait tourner un terminal (avec
`vim`, `htop`, `less` et un `tmux` imbriqué dedans), un gestionnaire de
fichiers, un moniteur système et un éditeur — et il survit à la
déconnexion.

**Ce que la méthode a coûté et rapporté.** Les campagnes de mutation ont
joué plus de 450 mutations sur les jalons 3 à 6 ; l'écrasante majorité des
survivantes étaient des trous de test, pas des équivalences. Et **quatre
défauts n'ont été trouvés que par des sondes bout-en-bout**, jamais par la
suite : `Decoder::failed()`, la garde A2, `InputParser::timeout()` — qui
rendait `vim` inutilisable — et le moniteur qui ne se rafraîchissait pas.

La leçon tient en une phrase : **une méthode née sans appelant, et une
règle qui dépend du temps qui passe, ne se signalent qu'en faisant tourner
le vrai logiciel.**
