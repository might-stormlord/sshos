# ssh_os 2.0 — Document de conception

**Date :** 10 août 2026
**Statut :** conception validée, prêt pour la planification d'implémentation
**Langage :** C++20, zéro dépendance externe

---

## 1. Ce qu'on construit

Un environnement de bureau rendu en terminal, accessible par SSH, dans lequel les fenêtres et les processus qu'elles contiennent **survivent à la déconnexion**.

On se connecte en SSH, on tape `sshos`, on obtient un bureau : une barre des tâches avec une horloge, un menu d'applications, des fenêtres qu'on déplace et redimensionne à la souris, avec des boutons fermer / minimiser / plein écran. On lance une compilation dans un terminal, on ferme le portable, on se reconnecte le lendemain depuis une autre machine : la compilation est terminée, la fenêtre est là, le shell a gardé son historique.

C'est un projet neuf. Aucun code, aucun choix et aucune contrainte ne sont repris de la version 1 (`/home/storm/dev/ssh_os`, un gestionnaire de fenêtres pavant écrit en Rust). Le modèle d'interaction est différent — fenêtres flottantes à la souris plutôt que pavage au clavier — et le modèle de persistance n'existait pas.

### Fonctionnalités demandées

- Menu d'applications
- Barre des tâches avec horloge et applications épinglables
- Barre ancrable **en bas ou à gauche**, les deux disponibles en option
- Fenêtres déplaçables et redimensionnables
- Boutons fermer, minimiser vers la barre, plein écran
- Persistance complète des fenêtres et des sessions à travers les déconnexions SSH

### Hors périmètre, assumé

Instantané de session sur disque · bureaux virtuels · reflow du terminal au redimensionnement · coloration syntaxique dans l'éditeur · plusieurs clients en miroir · transparence, ombres, animations · attache distante par TCP (le socket est local, SSH est le transport).

---

## 2. Décisions structurantes

Ces décisions sont prises. Elles ne sont pas à re-débattre pendant l'implémentation ; elles sont à appliquer ou, si un fait nouveau les invalide, à révoquer explicitement en modifiant ce document.

| Sujet | Décision | Raison |
|---|---|---|
| Langage | **C++20** | Le programme jongle en permanence avec des fd de PTY, des sockets, le mode brut du terminal. RAII garantit qu'aucun chemin de sortie ne laisse un terminal cassé ou un fd fuité. Sur 12 000 à 15 000 lignes, `string`/`vector`/`unique_ptr` suppriment une classe entière de bugs que le C imposerait de gérer à la main. |
| Dépendances | **Aucune**, binaire statique | Rien n'est installé sur la machine cible — ni ncurses, ni libssh, ni SQLite. Et le rendu est justement le cœur du projet : on veut le contrôler, pas le déléguer. |
| Binaire | **Un seul**, modes `sshos` / `--daemon` / `--kill` / `--status` | Pas deux exécutables à maintenir synchronisés. |
| Persistance | **Démon détaché** uniquement, pas d'instantané disque | C'est le modèle tmux : les processus continuent réellement de tourner. Un instantané ne préserverait que la disposition, pas le `make` en cours — donc pas ce qui a été demandé. Coût accepté : un plantage du démon ou un redémarrage machine perd la session. |
| Transport | **Socket UNIX abstrait**, `\0sshos/<uid>/<boot_id>` | Voir §3.3 : aucun fichier, donc rien à nettoyer, rien à voir disparaître sous le démon, et le `bind()` sert lui-même de mutex. |
| Protocole | Démon → client : **diff de cellules déjà encodé en ANSI**. Client → démon : octets d'entrée bruts + taille. | Le minimum d'octets sur un lien lent, et un client trivial. |
| Concurrence | **Un thread, un `epoll`**, aucun mutex | L'état est reproductible, donc testable sans harnais de synchronisation. Le parallélisme vient du noyau, pas du programme. |
| Rendu | Piloté par un drapeau **dirty**, jamais par une horloge. Plafond à **33 ms** (30 fps). | Bureau au repos = zéro octet réseau, zéro CPU. |
| Entrée | **Souris SGR 1002+1006 + clavier complet** | Les deux, pas l'un ou l'autre. |
| Raccourcis | **Touche leader mono-octet** (`Ctrl+A` par défaut) | Voir §7.4 : `Alt+Tab` et `Alt+Flèches` n'arrivent pas jusqu'au programme sur une bonne partie des configurations clientes. |
| Bureau | **Panneau ancré**, `edge = bottom \| left \| top \| right` | Bas et gauche demandés ; haut et droite tombent gratuitement du même mécanisme. |
| Multi-client | **Le nouveau détache l'ancien** (`tmux attach -d`) | Un PTY n'a qu'une seule taille ; le miroir imposerait le plus petit dénominateur commun à tout le monde. |
| Applications v1 | Terminal, Fichiers, Moniteur, Éditeur | L'éditeur en dernier, sans coloration syntaxique. |

---

## 3. Architecture

### 3.1 Deux modes, un binaire

```
sshos              client — s'attache au démon, le démarre s'il n'existe pas
sshos --daemon     l'OS lui-même (lancé automatiquement, rarement à la main)
sshos --kill       arrête le démon et tout ce qu'il contient
sshos --status     démon vivant ? depuis quand ? combien de fenêtres ?
```

Le protocole entre les deux est interne et non versionné, mais le handshake compare les identifiants de build et signale clairement un décalage plutôt que de produire un affichage corrompu.

### 3.2 Topologie

```
   session SSH                         détaché de toute session
┌────────────────┐                ┌──────────────────────────────────┐
│  sshos         │   socket UNIX  │  sshosd                          │
│                │◄──────────────►│                                  │
│  mode brut     │  diff ANSI ──► │  epoll ─┬─ socket d'écoute       │
│  écran alterné │  ◄── entrée    │         ├─ socket client         │
│  souris 1002   │      + taille  │         ├─ timerfd (1 s)         │
│                │                │         ├─ signalfd (CHLD/TERM)  │
│  ~250 lignes   │                │         └─ N maîtres de PTY      │
└────────────────┘                │              │                   │
                                  │   Session : fenêtres, z-order,   │
                                  │   panneau, menu, focus, apps     │
                                  └──────────────────────────────────┘
                                        bash · make · vim · …
```

Le démon détient tout. Le client n'a aucun état propre au-delà de son propre terminal. Débrancher un écran ne tue pas une machine.

### 3.3 Le socket et l'unicité du démon

**Adresse abstraite du domaine UNIX** — un `sun_path` commençant par un octet nul, hors du système de fichiers :

```
\0sshos/<uid>/<boot_id>
```

Un socket *fichier* aurait posé trois problèmes, dont deux sans solution propre :

1. `$XDG_RUNTIME_DIR` (`/run/user/<uid>`) est supprimé par `pam_systemd` à la dernière déconnexion de l'utilisateur — c'est-à-dire exactement au moment où le démon doit rester joignable. Socket et verrou disparaissent sous un démon bien vivant, et deux `flock LOCK_EX` posés sur deux inodes différents ne s'excluent plus : un démon orphelin par jour, avec ses PTY et sa RAM devenus inatteignables. La variable est de surcroît parfois vide, auquel cas le chemin devient `/sshos/<uid>.sock` et le `mkdir` échoue en `EACCES`.
2. Après un `SIGKILL` du démon, le fichier survit et il faut une séquence de détection d'obsolescence — `connect()` → `ECONNREFUSED` → `unlink` → retenter, le tout sous verrou pour éviter que deux clients délient le socket l'un de l'autre.
3. `~/.local/state/` règle le point 1 mais pas le point 2, et ajoute une dépendance à un `$HOME` local.

L'adresse abstraite supprime les trois : elle n'existe que tant qu'un processus la détient, elle disparaît d'elle-même à la mort du démon quel qu'en soit le mode, et **le `bind()` est lui-même le mutex** — deux clients qui démarrent en même temps, l'un obtient l'adresse, l'autre reçoit `EADDRINUSE` et se contente de se connecter. Aucun fichier de verrou, aucun code de détection d'obsolescence.

Le `boot_id` (`/proc/sys/kernel/random/boot_id`) est inclus dans le nom : un démon ne peut de toute façon pas survivre à un redémarrage, autant que le nom le dise.

Deux contreparties, toutes deux obligatoires :

- **Les permissions du système de fichiers n'existent pas sur une adresse abstraite.** Tout processus de la machine peut s'y connecter. Chaque `accept()` interroge donc `SO_PEERCRED` et rejette tout pair dont l'uid n'est pas le nôtre.
- **Un fd fuité dans un enfant garde l'adresse occupée** sans que personne ne fasse `accept()`. D'où `SOCK_CLOEXEC` sur le socket d'écoute, et `CLOEXEC` sur tous les descripteurs du démon sans exception (§9.1).

