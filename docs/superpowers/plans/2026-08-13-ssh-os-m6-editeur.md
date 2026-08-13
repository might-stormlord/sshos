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

- [ ] Tâche 1 livrée : tests, mutations, commit

## Tâche 2 — L'application

**Fichiers :** `src/apps/editor/editor.hpp/.cpp`, `src/app/catalog.cpp` *(modifié)*, `tests/test_editor.cpp`

Curseur, défilement, saisie, `Ctrl+S`, `Ctrl+X` avec confirmation, recherche. Rendu avec numéro de ligne et indicateur de modification.

- [ ] Tâche 2 livrée : tests, mutations, commit

## Tâche 3 — Vérification manuelle

- [ ] on ouvre l'éditeur, on tape, on enregistre, le disque le confirme
- [ ] on quitte avec des modifications : la question est posée
- [ ] le démon reste à 0 jiffie au repos
