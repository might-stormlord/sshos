# Round correctif — constat C : EPOLLHUP honoré avant le drainage d'EPOLLIN

Base : branche `m1-noyau`, HEAD `af36d6d`. Référence : **186 cas, 0 échec**, Debug (ASan+UBSan) et Release.

## Le défaut

`src/daemon/daemon.cpp`, branche du client attaché :

```cpp
if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
  drop_client(nullptr);
  continue;              // <-- jette le lot sans jamais lire
}
...
if ((events & EPOLLIN) != 0) { const bool closed = drain_socket(*client); ... }
```

`epoll_wait()` peut parfaitement rendre `EPOLLIN | EPOLLHUP` **dans le même
événement** quand le pair a écrit puis fermé : les données sont encore dans le
tampon de réception du socket, et le HUP ne signale que la fin d'écriture du
pair. Le `continue` détruit alors la connexion sans jamais consommer ces
octets.

Conséquence concrète : la session du démon **survit** au détachement du client
(c'est tout l'intérêt du démon), mais les derniers messages envoyés juste avant
la fermeture — frappes clavier, dernier Resize — sont perdus. L'utilisateur
retrouve à la réattache un état amputé de ses dernières actions.

**Le même motif existe sur la connexion `pending`** (vers la ligne 399). Les
deux emplacements sont dans le périmètre.

## Ce qu'il faut faire

Inverser l'ordre : **drainer d'abord `EPOLLIN` et traiter les messages
décodés, puis seulement honorer `EPOLLHUP`/`EPOLLERR`**.

Points de vigilance, à trancher et à documenter en commentaire :

- `drain_socket()` détecte déjà la fermeture (`read()` rendant 0). Après
  réordonnancement, la fermeture peut donc être signalée par deux voies : le
  retour de `drain_socket()` et le bit HUP. Il ne doit en résulter qu'une
  seule fermeture, et surtout pas un `drop_*` appelé deux fois.
- Le test de `Decoder::failed()` ajouté au round précédent doit rester
  **après** le drainage et rester atteignable. Ne pas régresser sur ce point.
- `EPOLLERR` n'est pas `EPOLLHUP` : sur erreur franche, lire peut échouer
  immédiatement. Le comportement doit rester correct, pas seulement le cas
  HUP propre.
- Ne pas réintroduire de boucle active : un HUP non consommé qui reste
  signalé à chaque tour fait tourner le démon à 100 % de CPU. C'est
  exactement ce que le commentaire existant met en garde. Vérifie
  explicitement, après ta correction, que le démon ne consomme pas de CPU en
  boucle quand un pair se ferme (mesure-le, ne le suppose pas).

## Test discriminant exigé

Un test qui **échoue contre le code actuel** et passe après correction. Le
chemin d'observation suggéré, parce qu'il est visible de bout en bout :

1. un client s'attache normalement (Hello → Welcome → première trame) ;
2. il envoie des événements d'entrée qui font monter le compteur de clics de
   la session, **puis ferme son socket immédiatement**, sans attendre — c'est
   ce qui provoque la coalescence `EPOLLIN | EPOLLHUP` ;
3. un second client s'attache et lit l'état de la session : le compteur doit
   inclure les événements de l'étape 2.

Pour que la coalescence se produise de façon fiable, assure-toi que le démon
est bien bloqué dans `epoll_wait()` avant l'écriture+fermeture. Si tu ne
parviens pas à la rendre déterministe, dis-le explicitement plutôt que de
livrer un test qui passe par hasard : un test non discriminant est pire
qu'aucun test.

Donne dans ton rapport la **sortie exacte** de l'échec contre le code d'avant
(nom du cas, fichier:ligne). Vérifie la restauration du code de production par
`sha256sum` et donne les hachages.

## Contraintes

- Ne modifie pas `CMakeLists.txt`.
- Commentaires en **français avec accents** ; code et identifiants en anglais.
- `\033`, jamais `\e`.
- Le harnais : `CHECK`/`CHECK_EQ` enregistrent l'échec et **continuent** ;
  seul `REQUIRE` retourne. Écris tes nettoyages en conséquence — un `REQUIRE`
  qui retourne ne doit pas laisser un processus ou un fd derrière lui.
- **Aucune commande détachée** (`nohup`, `&`, `disown`) : tout en avant-plan,
  sinon ton exécution se bloque.
- 20 exécutions consécutives en Debug et 20 en Release, toutes vertes.
- **Crée le commit toi-même** à la fin, sur ta branche de worktree, une fois
  tout vert. Message en français, préfixe `fix(daemon):`.