Le journal, lui, reste un fichier : `~/.local/state/sshos/daemon.log`.

### 3.4 La boucle d'événements

Un seul thread, un seul `epoll`, pas de mutex.

```
boucle :
    epoll_wait(timeout)
    traiter TOUS les fds prêts        ← draine avant de dessiner
    si (dirty et ≥33 ms depuis la dernière frame) :
        composer  →  diffuser  →  écrire sur le socket
```

Le rendu est piloté par le drapeau *dirty*, jamais par une horloge. Le tick d'une seconde ne salit l'écran que si la minute affichée a changé. Un bureau au repos ne consomme ni CPU ni bande passante.

Le drainage complet avant composition est ce qui absorbe les rafales : un `cat` d'un gros fichier peut produire des milliers de lignes, elles sont toutes intégrées à l'état de l'écran, et **une seule** frame en sort. Le programme invité, lui, tourne à pleine vitesse dans son PTY — on ne le ralentit jamais, on se contente de ne pas tout afficher.

### 3.5 Arborescence

```
src/
  main.cpp              aiguillage client / démon
  common/               Fd RAII, socket, signalfd, timerfd, log, codec du protocole
  client/               boucle d'attache, mode brut, SIGWINCH, restauration du tty
  daemon/               boucle epoll, gestion du client, Session (le bureau)
  render/               Cell, Surface, View, diff→ANSI, thème, profils de sortie
  wm/                   Window, z-order, focus, hit-test, drag/resize, décorations
  shell/                panneau (ancrage, layout H/V), menu, horloge
  input/                parseur d'octets → Key/Mouse, table de raccourcis
  app/                  interfaces App et Host, catalogue d'applications
  apps/terminal/        pty, parseur VT, écran, app          ← le gros morceau
  apps/files/
  apps/monitor/
  apps/editor/
  config/               parseur INI maison
tests/                  harnais d'assertions maison + corpus de frames golden
```

**Règle de dépendance, à sens unique et sans exception :**

```
render  →  (rien)
wm      →  render
shell   →  wm, render
daemon  →  tout
apps/*  →  render, app/          ← une application ne peut pas atteindre le WM
```

Conséquence directe : ajouter une cinquième application ne modifie aucun fichier existant, sauf une ligne dans le catalogue.

---

## 4. Le moteur de rendu

### 4.1 La cellule

```cpp
struct Cell {
  char32_t ch;        // codepoint de base
  uint32_t cluster;   // 0, ou index vers un cluster (combinants, emoji ZWJ)
  Color    fg, bg;    // défaut | indexée 0-255 | RGB
  uint16_t attrs;     // gras, faible, italique, souligné, inversé, barré
  uint8_t  width;     // 1 normal · 2 pleine chasse · 0 continuation
};
```

`ch` porte un scalaire Unicode quand le graphème tient en un seul code point — le cas de l'écrasante majorité des cellules — et bascule sur `cluster`, un index dans un réservoir par surface, dès qu'il faut représenter une marque combinante, un sélecteur de variation ou une séquence emoji ZWJ. Le réservoir ne coûte donc que sur ce qui le nécessite réellement.

`Color` est un type somme explicite — `Défaut | Indexée(0-255) | RGB(r,g,b)` — et non un entier. La distinction compte : `SGR 39/49` (couleur par défaut) n'est pas la couleur indexée 7, et une valeur non typée écraserait le truecolor d'un `vim` avant même que le diffeur ne le voie.

#### La largeur, et pourquoi c'est le champ le plus dangereux

Un idéogramme ou un emoji occupe deux colonnes : cellule N avec `width=2`, cellule N+1 en continuation vide `width=0`.

Dans un protocole qui n'envoie que des diffs, **un désaccord de largeur ne se répare jamais tout seul.** Si le démon croit qu'un glyphe fait deux colonnes et que le terminal client en dessine une, tout ce qui est à droite se décale — et le démon, convaincu que ces cellules n'ont pas changé, ne les réécrira plus jamais. Quatre règles, non négociables :

1. Le diffeur ne commence **jamais** un segment de réécriture sur une cellule de continuation : il élargit d'une colonne vers la gauche jusqu'à la cellule de tête.
2. Un glyphe large n'est **jamais** placé en dernière colonne. Il est repoussé à la ligne suivante, la dernière colonne restant blanche — comportement des émulateurs sérieux, et seule façon d'éviter le retour à la ligne parasite décrit en §4.3.
3. Tout graphème non-ASCII **termine le run courant** et la reprise se fait par un `CUP` absolu. On ne se fie jamais à la position implicite du curseur après un caractère dont la largeur peut être contestée.
4. Un raccourci de **repaint complet forcé** (`<leader>r`) existe comme échappatoire humaine. Si malgré tout l'écran se désaligne, l'utilisateur a un geste pour le remettre d'aplomb sans se déconnecter.

**Les largeurs viennent d'une table Unicode embarquée, pas de `wcwidth()`.** Deux raisons : le projet a fait vœu de zéro dépendance, et surtout `wcwidth()` dépend de la locale du **démon**, qui n'a aucune raison d'être celle du client — le démon tourne détaché, avec l'environnement fossilisé de la première session SSH.

Reste le cas *East Asian Ambiguous* (le `±`, les caractères de dessin de boîte…), que certains terminaux dessinent large et d'autres étroit. On le **sonde à l'attache** : émission d'un caractère test suivi de `\e[6n`, la colonne rapportée tranche. Surchargeable en configuration, étroit par défaut.

Une grille 200×50 pèse environ 200 Ko ; on en garde deux, courante et précédente.

Le scrollback ne peut pas utiliser ce format — 10 000 lignes × 200 colonnes × 20 octets font 40 Mo par terminal. Les lignes d'historique sont stockées en longueur variable, rognées de leurs blancs de fin.

### 4.2 Surface et View

Le démon possède une `Surface` : la grille complète. Aucune application ne la reçoit jamais. Une application reçoit une `View` — une fenêtre rectangulaire, translatée et découpée, sur cette surface.

```cpp
class View {
public:
  int w() const, h() const;                       // 0,0 = coin de MA zone cliente
  void put(int x, int y, char32_t, Style);
  void text(int x, int y, std::string_view utf8, Style);
  void fill(Rect, Style);
  void box(Rect, Border, Style);
  View sub(Rect) const;                            // découpage imbriqué
};
```

Écrire hors du clip n'est pas une erreur : c'est ignoré. Un bug d'arithmétique dans le gestionnaire de fichiers ne peut donc pas peindre par-dessus la barre des tâches ni par-dessus la fenêtre voisine. La robustesse vient de la structure, pas de la discipline.

### 4.3 Le diffeur

À chaque frame, comparaison de la grille courante à la précédente :

- Ligne identique → sautée, zéro octet.
- Segment modifié → un `CUP` pour s'y placer, **sauf** si l'émission précédente s'est terminée exactement à cette colonne sur cette ligne.
- Le style SGR est suivi comme un **état courant sur toute la frame**, pas par segment : une séquence de couleur n'est émise que quand elle change réellement. Une ligne uniforme coûte un seul SGR.
- Chaque frame débute par une réinitialisation explicite : on n'hérite jamais d'un état supposé.
- Les fins de ligne à fond par défaut se font en `CSI K` plutôt qu'en espaces.

#### L'enveloppe de frame

Chaque frame est encadrée, et l'encadrement fait partie du contrat :

```
CSI ?25l          masquer le curseur           ← sinon il court à l'écran
CSI 0m            réinitialiser le style       ← on n'hérite jamais d'un état supposé
  … corps du diff …
CUP absolu        position finale du curseur
CSI ?25h          seulement si wants_cursor()
```

Le masquage n'est pas cosmétique : sans lui, sur un lien à 150 ms de latence, le curseur matériel traverse visiblement l'écran à chaque frame en suivant les sauts du diffeur.

**Le retour à la ligne automatique est désactivé pour toute la session** : `CSI ?7l` à l'attache, `CSI ?7h` à la restauration. Sans cela, un graphème large écrit en dernière colonne provoque un retour à la ligne — et sur la dernière ligne, un **défilement de tout l'écran**, ce qui désynchronise définitivement le modèle de frame précédente. La règle 2 du §4.1 interdit déjà le cas ; `?7l` est la ceinture par-dessus les bretelles.

**Pas d'optimisation par défilement matériel en v1.** Les régions de défilement portent sur des lignes entières de l'écran ; dans un compositeur, une fenêtre qui défile n'occupe qu'une tranche de colonnes, souvent partiellement recouverte. Les marges gauche et droite, qui rendraient l'optimisation applicable, exigent `DECLRMM` + `DECSLRM` — absents des terminaux VTE — et `CSI Pl;Pr s` est **indiscernable de `SCOSC`** quand `DECLRMM` est inactif : une sonde erronée corromprait silencieusement le curseur sauvegardé de l'invité. Optimisation possible plus tard, pas dette.

### 4.4 Profils de sortie

Le client transmet `TERM`, `COLORTERM` et son support UTF-8 à l'attache.

| Signal | Sortie couleur |
|---|---|
| `COLORTERM=truecolor` ou `24bit` | `\e[38;2;r;g;bm` |
| `TERM` contient `256color` | quantification vers la palette 256 |
| sinon | 16 couleurs |

Le thème est écrit une seule fois en RGB et converti à l'attache. Même principe pour les caractères : si la locale annoncée n'est pas UTF-8, un profil de bordures ASCII (`+`, `-`, `|`) remplace les semi-graphiques.

---

## 5. Le gestionnaire de fenêtres

### 5.1 La fenêtre

```cpp
enum class Mode { Normal, Maximized, Fullscreen };

struct Window {
  WindowId id;
  Rect  user_rect;         // géométrie voulue par l'utilisateur — AUTORITAIRE
  Size  user_ref;          // taille de la zone de travail où user_rect a été posé
  Rect  display_rect;      // dérivé, recalculé à chaque changement de zone de travail
  Mode  mode;
  bool  minimized;         // orthogonal au mode, pas une quatrième valeur d'enum
  std::string title;       // alimenté par l'application (OSC 2 pour un shell)
  std::unique_ptr<App> app;
};
```

`minimized` est un booléen séparé et non une valeur de `Mode` : une fenêtre peut être maximisée **et** minimisée, et la restaurer doit la ramener maximisée. Deux booléens indépendants « maximisé » / « plein écran » se contrediraient ; un enum à trois valeurs, non.

Le z-order est l'ordre d'un `std::vector<Window*>` : début = arrière-plan, fin = premier plan. Passer au premier plan, c'est déplacer en fin. Avec un plafond de 64 fenêtres, le parcours linéaire est gratuit et le code reste lisible.

### 5.2 Décorations

```
╭─ Terminal — bash ─────────────────────[_][□][×]╮
│ user@box:~$ make -j8                        │
│ [ 42%] Building CXX object src/wm.cpp.o        │
│ [ 47%] Building CXX object src/render.cpp.o    │
╰───────────────────────────────────────────────◢╯
```

La barre de titre remplace la bordure haute — aucune ligne gaspillée. Zone cliente = cadre moins une colonne à gauche et à droite, une ligne en haut, une en bas. Les trois boutons occupent trois cellules chacun, calés à droite. Le `◢` en bas à droite est la poignée de redimensionnement.

Fenêtre focalisée : barre de titre en couleur d'accent, bordure vive. Hors focus : tout est atténué. Aucune ambiguïté sur qui reçoit le clavier.

Chaque application déclare une taille minimale via `min_size()` ; le cadre ne descend jamais en dessous. Plancher global : **16×5**.

#### Le mode plein écran, et pourquoi il n'est pas un luxe

Sur un client en 80×24 avec un panneau d'une ligne, une fenêtre *maximisée* mais décorée offre une zone cliente de **78×21**. L'application phare — le Terminal — ne peut donc jamais héberger le 80×24 canonique que présuppose la moitié des programmes TUI. Deux fenêtres côte à côte donnent 38 colonnes chacune, ce qui n'est utilisable pour rien.

`Fullscreen` est donc un troisième mode, distinct de `Maximized` : barre de titre et bordures masquées, panneau escamoté, l'application occupe l'écran entier. Sur les petits terminaux, c'est le mode dans lequel les utilisateurs vivront réellement. Bascule au clavier (`<leader>f`), retour par le même geste, `user_rect` conservé intact pendant toute la durée.

### 5.3 Hit-testing

Du dessus vers le dessous, premier touché gagne : **menu ouvert → panneau → fenêtres, du premier plan vers l'arrière**.

Dans une fenêtre, dans l'ordre : les trois boutons ; le reste de la barre de titre (déplacer) ; le coin bas-droit (redimensionner en diagonale) ; les bordures droite et basse (redimensionner sur un axe) ; la zone cliente (transmis à l'application en coordonnées locales).

### 5.4 Déplacement et redimensionnement

Machine à états à trois positions : `Idle`, `Moving{offset de préhension}`, `Resizing{ancre}`.

**Le déplacement est live**, pas un contour fantôme. Avec le diff de cellules, bouger une fenêtre n'envoie que le bord qui se découvre et celui qui se recouvre ; le plafond à 30 fps absorbe les rafales de mouvement sur lien lent. Rien n'est notifié à l'application : sa taille ne change pas.

**Le redimensionnement, lui, est un contour élastique.** L'asymétrie n'est pas une incohérence, c'est la conséquence directe de ce que coûte un changement de taille. Sous le tracking 1002, un rapport de mouvement arrive à **chaque franchissement de cellule** : tirer une poignée sur quarante colonnes produirait quarante `TIOCSWINSZ`, donc quarante `SIGWINCH`, donc quarante redessins complets de `htop` — que ni le plafond à 30 fps ni la coalescence de la sortie PTY ne peuvent absorber, puisque l'invité régénère réellement son écran à chaque fois. Pendant le glissement, seul le contour bouge. `on_resize()` et `TIOCSWINSZ` sont appelés **une seule fois, au relâchement**.

#### Annulation d'un glissement

Le relâchement du bouton peut légitimement ne jamais arriver : pointeur sorti de la fenêtre du terminal, `F12` pressé au milieu du geste, client remplacé par un autre. Sept chemins ramènent donc à `Idle` : `Échap`, n'importe quelle frappe, la perte de focus du terminal (`\e[O`), la désactivation de la souris, un détachement, une attache, un rapport de mouvement où le bouton a disparu, et un chien de garde d'environ deux secondes sans événement.

**Tous les sept valident la géométrie courante** — ils ne reviennent pas à la géométrie initiale. Une politique unique dans les sept chemins ; c'est la divergence entre eux qui produirait des bugs impossibles à reproduire.

### 5.5 Redimensionnement du bureau

Le cas nominal : attaché en 160×50 depuis un portable, rattaché en 80×24 depuis un téléphone, puis de retour en 160×50.

Contraindre les fenêtres dans les nouvelles bornes **en écrasant leur géométrie** est destructif et convergent : quatre fenêtres disposées en 160×50 se retrouvent toutes à `x=0, w=80` en 80×24 — empilées, indiscernables — et le retour en 160×50 ne les sépare plus. L'arrangement est perdu pour de bon. Second effet : une fenêtre dont le coin bas-droit est passé hors écran perd sa **seule** poignée de redimensionnement souris, définitivement.

D'où le dédoublement de la géométrie :

- **`user_rect` est autoritaire et n'est jamais réécrit** par un changement de taille du bureau. Seul l'utilisateur le modifie, en déplaçant ou en redimensionnant. Il est mémorisé avec `user_ref`, la taille de la zone de travail dans laquelle il a été posé.
- **`display_rect` est dérivé**, recalculé à chaque changement de zone de travail — nouveau client, bascule du bord du panneau, `SIGWINCH`.

```
échelle  : x' = round(x × nouvelle_largeur / user_ref.w)     (idem en y)
taille   : conservée si elle rentre, sinon réduite, jamais sous max(min_size(), 16×5)
ancrage  : une fenêtre à droite reste à droite, en bas reste en bas
garantie : la barre de titre reste toujours partiellement visible, et jamais
           repoussée hors des bords haut et gauche
```

Le retour à la taille d'origine restitue donc exactement la disposition d'origine : la dérivation est réversible parce que sa source ne bouge pas.

Les fenêtres `Maximized` et `Fullscreen` voient leur rectangle **recalculé** à chaque changement de zone de travail, bascule du bord du panneau comprise, sans jamais toucher `user_rect`.

Deux corollaires de placement : les nouvelles fenêtres se posent en cascade (`+2, +1` par rapport à la précédente) plutôt que superposées, et un déplacement qui approche un bord s'y aimante.

Enfin, si la zone de travail passe **sous le minimum** (un terminal de 30×8), le bureau ne tente pas une disposition impossible : il affiche `terminal trop petit — 40×12 minimum`, et reprend son état exact dès que la place revient.

### 5.6 Le curseur

Il n'existe qu'un seul curseur matériel pour tout l'écran. Il appartient à l'application de la fenêtre focalisée si elle en veut un, et il est masqué sinon. Les terminaux non focalisés dessinent leur curseur comme une cellule en vidéo inverse dans la grille — un faux curseur, exactement ce que font tmux et les émulateurs pour leurs panneaux inactifs. On voit où chaque shell en est, sans ambiguïté sur celui qui reçoit les frappes.

---

## 6. Le shell du bureau

### 6.1 Le panneau

```cpp
struct Panel { Edge edge; int thickness; };   // bottom | left | top | right
Rect desktop = ecran - panel.rect();
```

Toute la géométrie du bureau découle de cette soustraction. Les quatre bords tombent du même mécanisme ; seuls le moteur de disposition et le hit-testing diffèrent entre horizontal et vertical.

```
En bas (1 ligne, défaut)
┌──────────────────────────────────────────────────────────┐
│                       (le bureau)                        │
├──────────────────────────────────────────────────────────┤
│ ☰ │ ⌂ ▤ ⚙ │ ● Terminal │ Fichiers │ _Moniteur │    14:32 │
└──────────────────────────────────────────────────────────┘

À gauche (16 colonnes)
┌────────────────┬─────────────────────────────────────────┐
│ ☰  Menu        │                                         │
│                │                                         │
│ ⌂  Terminal    │              (le bureau)                │
│ ▤  Fichiers    │                                         │
│ ⚙  Moniteur    │                                         │
│ ───────────    │                                         │
│ ● Terminal     │                                         │
│   Fichiers     │                                         │
│ _ Moniteur     │                                         │
│                │                                         │
│         14:32  │                                         │
│    lun 10 août │                                         │
└────────────────┴─────────────────────────────────────────┘
```

`●` marque la fenêtre active, `_` une fenêtre minimisée. Cliquer une minimisée la restaure ; cliquer l'active la minimise.

**Débordement.** Quand les boutons de fenêtres ne rentrent plus, les libellés rétrécissent avec élision (`Termin…`) jusqu'à un plancher d'environ 8 cellules ; en dessous, le surplus se replie dans un bouton `»3` qui ouvre la liste complète. Rien ne disparaît jamais silencieusement de la barre.

**Épinglées.** Cliquer une application épinglée déjà ouverte lui donne le focus au lieu d'en lancer une seconde. Clic milieu pour forcer une instance supplémentaire.

Le bord du panneau est rechargeable à chaud : basculer de bas à gauche ne perd aucune fenêtre.

### 6.2 Le menu

Superposition ancrée au `☰`, dessinée au-dessus de tout, avec un champ de recherche qui filtre à la frappe. Flèches et `Entrée` pour choisir, `Échap` pour fermer. Contient le catalogue d'applications, l'entrée Paramètres et « Quitter la session ».

### 6.3 L'horloge

Alimentée par le `timerfd` d'une seconde, mais ne salit l'écran que si la chaîne formatée a changé. Format configurable ; en panneau vertical, la date passe sur une seconde ligne.

---

## 7. L'entrée

### 7.1 Parsing

Le client n'interprète rien : il transmet les octets bruts. Le parseur vit dans le démon et doit résister à la fragmentation — une séquence coupée en deux par le réseau est le cas normal, pas le cas limite. L'état persiste entre les appels.

Il produit deux types d'événements : `KeyEvent{code, modificateurs}` et `MouseEvent{bouton, action, x, y, modificateurs}`.

### 7.2 Souris

Mode **1002 (button-event tracking)** plus **1006 (encodage SGR)**.

Le choix de 1002 plutôt que 1003 est délibéré : 1002 ne rapporte le mouvement que bouton enfoncé — exactement ce qu'exige un glisser-déposer — et ne coûte rien quand la souris traverse l'écran. Le mode 1003 émettrait un paquet par cellule parcourue et noierait le lien SSH. Contrepartie acceptée : pas d'effet de survol.

L'encodage SGR (1006) est requis : l'encodage historique X10 ne peut pas exprimer une coordonnée au-delà de la colonne 223. Les deux modes s'activent ensemble — 1006 seul ne rapporte rien, il ne choisit que la façon d'écrire ce que 1002 rapporte.

#### Décoder `Cb` bit à bit

Dans `CSI < Cb ; Cx ; Cy M|m`, `Cb` est un **champ de bits**, pas un numéro de bouton :

| Bits | Signification |
|---|---|
| 0-1 | bouton (0 gauche, 1 milieu, 2 droit, 3 « aucun ») |
| +4 | Shift |
| +8 | Alt |
| +16 | Ctrl |
| +32 | mouvement |
| +64 | molette (bit 0 : 0 = haut, 1 = bas) |
| +128 | boutons 8-11 |

Tout `switch (cb)` écrit sur des valeurs littérales se trompe dès le premier glissement avec un modificateur enfoncé. Le décodage se fait par masques, toujours.

**La molette n'est pas un bouton.** Elle n'émet jamais de `m` correspondant à son `M`. Une machine à états naïve « `M` = enfoncé, `m` = relâché » se verrouille donc en glissement au premier coup de molette, et la fenêtre suit le curseur indéfiniment. Les événements molette sont classés `Wheel` et sortent complètement de la machine à états de glissement. Piège hérité de X10 dans le même esprit : `cb & 3 == 3` avec un `M` sous 1003 signifie « mouvement sans bouton », pas « relâchement ».

`F12` coupe et remet le tracking, pour récupérer la sélection et le copier-coller natifs du terminal local. Comme `F12` est lui-même peu fiable (§7.4), `<leader>m` fait la même chose.

### 7.3 L'ambiguïté de Échap

`ESC` seul et le début de `ESC [ A` commencent par le même octet, et la latence SSH peut couper une séquence en deux paquets — « la suite est arrivée dans le même bloc » n'est donc pas un critère fiable.

Un `ESC` isolé arme un timer de **50 ms**. Si rien ne suit, c'était la touche Échap. La boucle étant déjà pilotée par un `timerfd`, le coût est nul et 50 ms sont imperceptibles. Valeur configurable.

### 7.4 Raccourcis : une touche leader, pas des accords `Alt`

Le réflexe est de calquer les raccourcis d'un vrai bureau — `Alt+Tab`, `Alt+Flèches`, `F12`. Sur un programme atteint par SSH, c'est le choix le moins fiable qui soit, et pour trois raisons distinctes :

- **`Alt+Tab` n'atteint jamais le fil.** Il est capturé par GNOME, KDE et macOS avant même le terminal.
- **`Alt+Flèches` a trois encodages incompatibles** : `\e[1;3D`, `\e\e[D`, et *rien du tout* sous Terminal.app dans sa configuration par défaut. Un utilisateur macOS n'aurait tout simplement pas accès à la moitié clavier du produit.
- **`F12` est déjà pris** : raccourci par défaut de Guake et Yakuake, touche média sur macOS. Le raccourci censé libérer la souris serait précisément le plus fragile.

Et même quand ils arrivent, capturer globalement `Alt+Flèches` vole `M-<flèche>` à emacs, `mc` et readline.

**Le chemin principal est donc une touche leader d'un seul octet**, qui traverse n'importe quelle configuration sans ambiguïté : `Ctrl+A` par défaut, configurable.

| Raccourci | Action |
|---|---|
| `<leader>` `←↑↓→` | Déplacer la fenêtre — *s'enchaîne* |
| `<leader>` `h` `j` `k` `l` | Idem, sans quitter la rangée de repos — *s'enchaîne* |
| `<leader>` `Shift+←↑↓→` | Redimensionner la fenêtre — *s'enchaîne* |
| `<leader>` `H` `J` `K` `L` | Idem — mêmes lettres, majuscules — *s'enchaîne* |
| `<leader>` `Tab` / `Shift+Tab` | Fenêtre suivante / précédente — *s'enchaîne* |
| `<leader>` `n` / `p` | Idem, sans dépendre de `Tab` — *s'enchaîne* |
| `<leader>` `w` | Fermer la fenêtre |
| `<leader>` `-` | Minimiser |
| `<leader>` `z` | Maximiser (bascule) |
| `<leader>` `f` | Plein écran sans décoration (bascule) |
| `<leader>` `Espace` | Ouvrir le menu |
| `<leader>` `m` | Bascule du tracking souris |
| `<leader>` `r` | Repaint complet forcé |
| `<leader>` `?` | Afficher la table des accords |
| `<leader>` `d` | Détacher le client, la session survit |
| `<leader>` `<leader>` | Émettre l'octet leader littéral à l'application |
| `Ctrl+Q` | Détacher — le geste réflexe, sans accord |
| `Shift+PgUp` / `PgDn` | Scrollback du terminal focalisé |

Les doublures `hjkl` et `HJKL` ne remplacent pas les flèches, elles s'y
ajoutent : les flèches sont ce qu'essaie en premier qui vient d'un vrai
bureau, `hjkl` ce que cherchent des doigts habitués à `vi`. Aucun des deux
publics n'a à apprendre le geste de l'autre.

**Les gestes marqués *s'enchaîne* gardent l'accord ouvert.** Un accord
ordinaire ne dure qu'une touche, ce qui est juste pour une bascule mais
absurde pour un déplacement : pousser une fenêtre de dix cellules
demanderait dix `Ctrl+A`. Après un geste enchaînable, la touche suivante
agit donc sans qu'on reprenne le leader, et ainsi de suite tant que l'écart
entre deux gestes reste sous **1,5 s** — c'est l'écart qui est borné, pas la
durée totale de la série, si bien qu'un seul accord suffit pour traverser
l'écran.

La fenêtre est longue exprès, et elle est pourtant sans danger : **en série,
seuls les gestes enchaînables sont captés.** Tout le reste rend la main à
l'application sans même être consommé. Un `w` tapé dans un document une
seconde après un déplacement s'écrit donc dans le document ; il ne ferme
pas la fenêtre. Le pire cas est qu'un `j` déplace au lieu de s'écrire —
visible, et défait par le geste inverse. `<leader>` rouvre un accord franc
depuis l'intérieur d'une série : on veut visiblement autre chose qu'un
déplacement de plus.

L'aide de `<leader>?` marque ces lignes d'un `∙` et porte la légende en
en-tête, de sorte que le comportement se lise là où on le cherche. Elle ne
s'ouvre en revanche jamais d'elle-même au milieu d'une série : le compte à
rebours répond à l'hésitation de qui vient de taper le leader, pas à celle
de qui pousse une fenêtre.

**`Ctrl+Q` détache, il ne détruit pas.** C'est le geste que la main fait
pour « quitter », et le laisser tuer la session serait le contraire exact
de ce que ce projet promet. Détruire la session pour de bon se demande
explicitement, par l'entrée « Quitter la session » du menu.

`Shift+Tab` est retenu parce que son encodage (`\e[Z`) est, lui, universel.

`<leader><leader>` est indispensable : `Ctrl+A` est le début de ligne de readline et le préfixe de `screen`. Sans échappement, on rendrait cette touche inaccessible dans tous les shells. C'est aussi pourquoi la touche est configurable — un utilisateur de `tmux` imbriqué voudra probablement autre chose que `Ctrl+A` ou `Ctrl+B`.

Les accords `Alt` restent implémentés en **couche secondaire, désactivée par défaut** (`[input] alt_chords = false`), acceptant les deux encodages `\e[1;3D` et `\e\e[D`. Qui les veut les active ; personne n'en dépend.

Un mode pass-through par fenêtre suspend toute interception, pour les cas où un programme invité a réellement besoin de tout le clavier.

### 7.5 Collage entre crochets et rapport de focus

Sans encadrement du collage, **chaque octet collé traverse le répartiteur de raccourcis globaux**. Coller un transcript coloré fait alors tirer des accords au milieu du texte — un `\e[1;3D` dans la charge utile déplace une fenêtre. C'est un bug de sécurité autant que d'ergonomie.

`DECSET 2004` est donc géré **aux deux couches** :

- **Vers le client** : le démon active `\e[?2004h` à l'attache. Ce qui arrive entre `\e[200~` et `\e[201~` est du texte, jamais des raccourcis, et part vers l'application par un rappel dédié `on_paste(std::string_view)`.
- **Vers l'invité** : chaque fenêtre Terminal modélise son propre état `DECSET 2004`. Le collage n'est ré-encadré vers le PTY que si l'enfant l'a demandé — sinon on transmet le texte nu.

Deux précautions : un `\e[201~` **littéral** présent dans la charge utile est retiré avant transfert, faute de quoi il fermerait le crochet prématurément et le reste s'exécuterait comme des commandes tapées ; et l'accumulation, qui s'étale sur plusieurs `read()`, est plafonnée à environ 4 Mo.

Même logique pour le focus : `\e[?1004h` à l'attache, `\e[I` et `\e[O` analysés comme des **événements** — jamais comme des frappes — et servant notamment à annuler un glissement en cours (§5.4).

---

## 8. Le modèle d'applications

### 8.1 Interfaces

Une application ne connaît ni le gestionnaire de fenêtres, ni le démon, ni le client. Elle connaît une `View` sur laquelle dessiner et un `Host` à qui demander des services.

```cpp
class Host {
  virtual void watch(int fd) = 0;              // surveiller dans l'epoll
  virtual void unwatch(int fd) = 0;
  virtual void invalidate() = 0;               // « j'ai changé, redessine »
  virtual void set_title(std::string) = 0;
  virtual void request_close() = 0;            // « ferme-moi » (:q, exit)
};

enum class IoStatus { Ok, Closed };          // « ce fd est fini, retire-le »

class App {
  virtual ~App() = default;
  virtual void render(View&) = 0;
  virtual void on_key(const KeyEvent&) = 0;
  virtual void on_mouse(const MouseEvent&) = 0;   // coordonnées locales
  virtual void on_paste(std::string_view) {}      // texte, jamais des raccourcis
  virtual void on_resize(int w, int h) = 0;
  virtual IoStatus on_io(int fd, uint32_t events) { return IoStatus::Ok; }
  virtual void tick() {}
  virtual Size min_size() const { return {20, 5}; }
  virtual bool wants_cursor(int& x, int& y) const { return false; }
  virtual CloseCheck can_close() { return CloseCheck::ok(); }
};
```

`watch`/`unwatch` plutôt qu'une méthode `pollfds()` interrogée périodiquement : un instantané de descripteurs n'a pas d'inverse. Ou bien les fds ouverts plus tard ne sont jamais surveillés, ou bien il faut comparer deux ensembles — et cette comparaison confond « même numéro de fd » avec « même chose ouverte », puisque `open()` rend toujours le plus petit numéro libre.

`on_io(fd, events)` plutôt que `on_readable(fd)`, pour une raison précise : **`EPOLLHUP` est signalé quel que soit le masque demandé.** Un répartiteur écrit en `if (events & EPOLLIN)` ne fait alors rien du tout, et `epoll_wait` revient instantanément, en boucle, à l'infini — un cœur à 100 % sans qu'aucune ligne de code n'ait l'air fautive. L'application doit voir les drapeaux réels et pouvoir répondre « ce descripteur est terminé ».

### 8.2 Deux invariants de cycle de vie

Ce sont les deux bugs qui tuent ce genre de programme. Ils se règlent par construction.

**Un fd surveillé ne survit jamais à son application.** L'implémentation de `Host` tient la liste des fds enregistrés par application ; la destruction d'une fenêtre les retire tous de l'epoll avant d'appeler `~App()`. Sans ça : l'application meurt, son fd est fermé, le noyau réattribue le même numéro à un nouveau PTY, l'epoll livre un événement sur ce numéro, le démon le route vers un pointeur mort.

L'ordre est **`unwatch` puis `close`**, jamais l'inverse — un `EPOLL_CTL_DEL` sur un descripteur déjà fermé rend `EBADF`.

Et le `epoll_data` ne contient pas le numéro de fd nu, mais une clé `(window_id << 32) | génération`. Un numéro de fd est réutilisable ; une clé générationnelle, non. Un événement en retard portant une clé périmée est reconnu comme tel et jeté.

**Aucune fenêtre n'est détruite pendant la distribution d'un événement.** Un `exit` dans un shell, un `Ctrl+Q` dans l'éditeur, un clic sur `[×]` : tout part dans une file de fermetures différées, drainée en fin de tour de boucle. Détruire une application au milieu d'un lot d'événements empoisonne les événements suivants **du même lot** : usage après libération, ou pire, numéro de fd réattribué entre-temps à un nouveau socket et donnée PTY livrée au mauvais propriétaire. Deux événements par tour suffisent à le déclencher, ce qui en fait un bug qu'on ne reproduit jamais deux fois pareil.

**Règle du drain-puis-mort.** Un `EPOLLHUP` ne signifie pas « jette ce descripteur maintenant » : il reste presque toujours des octets lisibles derrière. On lit jusqu'à `EAGAIN` ou `EIO`, on livre ce qu'on a lu, et seulement ensuite on retire et on ferme. Sans cela, un `ls; exit` affiche la fin du shell mais pas la sortie du `ls`.

### 8.3 Isolation des fautes

Les exceptions sortant d'un rappel d'application sont attrapées à la frontière de distribution : la fenêtre passe en état « plantée », affiche le message, l'application est détruite, le démon continue. Cela couvre les erreurs de logique, `bad_alloc`, les accès hors bornes des conteneurs.

Cela ne couvre **pas** une corruption mémoire. Dans un démon mono-processus, un segfault dans le gestionnaire de fichiers emporte tout, PTY compris. La parade est en amont : ASan et UBSan systématiques en debug, et le fait que le code le plus risqué — le parsing d'octets — est le module le mieux testé.

---

## 9. Les quatre applications

### 9.1 Terminal — environ 1 800 lignes

Trois couches nettes.

#### Le PTY

`posix_openpt(O_RDWR|O_NOCTTY)` → `grantpt` → `unlockpt` → `ptsname_r`, puis `fork`. Le maître est ouvert **`CLOEXEC` et non bloquant**.

Dans l'enfant : `setsid()` pour quitter la session du démon, `ioctl(esclave, TIOCSCTTY, 0)` pour acquérir le terminal de contrôle — sans quoi `Ctrl+C` ne génère aucun signal et le contrôle de tâches du shell ne fonctionne pas — puis `dup2` de l'esclave sur 0/1/2 et `execve`.

#### Assainir l'enfant avant `exec`

Trois héritages traversent `execve` et cassent silencieusement les programmes invités. Aucun n'est visible dans le code de l'enfant ; tous doivent être annulés explicitement.

**Le masque de signaux survit à `execve`.** Le démon bloque `SIGCHLD` pour le recevoir par `signalfd` — hériter de ce blocage casse `make -j8` dans chaque shell, qui n'apprend jamais que ses compilateurs sont morts. Il faut donc un `sigprocmask(SIG_SETMASK, &vide)` dans l'enfant.

**Les dispositions `SIG_IGN` survivent aussi.** Le démon met `SIGPIPE` à `SIG_IGN` (§10.4) — un enfant qui en hérite ne s'arrête plus jamais sur un tuyau fermé : `yes | head -1` tourne indéfiniment. Toutes les dispositions sont donc remises à `SIG_DFL`.

**Tous les descripteurs du démon sont `CLOEXEC`.** Un maître de PTY fuité laisse le shell de la fenêtre A lire la sortie de la fenêtre B, et empêche à jamais la libération du PTY ; un socket d'écoute fuité garde l'adresse abstraite occupée par un processus qui n'y répondra jamais (§3.3).

#### L'environnement

Le shell est lu dans `getpwuid()`, **pas dans `$SHELL`** : l'environnement du démon est un fossile de la première session SSH.

C'est le même problème pour tout le reste, et il mord fort : `SSH_AUTH_SOCK` pointe vers un agent mort depuis la première déconnexion, donc `git push` réclame une passphrase dans toutes les fenêtres, pour toute la durée de vie du démon.

Le handshake transporte donc un **delta d'environnement** — `SSH_AUTH_SOCK`, `SSH_CONNECTION`, `SSH_CLIENT`, `SSH_TTY`, `DISPLAY`, `XDG_SESSION_ID` — que le démon mémorise et applique aux **nouveaux enfants seulement**. C'est le modèle `update-environment` de tmux, pour la même raison : on ne peut pas modifier proprement l'environnement d'un processus déjà lancé.

Le reste est fixé par nous : `TERM=xterm-256color` — il décrit **notre** émulateur, jamais celui du client — `COLORTERM=truecolor`, `SSHOS=1` pour que les scripts sachent où ils sont. Ni `LINES` ni `COLUMNS` : la taille faisant autorité est celle du PTY, posée par `TIOCSWINSZ`, et le noyau se charge d'envoyer `SIGWINCH` au groupe de processus au premier plan.

Shell interactif non-login par défaut, configurable.

#### Le parseur VT

Machine à états au sens strict, de forme DEC : `Ground`, `Escape`, `EscapeIntermediate`, `CsiEntry`, `CsiParam`, `CsiIntermediate`, `CsiIgnore`, `OscString`, `DcsPassthrough`, `SosPmApcString`, plus un décodeur UTF-8 dans `Ground`. La machine fait environ 300 lignes mécaniques ; tout le travail est dans les actions.

**L'état survit entre les appels.** Le parseur est nourri de morceaux arbitraires venant de `read()`, et une séquence coupée en deux doit fonctionner. C'est un test de première classe.

Couverture requise pour que `vim`, `htop` et `less` fonctionnent réellement :

| Famille | Contenu |
|---|---|
| Déplacements | `CUP CUU CUD CUF CUB CHA VPA`, `IND RI NEL` |
| Effacements | `ED EL` (tous modes), `ICH DCH IL DL ECH` |
| Régions | `DECSTBM`, `DECSC` / `DECRC` |
| Attributs | `SGR` complet : 16 / 256 / `38;2;r;g;b` |
| Modes | `DECSET/DECRST` 1, 7, 25, 1000/1002/1003/1006, 1049, 2004 |
| Titre | `OSC 0/2` → alimente la barre de titre |
| Jeu de caractères | `SCS ESC ( 0` — semi-graphiques DEC |

Deux détails dont l'omission casse tout :

**Le retour à la ligne différé.** Écrire dans la dernière colonne ne provoque pas le retour : un drapeau « en attente » est posé et le retour n'a lieu qu'à l'arrivée du caractère suivant. Toute ligne de largeur pleine s'affiche de travers si on simplifie.

**Les requêtes qui attendent une réponse.** `\e[c` (Device Attributes), `\e[6n` (position du curseur), `DECRQM`. Un programme qui les émet **bloque** jusqu'à obtenir sa réponse. C'est le démon qui répond, en écrivant lui-même sur le maître du PTY. Relayer la question au vrai terminal serait faux à trois titres : la réponse décrirait le terminal du client, elle arriverait de façon asynchrone, et elle s'intercalerait au milieu des frappes de l'utilisateur.

#### L'écran

Le `DECSET 1049` d'un `vim` invité bascule le tampon d'écran **à l'intérieur de l'émulateur de cette fenêtre**. Ces octets ne sortent jamais vers le client, qui utilise l'écran alterné une seule fois, pour le bureau entier.

C'est la propriété centrale de l'architecture à client mince : **aucun octet invité n'est jamais relayé tel quel.** Le démon interprète tout, met à jour une grille, et re-synthétise sa propre sortie. Un `vim` dans une fenêtre ne peut pas plus perturber le bureau qu'un programme ne peut perturber le compositeur d'un vrai OS.

**Souris transmise aux invités.** Si le terminal focalisé a activé 1000/1002/1003, les événements souris tombant dans sa zone cliente sont ré-encodés en SGR 1006 avec des coordonnées locales et écrits sur le PTY : `htop` reste cliquable, la souris de `vim` fonctionne. Aucun conflit avec le déplacement de fenêtre, qui vit sur la barre de titre et les bordures.

**Scrollback** : tampon circulaire de 10 000 lignes rognées, configurable. La molette fait défiler quand l'écran alterné est inactif ; quand il est actif, elle est transmise à l'invité — comportement des vrais émulateurs. `Shift+PgUp/PgDn` en équivalent clavier.

**Redimensionnement** : `TIOCSWINSZ`, une seule fois au relâchement de la poignée (§5.4). La politique est écrite noir sur blanc, parce que ne pas la décider est la recette exacte du bug « mon terminal est mélangé après un redimensionnement » :

| Aspect | Politique |
|---|---|
| Écran principal, rétrécissement | **Troncature**, pas de reflow |
| Écran principal, élargissement | Complété par des blancs |
| Écran alterné | Jeté et régénéré par l'invité, qui reçoit `SIGWINCH` |
| Curseur | Contraint dans les nouvelles bornes |
| Région de défilement | Remise à pleine hauteur à **tout** changement de taille |

Le reflow reviendrait à invalider la position du curseur et **tous les décalages du scrollback** ; il interagit en plus avec les caractères pleine chasse. C'est un projet en soi, pas une option de v1.

**Fin de processus.** Deux événements que l'on croit confondus et qui sont indépendants **dans les deux sens** : un `nohup … &` garde l'esclave ouvert après la mort du shell, et un enfant qui se démonise ferme l'esclave avant sa propre mort. Deux drapeaux séparés, donc — `[processus terminé]` s'affiche au premier, le descripteur n'est libéré qu'au second.

`SIGCHLD` arrive par `signalfd`, et les signaux standards **ne sont pas mis en file** : trois enfants morts entre deux lectures produisent un seul enregistrement, dont le `ssi_pid` n'en nomme qu'un. On boucle donc sur `waitpid(-1, WNOHANG)` jusqu'à `0` ou `ECHILD`, et on draine le `signalfd` jusqu'à `EAGAIN`. Sans cette double boucle : zombies permanents, maîtres jamais fermés, `kernel.pty.max` (4096) épuisé, et des fenêtres qui affichent un shell mort.

Et l'on ne ferme **jamais** le maître sur simple réception de `SIGCHLD` : cela jetterait la sortie encore en tampon dans la discipline de ligne. Les noyaux récents livrent d'abord les données puis rendent `EIO` — il suffit de drainer.

La fenêtre affiche `[processus terminé (code 1) — Entrée pour fermer]` et reste ouverte, pour qu'on puisse lire la dernière erreur. Fermer une fenêtre dont le processus vit encore déclenche une confirmation, puis `SIGHUP` au groupe de processus, puis `SIGKILL` après un délai de grâce.

Répartition : PTY ~150, parseur ~700, écran ~600, liant ~300.

### 9.2 Gestionnaire de fichiers — environ 600 lignes

Panneau unique, barre de chemin, liste triée dossiers d'abord. `Entrée` descend ou ouvre dans l'éditeur, `Retour arrière` remonte, `F2` renommer, `Suppr` supprimer avec confirmation, `.` bascule les fichiers cachés, saisie au clavier pour filtrer. Pas de vue en arbre.

### 9.3 Moniteur système — environ 500 lignes

`/proc/stat` par cœur, `/proc/meminfo`, `/proc/loadavg`, liste de processus depuis `/proc/[pid]/stat`, triable par CPU ou mémoire. Rafraîchi sur le tick d'une seconde, et **uniquement quand la fenêtre est visible** : un moniteur minimisé ne consomme rien.

### 9.4 Éditeur — environ 800 lignes

Buffer en vecteur de lignes — pas de gap buffer : pour les tailles de fichiers concernées, c'est plus simple et suffisant. Navigation aux flèches, `Ctrl+S` enregistrer, `Ctrl+X` quitter avec confirmation si modifié, recherche simple. **Pas `Ctrl+Q`** : le bureau l'intercepte pour détacher (§7.4) et l'éditeur ne le verrait jamais. C'est la contrepartie assumée d'un geste de détachement sans accord ; une application qui tient absolument à `Ctrl+Q` devra passer par le mode pass-through. **Pas de coloration syntaxique.** Il arrive en dernier, et c'est assumé : on peut déjà lancer `vim` dans une fenêtre Terminal.

---

## 10. Cycle de vie du démon et du client

### 10.1 Survivre à la session SSH

Toute la valeur du programme tient là. Trois obstacles, et il faut les traiter tous les trois.

**Sortir de la session.** À la déconnexion, `sshd` envoie `SIGHUP` au groupe de processus de la session ; un démon encore dedans meurt — et `Ctrl+C` dans la session le tuerait tout autant, par `SIGINT` au groupe. Séquence complète, dans cet ordre :

```
fork
  └─ enfant : setsid()                 nouvelle session, plus de terminal de contrôle
              fork                     l'orphelin ne peut plus jamais en réacquérir un
                └─ petit-enfant :
                     chdir("/")        ne pas retenir un point de montage
                     0/1/2 → /dev/null, journal
                     sigprocmask(vide) le masque hérité survivrait à l'exec
                     dispositions → SIG_DFL, puis SIGHUP → SIG_IGN
                     execve("/proc/self/exe", {"sshos", "--daemon"})
```

L'`execve` sur `/proc/self/exe` plutôt qu'un simple retour de fonction donne un espace d'adressage neuf, sans rien hériter de l'état du client — et fonctionne quel que soit le répertoire courant, `$PATH` compris.

**Jamais de `PR_SET_PDEATHSIG`.** Ce serait l'exact contraire du but recherché : le démon mourrait avec le client qui l'a lancé.

Le lancement est particulièrement piégeux à tester : tuer le *client* ne révèle rien du tout. Seul un test qui fait réellement `exit` de la session SSH expose une erreur de détachement.

**Lâcher le tuyau.** `sshd` ne ferme pas le canal SSH tant qu'un processus garde ouverte l'extrémité du PTY de la session. Un démon qui hérite de `stdout` et le conserve fait **pendre le `ssh` de l'utilisateur à la déconnexion** — le terminal reste figé jusqu'à ce qu'on tape `~.`. D'où la redirection de 0/1/2 avant tout le reste.

**Attendre le démon proprement.** Le client ne dort pas une durée arbitraire : il fait `waitpid()` sur l'enfant intermédiaire — qui se termine immédiatement — puis retente `connect()` en boucle, environ cinquante fois toutes les 20 ms. Le cas normal réussit au premier ou deuxième essai.

**Le dernier obstacle est `logind`.** L'adresse de socket abstraite (§3.3) neutralise déjà la suppression de `$XDG_RUNTIME_DIR`, mais pas `KillUserProcesses=yes`, qui tue à la déconnexion tout ce qui reste de l'utilisateur. Le démon **détecte ce réglage au premier lancement et avertit** — un message clair vaut mieux qu'une session mystérieusement disparue. La parade est documentée : `loginctl enable-linger <user>`, ou lancer le démon sous `systemd-run --user --scope`.

### 10.2 Handshake

```
client ──► Hello{build_id, cols, rows, TERM, COLORTERM, utf8, ea_ambiguous,
                 env_delta{SSH_AUTH_SOCK, SSH_CONNECTION, SSH_CLIENT,
                           SSH_TTY, DISPLAY, XDG_SESSION_ID}}
       ◄── Welcome{}   ou   Incompatible{"démon en version X — relance sshos --kill"}
```

Si un client est déjà attaché, il reçoit `Detached{"session reprise ailleurs"}`, restaure son terminal et rend la main au shell. Le démon redimensionne ensuite le bureau à la taille du nouveau venu, remet les fenêtres à l'échelle (§5.5), invalide la grille précédente et envoie une frame complète.

### 10.3 Le client

Un `TtyGuard` RAII fait, dans l'ordre : sauvegarde de `termios`, mode brut (ni `ICANON`, ni `ECHO`, ni `ISIG`, ni `IXON`), écran alterné, souris `?1002h` + `?1006h`, collage entre crochets `?2004h`, rapport de focus `?1004h`, retour à la ligne automatique désactivé `?7l`, masquage du curseur.

Le destructeur fait l'exact inverse dans l'ordre inverse — et l'ensemble à restaurer est nommé ici pour qu'aucun mode ne soit oublié : `?25h`, `?7h`, `?1004l`, `?2004l`, `?1006l`, `?1002l`, sortie de l'écran alterné, `termios` d'origine.

**Tous** les chemins de sortie passent par lui : fin normale, `Detached`, EOF du socket, `SIGINT`/`SIGTERM`/`SIGHUP`.

Filet de dernier recours : un gestionnaire de `SIGSEGV` qui écrit la séquence de restauration directement avec `write(2)` — sûr en contexte de signal. Même un plantage du client rend un terminal propre.

`SIGWINCH` → envoi de la nouvelle taille au démon.

`Ctrl+C` n'a aucun traitement spécial : en mode brut c'est l'octet `0x03`, relayé au démon, écrit sur le PTY de la fenêtre focalisée, où la discipline de ligne du noyau le transforme en `SIGINT` pour le programme invité.

### 10.4 Contre-pression

**Aucune écriture n'est bloquante nulle part** — ni sur le socket client, ni sur les maîtres de PTY. Un `write()` bloquant sur un lien SSH lent gèlerait l'unique thread : plus aucun PTY drainé, et le `make` qui devait continuer se retrouve bloqué en écriture sur son propre terminal. C'est exactement la promesse du produit qui tombe, et elle tombe **parce que** le client est lent — c'est-à-dire dans le cas où le produit devait précisément briller.

Le socket client est donc non bloquant, les octets en attente s'accumulent dans une file, et `EPOLLOUT` prend le relais. **Armé et désarmé**, jamais laissé armé : un `EPOLLOUT` en déclenchement par niveau sur un socket inscriptible réveille la boucle en continu, soit un cœur à 100 %.

`SIGPIPE` est mis à `SIG_IGN` (ou chaque envoi passe `MSG_NOSIGNAL`) : sans cela, **un client qui meurt tue le démon**, et avec lui tout le bureau. C'est la disposition dont §9.1 exige l'annulation avant chaque `exec`.

Au-delà d'un plafond de file d'environ **1 Mo**, on ne rogne pas : on **jette la file entière, on invalide la frame précédente et on inscrit un repaint complet.** Un diff de cellules n'est pas idempotent — appliquer une moitié d'arriéré produirait un écran faux — alors qu'un repaint est un sur-ensemble sûr de tout ce qui était en attente. Une frame fraîche est en outre souvent plus petite qu'un arriéré de deltas périmés. Le retard se résorbe au lieu de s'aggraver.

Tant que la file reste haute, l'état VT continue d'être mis à jour normalement : on cesse seulement de composer. Les programmes invités ne sont jamais ralentis.

---

## 11. Erreurs et journalisation

| Panne | Réponse |
|---|---|
| Exception dans une application | Fenêtre « plantée », application détruite, démon continue |
| PTY en EOF | Fenêtre `[processus terminé (code N)]`, reste ouverte |
| Erreur socket / client tué | Détache propre, démon vit |
| Client voit EOF | Terminal restauré, message + chemin du journal |
| **Démon planté** | **Session perdue.** Coût assumé de l'absence d'instantané |
| Configuration illisible | Valeurs par défaut, journal, notification dans le panneau |
| Plus de 64 fenêtres / fd épuisés | Lancement refusé, notification |
| `bind()` rend `EADDRINUSE` | Un démon existe déjà : se connecter au lieu d'en démarrer un |
| Pair `SO_PEERCRED` d'un autre uid | Connexion refusée, entrée au journal |
| `logind KillUserProcesses=yes` | Avertissement au premier lancement, avec la parade (§10.1) |
| Zone de travail sous le minimum | Écran `terminal trop petit`, état conservé intact |

Journal : `~/.local/state/sshos/daemon.log`, rotation à une taille plafond.

---

## 12. Configuration

Seule la **configuration** va sur le disque. L'**état de session**, jamais. La ligne de partage est nette.

`~/.config/sshos/config.ini`, parseur maison d'environ 120 lignes.

```ini
[panel]
edge = bottom            ; bottom | left | top | right
thickness = 16           ; colonnes, si edge = left|right
clock_format = %H:%M
date_format = %a %d %b

[pinned]
apps = terminal, files, monitor

[terminal]
shell =                  ; vide = getpwuid()->pw_shell
scrollback = 10000

[input]
leader = ctrl-a          ; touche leader mono-octet — <leader><leader> = octet littéral
esc_timeout_ms = 50
alt_chords = false       ; couche secondaire Alt+flèches, désactivée par défaut
ea_ambiguous = probe     ; probe | narrow | wide

[render]
backpressure_kb = 1024   ; au-delà : file jetée, repaint complet

[theme]
accent = #4a9eff
```

Rechargeable à chaud depuis le menu — c'est ainsi qu'on bascule la barre de bas à gauche sans rien perdre.

---

## 13. Tests

### 13.1 Harnais

Une centaine de lignes : une macro `TEST(nom)` qui s'enregistre dans un vecteur statique, `CHECK` / `CHECK_EQ`, un `main` qui déroule et imprime les échecs en `fichier:ligne`. Pas de gtest — le projet a fait vœu de zéro dépendance et ce harnais suffit réellement.

### 13.2 Tests unitaires

Surface et clipping · le diffeur, avec assertion sur les **octets ANSI exacts** produits par un changement connu, enveloppe de frame comprise · le décodage de `Cb` en champ de bits, avec la table complète des combinaisons bouton × modificateurs et la molette hors machine à états · l'encadrement du collage, y compris un `\e[201~` littéral dans la charge utile · le parseur d'entrée (séquences coupées, UTF-8 tronqué, `ESC` isolé, accords `Alt` dans leurs deux encodages) · la géométrie du WM (hit-test des boutons, contraintes de glissement, aller-retour maximiser/restaurer, et surtout **la réversibilité de `user_rect` → `display_rect`** sur un aller-retour 160×50 → 80×24 → 160×50) · les sept chemins d'annulation de glissement, qui doivent tous produire la même géométrie · le codec du protocole en aller-retour · le parseur de configuration.

Pour le parseur VT, une propriété vaut cent cas : **nourrir la même séquence en un bloc, puis octet par octet, et exiger la grille identique.** Cela teste la reprise d'état sur tout le corpus d'un coup. Complété par du fuzzing d'octets aléatoires sous ASan — aucune assertion sur le résultat, seulement l'absence de crash et de débordement.

### 13.3 La couture d'injection

`Session` telle qu'elle vient ne peut pas être testée : elle construit des applications qui forkent un shell, lisent `/proc`, lisent `$HOME` et appellent `time()`. Un harnais bâti dessus serait non déterministe **par construction**, et un journal global interdirait d'instancier deux `Session` dans un même binaire de test.

D'où une couture explicite, injectée à la construction :

```cpp
struct Platform {
  virtual Child spawn(const SpawnSpec&) = 0;
  virtual std::string read_file(std::string_view path) = 0;
  virtual std::chrono::system_clock::time_point now() = 0;
};
```

Plus un puits de journalisation injecté plutôt que global. En test, `Platform` est un double : horloge figée, `/proc` en dur, processus factices. Le harnais devient déterministe et réentrant.

### 13.4 Frames golden — peu, et sur ce qui compte

Le réflexe est de figer des captures de bureau entier. C'est la technique qui pourrit le plus vite : elle est fragile sur ce dont personne ne se soucie — un glyphe, une colonne de remplissage — et **aveugle sur ce qu'elle devrait protéger** : la barre de titre focalisée a-t-elle bien la couleur de focus ? Changer un caractère de bouton casse trente fichiers de dix mille caractères, et `UPDATE_GOLDEN=1` devient un réflexe en deux semaines. À ce moment-là le corpus ne teste plus rien.

Le mécanisme principal est donc l'**assertion de propriété** sur la frame composée :

```cpp
CHECK_EQ(frame.text_row(0).substr(2, 8), "Terminal");
CHECK_EQ(frame.cell(3, 0).fg, theme.focus_fg);
```

Les goldens ne subsistent qu'en appoint : **six à dix fichiers**, en 60×20 recadré, thème ASCII fixe, la couche couleur dans un fichier séparé du texte. `UPDATE_GOLDEN=1` régénère **et imprime le diff** — on ne peut pas régénérer sans avoir vu ce qu'on change.

### 13.5 Le test qui compte

Celui de la fonctionnalité phare, et il s'automatise :

```
démarrer un démon sur une adresse abstraite temporaire
  → client factice s'attache
  → lance un Terminal, tape « sleep 300 & »
  → SIGKILL sur le client
  → se rattacher
  → assertion : la fenêtre est là, le PID enfant est vivant, la grille est identique
```

S'il passe, le projet fait ce qui a été demandé — **presque**. Ce test tue le *client*, et un client tué ne prouve rien du détachement (§10.1) : le démon peut très bien être resté dans la session SSH et mourir au vrai `logout`. Il faut donc un second scénario, moins commode mais indispensable :

```
ssh localhost 'sshos --daemon-start && exit'   ← vraie session, vraie sortie
  → attendre la fermeture effective de la session
  → sshos --status doit répondre, et le PID enfant doit être vivant
```

C'est le seul qui expose une erreur de `setsid`, de double `fork` ou de redirection de `stdout`. Il exige `ssh` vers `localhost` et sera marqué comme test d'intégration facultatif, mais il doit exister.

### 13.6 Liste manuelle irréductible

À cocher avant chaque version : `vim`, `htop`, `less`, un `tmux` imbriqué dans une fenêtre, un nom de fichier en japonais et un emoji ZWJ, un client en 300 colonnes, un redimensionnement en pleine compilation, un copier-coller natif après `<leader>m`, le collage d'un transcript coloré de plusieurs milliers de lignes, `yes | head -1` dans une fenêtre, `make -j8` dans une autre, et une déconnexion SSH franche pendant que les deux tournent.

---

## 14. Build

CMake, C++20, `-Wall -Wextra -Wpedantic -Werror`. `-fsanitize=address,undefined` en Debug, `-O2` en Release, `-static` optionnel. Zéro dépendance : `cmake && make` sur une machine nue.

Toolchain vérifiée sur la machine cible : g++ 15.2.0, CMake 4.2.3, GNU Make 4.4.1, Ubuntu 26.04 LTS.

---

## 15. Ordre de construction

| Jalon | Contenu | Sortie visible |
|---|---|---|
| 1 | Rendu, diff, protocole, client | Une boîte colorée à l'écran, à travers SSH |
| 2 | WM, panneau, menu, application factice | Tout le geste testable sans PTY |
| 3 | **Terminal** | Le projet devient utilisable pour de vrai |
| 4 | Gestionnaire de fichiers | |
| 5 | Moniteur système | |
| 6 | Éditeur | |

À la fin du jalon 3 le projet a déjà sa valeur. Les trois suivants sont incrémentaux et ne touchent aucun fichier existant, sauf le catalogue d'applications.

---

## 16. Risques ouverts

| Risque | Mitigation |
|---|---|
| Le parseur VT est le composant le plus long et le plus subtil | Machine à états standard, tests par propriété, fuzzing sous ASan, jalon dédié |
| Un segfault dans une application emporte le démon et toutes les sessions | ASan/UBSan en debug, exceptions attrapées, code de parsing sur-testé. Résiduel accepté |
| Le corpus golden se régénère sans être relu | Assertions de propriété en mécanisme principal, 6-10 goldens en appoint, diff imprimé à la régénération |
| Les frames golden ne couvrent pas le rendu réel du terminal client | Liste manuelle §13.6 |
| Un désaccord de largeur de glyphe ne se répare jamais de lui-même | Quatre règles du §4.1, table Unicode embarquée, sonde EA à l'attache, repaint forcé `<leader>r` |
| La touche leader est peu découvrable pour qui vient d'un vrai bureau | **Livré au jalon 2** : rappel `^A = aide` dans le panneau, cliquable, qui cède la place aux tâches quand la barre se remplit ; `<leader>` laissé en l'air 500 ms ouvre la table des accords, que `<leader>?` et le clic sur le rappel ouvrent aussi. Accords `Alt` toujours en réserve |
| `logind` tue le démon malgré tout (`KillUserProcesses=yes`) | Détection et avertissement au premier lancement, parade documentée |
| Volume total 12 000–15 000 lignes | Découpage en jalons livrables ; le jalon 3 suffit à rendre le projet utile |
