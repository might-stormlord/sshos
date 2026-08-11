# ssh_os 2.0 — Jalon 2 : WM, panneau, menu, applications factices

**Date :** 11 août 2026
**Statut :** conception validée, prête pour la planification d'implémentation
**Spec de référence :** `docs/superpowers/specs/2026-08-10-ssh-os-design.md` (document maître)
**Socle :** jalon 1 livré au commit `00cfc4b`, 189 tests au vert

---

## 1. Objet

Le document maître décrit le gestionnaire de fenêtres (§5), le shell du bureau (§6) et le modèle d'applications (§8). Ces sections sont validées et **ne sont pas rouvertes ici**. Ce document ne fait que trois choses :

1. fixer le **périmètre exact** du jalon 2, y compris les deux pièces que le §15 du document maître n'attribue à aucun jalon ;
2. spécifier les **interfaces nouvelles** que le document maître ne détaille pas — la couture de descripteurs, le thème, `View::box`, la table de raccourcis, la modale ;
3. arrêter la **stratégie de test** et l'**ordre de construction**.

Tout ce qui n'est pas contredit ici reste régi par le document maître. En cas de désaccord entre les deux, c'est le document maître qui fait foi, et la divergence est un défaut de ce document à corriger.

### Sortie visible attendue

Le §15 du document maître la formule ainsi : **« tout le geste testable sans PTY »**. Concrètement, à la fin du jalon on se connecte en SSH, on tape `sshos`, et on obtient un bureau où l'on peut : ouvrir des fenêtres depuis un menu, les déplacer et les redimensionner à la souris, les minimiser, les maximiser, les passer en plein écran, les fermer — avec confirmation quand l'application la demande —, basculer le panneau du bas vers la gauche sans rien perdre, et tout refaire au clavier par la touche leader. Ce qu'il n'y a pas encore dans les fenêtres, c'est un shell : ce sera le jalon 3.

---

## 2. Périmètre

### Dans le jalon

| Domaine | Contenu |
|---|---|
| `wm/` | `Window`, modes, géométrie `user_rect`/`display_rect`, z-order, focus, hit-test, machine à états de glissement, décorations |
| `app/` | interfaces `App` et `Host`, `IoStatus`, `CloseCheck`, catalogue d'applications |
| `apps/` | deux applications factices : **Bloc** (dessin) et **Battement** (à descripteur) |
| `shell/` | panneau aux quatre bords, menu, horloge, dialogue modal de confirmation |
| `render/` | `Theme`, `View::box()` et `Border` |
| `input/` | table de raccourcis `LeaderDispatch` (§7.4 du document maître) |
| `daemon/` | implémentation de `Host`, clés générationnelles, fermetures différées ; `Session` recomposée |

### Hors du jalon, assumé

- **Le parseur de configuration INI** (§12 du document maître). Le jalon 2 fonctionne sur des valeurs par défaut codées en dur ; le bord du panneau et la touche leader restent modifiables **à l'exécution** par une entrée de menu, ce qui suffit à démontrer et à tester le rechargement à chaud de la géométrie. Le parseur INI viendra brancher un fichier sur des points d'entrée qui existeront déjà.
- **Tout ce qui touche un PTY** : `Platform::spawn()`, `TIOCSWINSZ`, le parseur VT, le scrollback, la souris transmise aux invités. Jalon 3.
- **Les accords `Alt`** (`[input] alt_chords`), couche secondaire désactivée par défaut. Ils n'ont d'intérêt qu'avec la configuration.
- L'aide affichée par `<leader>` seul (§16 du document maître, mitigation de découvrabilité).

### Décisions de périmètre prises pendant cette conception

| Question | Décision | Raison |
|---|---|---|
| Raccourcis leader dans ce jalon ? | **Oui** | Sans eux, le mode `Fullscreen` du §5.2 n'a **aucun geste pour l'atteindre** : les décorations n'exposent que trois boutons, et aucun n'est le plein écran. Il serait livré non atteignable, donc non testé. Le jalon 3 en a par ailleurs besoin dès son premier jour (`<leader><leader>`, `<leader>m`, scrollback). |
| Parseur INI dans ce jalon ? | **Non** | Rien d'autre ne l'attend, et une entrée de menu démontre la même propriété (bascule de bord sans perte de fenêtres) pour un dixième du code. |
| Combien d'applications factices ? | **Deux** | Un catalogue à une seule entrée ne prouve ni le « ajouter une application ne modifie qu'une ligne » du §3.5, ni le comportement de la barre des tâches avec des applications de natures différentes. Et il faut une application **propriétaire d'un descripteur** pour que les invariants du §8.2 soient exercés (voir §6.2 ci-dessous). |
| `can_close()` et la modale ? | **Oui** | `can_close()` fait partie de l'interface `App` qu'on fige maintenant. Le livrer sans rien derrière signifierait que le jalon 3 découvre la seule surface modale du produit en même temps que le parseur VT. |

---

## 3. Ce que le jalon 1 fournit, ce qui manque

| Acquis au jalon 1 | Manquant, à écrire ici |
|---|---|
| `Surface`, `View` (`put`, `text`, `fill`, `sub`) | `View::box(Rect, Border, Style)` — listé au §4.2 du document maître, jamais écrit |
| `Cell`, `Style`, `Color`, `Rect`, `Size`, `Pos` | `Theme` — `session.cpp` code aujourd'hui ses couleurs en `Color::indexed()` en dur |
| diffeur → ANSI, `OutputProfile` (profondeur, UTF-8) | quantification d'un thème RGB vers 256 et 16 couleurs |
| parseur d'octets → `KeyEvent`, `MouseEvent`, `PasteEvent`, `FocusEvent` | `LeaderDispatch` : touches → `Action` |
| `Platform` (`now()`, `read_file()`) | `FdRegistrar` : la seconde couture (§4.2 ci-dessous) |
| boucle `epoll`, contre-pression, cadence 33 ms | routage des événements de descripteurs vers les applications |
| `Session`, bouchon de 123 lignes | `Session` recomposée : WM + panneau + menu + modale |

Rien n'existe dans `src/wm/`, `src/shell/`, `src/app/`, `src/apps/` : ces répertoires sont créés par ce jalon.

---

## 4. Architecture

### 4.1 Arborescence ajoutée

```
src/wm/       window        Window, Mode, user_rect / user_ref / display_rect
              layout        mise à l'échelle, ancrage, cascade, aimantation, plancher
              decor         barre de titre, bordures, trois boutons, poignée
              hittest       Hit, HitResult, ordre de résolution
              manager       z-order, focus, MAE de glissement, file de fermetures
src/app/      app           App, Host, IoStatus, CloseCheck
              catalog       entrées {id, nom, fabrique}
src/apps/     bloc          application factice de dessin
              battement     application factice propriétaire d'un descripteur
src/shell/    panel         Edge, épaisseur, disposition H/V, débordement, épinglées
              menu          superposition, champ de recherche, filtrage
              clock         formatage, ne salit que si la chaîne change
              modal         dialogue de confirmation
src/render/   theme         couleurs en RGB, quantifiées par OutputProfile
src/input/    shortcuts     LeaderDispatch : KeyEvent -> Action
src/daemon/   host          registre de descripteurs, clés générationnelles
              session        recomposée : assemble WM, panneau, menu, modale
```

### 4.2 Règle de dépendance

Celle du §3.5 du document maître, avec une précision qui la rend vérifiable :

```
render  →  (rien)
input   →  (rien)                    ← LeaderDispatch ne connaît pas le WM
app     →  render
wm      →  render, app
shell   →  wm, render
apps/*  →  render, app               ← une application ne peut pas atteindre le WM
daemon  →  tout
```

Une précision par rapport au §3.5 du document maître, qui écrivait `wm → render` : **`wm → app` est ajouté**, parce que `Window` détient un `std::unique_ptr<App>` (§5.1 du maître) et lit `min_size()`. La dépendance était implicite dès l'origine ; elle est ici rendue explicite. Le sens reste à sens unique et `app/` continue de ne rien savoir du WM.

**`wm/` et `shell/` ne connaissent ni descripteur, ni `epoll`.** `daemon/host` est le seul pont. Conséquence directe et recherchée : toute la géométrie et tout le dessin sont testables sans démon, dans un binaire de test qui n'ouvre aucun socket.

### 4.3 Les deux coutures injectées dans `Session`

`Platform` existe déjà. La seconde est nouvelle et découle d'une contrainte que le document maître ne relève pas : **`Host::watch(fd)` a besoin de l'`epoll`, que `Session` ne voit pas.**

```cpp
struct FdRegistrar {
  virtual ~FdRegistrar() = default;
  virtual void watch(uint64_t key, int fd, uint32_t events) = 0;
  virtual void unwatch(int fd) = 0;   // toujours AVANT close(), jamais après
};
```

En production, l'implémentation appelle `epoll_ctl` sur l'`epoll` du démon. En test, un double enregistre la séquence des appels : les trois invariants de cycle de vie du §8.2 deviennent des assertions, pas des espoirs.

### 4.4 Clés générationnelles

`epoll_data` porte une clé de 64 bits, jamais un numéro de descripteur nu.

```
clé = (window_id << 32) | génération
```

La génération est un compteur monotone de `HostImpl`, **incrémenté à chaque appel de `watch()`** — pas à chaque création de fenêtre. C'est ce qui couvre le danger réel : une fenêtre qui vit, ferme son descripteur A, en ouvre un B auquel le noyau réattribue le même numéro. Un événement déjà en file pour A serait sinon livré à B.

`HostImpl` tient deux tables : `clé → {window_id, fd}` et `fd → clé`. `unwatch(fd)` efface les deux. **Un événement dont la clé est absente de la table est jeté sans bruit** — c'est la définition d'un événement périmé.

Les descripteurs propres au démon (écoute, client, `timerfd`, `signalfd`) prennent `window_id == 0` avec des générations réservées `1` à `4`. Ils ne peuvent donc jamais entrer en collision avec ceux d'une fenêtre, dont l'`id` commence à `1`.

---

## 5. Le gestionnaire de fenêtres

### 5.1 La fenêtre

Le §5.1 du document maître donne la structure. Deux précisions nécessaires à l'implémentation :

- `WindowId` est un `uint32_t` **monotone, jamais réutilisé**. Le plafond de 64 porte sur les fenêtres vivantes, pas sur les identifiants distribués.
- Le passage en `Fullscreen` mémorise le mode antérieur dans un champ `before_fullscreen` (`Normal` ou `Maximized`). Le retour le restitue.

**Une seule fenêtre à la fois peut être en plein écran** : donner le focus à une autre fenêtre ramène la précédente à son `before_fullscreen`. Sans cette règle, une fenêtre plein écran resterait devant celle qu'on vient de focaliser, et le focus deviendrait invisible — un état où le clavier va quelque part que l'écran ne montre pas.

### 5.2 Géométrie des décorations

Le cadre est `display_rect`. La barre de titre remplace la bordure haute et occupe **toute la largeur** du cadre :

```
╭─ Bloc — sans titre ──────────────────[_][□][×]╮
│                                               │
│              (zone cliente)                   │
│                                               │
╰──────────────────────────────────────────────◢╯
```

| Élément | Position |
|---|---|
| Zone cliente | `{x+1, y+1, w-2, h-2}` |
| Barre de titre | ligne `y`, colonnes `x` à `x+w-1` |
| Trois boutons | 3 cellules chacun, calés à droite, avant le coin : `x+w-10` à `x+w-2` |
| Bordure droite | colonne `x+w-1`, lignes `y+1` à `y+h-2` |
| Bordure basse | ligne `y+h-1`, colonnes `x+1` à `x+w-2` |
| Poignée `◢` | cellule `(x+w-1, y+h-1)` |

Le titre est tronqué avec élision pour laisser place aux boutons. Si la largeur ne permet malgré tout pas les trois boutons, ils sont retirés dans l'ordre `[_]`, `[□]`, `[×]` — le plus destructeur disparaît en dernier. Le plancher de 16 colonnes garantit que ce chemin n'est pas emprunté en usage normal ; il existe pour que le code n'ait pas de cas non défini.

**Taille minimale du cadre :**

```
frame_min.w = max(app.min_size().w + 2, 16)
frame_min.h = max(app.min_size().h + 2, 5)
```

`min_size()` porte sur la **zone cliente**, le plancher global de 16×5 sur le **cadre**. Confondre les deux est le genre d'erreur qui ne se voit qu'avec une application déclarant une taille minimale inhabituelle ; la formule est écrite ici pour qu'elle soit testée telle quelle.

### 5.3 Hit-test

L'ordre de résolution est celui du §5.3 du document maître — **modale, menu, panneau, fenêtres du premier plan vers l'arrière** — et c'est l'**inverse exact** de l'ordre de composition. Cette symétrie est une propriété testable, pas une coïncidence (§12.1).

**Un enum global de cibles violerait la règle de dépendance** : `wm/` nommerait des cibles de panneau et de menu, qui sont des notions de `shell/`. Chaque module résout donc *chez lui*, avec son propre type de résultat, et `Session` — seule à voir les deux étages — les séquence.

```cpp
// wm/hittest.hpp — uniquement l'intérieur d'une fenêtre
enum class WinHit { None, TitleBar, ButtonMinimize, ButtonMaximize, ButtonClose,
                    EdgeRight, EdgeBottom, CornerBR, Client };
struct WinHitResult {
  WinHit what = WinHit::None;
  WindowId win = 0;
  int lx = 0, ly = 0;                 // coordonnées locales, si what == Client
};

// shell/panel.hpp
enum class PanelHit { None, Body, MenuButton, Pinned, Task, Overflow, Clock };
struct PanelHitResult {
  PanelHit what = PanelHit::None;
  int index = -1;                     // rang de l'épinglée
  WindowId win = 0;                   // fenêtre visée, si what == Task
};

// shell/menu.hpp   -> MenuHit  { None, Body, Search, Item }  + index
// shell/modal.hpp  -> ModalHit { None, Body, Cancel, Confirm }
```

`Session::hit(x, y)` interroge dans l'ordre : modale si ouverte (et s'arrête là, quel que soit le résultat — une modale capte même les clics sur son fond), menu si ouvert, panneau si le point y tombe, puis les fenêtres du premier plan vers l'arrière.

Une fenêtre `minimized` n'est ni peinte ni atteignable par le hit-test. Elle reste présente dans le panneau.

### 5.4 Déplacement et redimensionnement

Machine à états à trois positions, conforme au §5.4 du document maître :

```cpp
struct Idle {};
struct Moving   { WindowId win; int grab_dx, grab_dy; };
struct Resizing { WindowId win; Rect outline; };
using DragState = std::variant<Idle, Moving, Resizing>;
```

- **Le déplacement est live** : `display_rect` suit le pointeur, la fenêtre est repeinte à sa nouvelle place, l'application n'est notifiée de rien.
- **Le redimensionnement est un contour élastique** : seul `outline` bouge pendant le glissement. `on_resize()` **et rien d'autre** est appelé une seule fois, au relâchement.

**Les sept chemins d'annulation** (`Échap`, n'importe quelle frappe, perte de focus du terminal `\033[O`, désactivation de la souris, détachement, attache, rapport de mouvement sans bouton, plus un chien de garde d'environ deux secondes) **valident tous la géométrie courante**. Un test par chemin, tous asseyant la même géométrie finale : c'est la divergence entre eux qui produirait des défauts non reproductibles.

Le chien de garde s'appuie sur le `timerfd` d'une seconde déjà présent : deux ticks sans événement de souris pendant un glissement le terminent.

### 5.5 Géométrie du bureau

Le §5.5 du document maître est appliqué tel quel : `user_rect` autoritaire et jamais réécrit par un changement de taille du bureau, `display_rect` dérivé et recalculé à chaque nouveau client, bascule de bord du panneau ou `SIGWINCH`.

Deux corollaires de placement à implémenter : cascade `(+2, +1)` pour les nouvelles fenêtres, et aimantation d'un bord approché à une cellule d'un bord de la zone de travail.

En dessous de 40×12, l'écran « terminal trop petit » du jalon 1 est conservé, et **l'état complet du bureau est préservé intact** — aucune fenêtre n'est redimensionnée, aucun `display_rect` n'est recalculé tant que la place n'est pas revenue.

---

## 6. Le modèle d'applications

### 6.1 Interfaces

Celles du §8.1 du document maître, avec `CloseCheck` précisé :

```cpp
struct CloseCheck {
  bool allowed = true;
  std::string question;                       // affichée par la modale si !allowed
  static CloseCheck allow() { return {}; }
  static CloseCheck ask(std::string q) { return {false, std::move(q)}; }
};
```

`spawn()` n'entre pas dans `Platform` à ce jalon : aucune application factice ne lance de processus.

### 6.2 Les deux applications factices

**Bloc** — l'application de dessin. Elle affiche sa propre taille, un curseur déplaçable aux flèches et à la souris, et un titre qu'elle change elle-même par `Host::set_title()`. Elle **compte ses appels à `on_resize()`** et l'affiche : c'est ce compteur qui verrouille la politique « un seul `on_resize` par geste » du §5.4, sans PTY. Après la première frappe, elle se déclare « modifiée » et son `can_close()` renvoie `CloseCheck::ask("Bloc a des modifications non enregistrées.")` — ce qui donne à la modale une raison d'exister et un test.

Elle exerce : `render`, `on_key`, `on_mouse`, `on_resize`, `wants_cursor`, `min_size`, `set_title`, `can_close`.

**Battement** — l'application propriétaire d'un descripteur. Elle crée un tuyau, enregistre l'extrémité de lecture par `Host::watch()`, affiche un compteur incrémenté à chaque `on_io()`, et renvoie `IoStatus::Closed` quand l'autre extrémité est fermée. Une entrée de menu écrit dans le tuyau ; une autre le ferme.

Elle exerce : `watch`/`unwatch`, les clés générationnelles, l'ordre `unwatch` puis `close`, la règle drain-puis-mort sur `EPOLLHUP`, et `request_close()`.

Ces deux applications ne sont **pas** du code jetable : elles restent au catalogue après le jalon 3 comme banc d'essai du modèle d'applications, et ce sont elles qui font que l'ajout d'une cinquième application ne touche qu'une ligne de catalogue.

### 6.3 Cycle de vie

Les trois invariants du §8.2 du document maître, dans leur forme implémentable :

1. **Aucun descripteur ne survit à son application.** `HostImpl` tient la liste des descripteurs enregistrés par fenêtre ; la destruction retire tous les descripteurs de l'`epoll` **avant** d'appeler `~App()`, dans l'ordre `unwatch` puis `close`.
2. **Aucune fenêtre n'est détruite pendant la distribution d'un événement.** Toute demande de fermeture — `request_close()`, clic sur `[×]`, `<leader>w`, `IoStatus::Closed`, exception dans un rappel — entre dans une file `pending_close_`, drainée **en fin de tour de boucle**, après le traitement de tous les descripteurs prêts.
3. **Drain-puis-mort.** Un `EPOLLHUP` ne libère pas le descripteur : on lit jusqu'à `EAGAIN` ou `EIO`, on livre, et seulement ensuite on retire et on ferme.

Une demande de fermeture reçue pendant un glissement **annule d'abord le glissement**, puis est traitée. Sans cette précédence, la machine à états garderait un `WindowId` mort.

---

## 7. Le shell du bureau

### 7.1 Le panneau

`Rect desktop = ecran - panel.rect()` (§6.1 du document maître). Épaisseur par défaut : **1 ligne** en haut ou en bas, **16 colonnes** à gauche ou à droite.

Disposition horizontale : `☰ │ épinglées │ tâches … │ horloge`. Disposition verticale : menu en haut, épinglées, séparateur, tâches, horloge et date en bas.

**Débordement** : les libellés rétrécissent avec élision jusqu'à un plancher de 8 cellules ; en dessous, le surplus se replie dans un bouton `»N` qui ouvre la liste complète. Rien ne disparaît jamais silencieusement.

**Épinglées** : cliquer une épinglée déjà ouverte lui donne le focus au lieu d'en lancer une seconde ; clic milieu pour forcer une instance supplémentaire. Par défaut, les deux applications factices sont épinglées.

`●` marque la fenêtre active, `_` une fenêtre minimisée. Cliquer une minimisée la restaure ; cliquer l'active la minimise.

En mode `Fullscreen`, le panneau est escamoté et `desktop` occupe l'écran entier.

### 7.2 Le menu

Superposition ancrée au `☰`, dessinée au-dessus de tout sauf la modale, avec un champ de recherche qui filtre à la frappe. Flèches et `Entrée` pour choisir, `Échap` pour fermer.

Contenu au jalon 2 : les entrées du catalogue (Bloc, Battement), les quatre bords du panneau (`Panneau : bas / gauche / haut / droite`), les deux commandes de Battement (écrire dans le tuyau, fermer le tuyau), et « Quitter la session ».

Les entrées de bord de panneau tiennent lieu de rechargement à chaud tant que le parseur INI n'existe pas — et démontrent la propriété qui compte : **basculer de bas à gauche ne perd aucune fenêtre**.

### 7.3 L'horloge

Alimentée par le `timerfd` d'une seconde déjà en place, elle ne salit l'écran que si la **chaîne formatée** a changé. En panneau vertical, la date passe sur une seconde ligne. Format codé en dur au jalon 2 (`%H:%M`, `%a %d %b`), lu depuis la configuration plus tard.

### 7.4 Le dialogue modal

La seule surface modale du produit. Centrée sur la zone de travail, dessinée par-dessus tout, elle **capte l'intégralité du clavier et du hit-test** tant qu'elle est ouverte.

```
╭─ Fermer Bloc ? ──────────────────────╮
│ Bloc a des modifications non         │
│ enregistrées.                        │
│                                      │
│              [ Annuler ]  [ Fermer ] │
╰──────────────────────────────────────╯
```

`Échap` et `Entrée` valent tous deux **Annuler** : le bouton par défaut est le geste sûr, jamais le destructeur. `Tab` déplace la sélection, `Espace` active le bouton sélectionné, le clic agit directement.

Une seule modale à la fois. Une demande d'ouverture alors qu'une modale est déjà affichée est ignorée — pas empilée : une pile de modales est un état dont l'utilisateur ne peut pas se sortir de façon prévisible.

---

## 8. Thème et bordures

```cpp
struct Theme {
  Color desktop_bg, panel_bg, panel_fg, accent,
        title_focus_bg, title_focus_fg, title_blur_bg, title_blur_fg,
        border_focus, border_blur, modal_bg, modal_fg;
  static Theme defaults();                        // écrit une seule fois, en RGB
  Theme quantized(const OutputProfile&) const;    // -> 256 ou 16 couleurs
};

enum class Border { Unicode, Ascii };             // choisi par OutputProfile::utf8
void View::box(Rect r, Border b, Style st);
```

Le thème est écrit **une seule fois en RGB** et quantifié à l'attache selon le profil du client (§4.4 du document maître). `Border::Ascii` remplace les semi-graphiques par `+`, `-`, `|` quand le client n'annonce pas UTF-8 — la poignée `◢` devient `#`.

Fenêtre focalisée : barre de titre en couleur d'accent, bordure vive. Hors focus : tout est atténué. Aucune ambiguïté sur qui reçoit le clavier.

---

## 9. Raccourcis

La table du §7.4 du document maître, en entier, moins les entrées qui n'ont pas de sens sans PTY (`Shift+PgUp`/`PgDn`, scrollback).

`LeaderDispatch` ne connaît pas le gestionnaire de fenêtres : elle traduit des touches en intentions, `Session` les exécute.

```cpp
enum class Action {
  MoveLeft, MoveRight, MoveUp, MoveDown,
  GrowWidth, ShrinkWidth, GrowHeight, ShrinkHeight,
  NextWindow, PrevWindow,
  Close, Minimize, MaximizeToggle, FullscreenToggle,
  OpenMenu, ToggleMouse, ForceRepaint,
  LiteralLeader,
};

class LeaderDispatch {
 public:
  explicit LeaderDispatch(char32_t leader);       // Ctrl+A par défaut
  std::optional<Action> feed(const KeyEvent&);    // nullopt = touche à transmettre
  bool armed() const;
};
```

Deux états seulement : au repos et leader armé. `<leader><leader>` désarme et fait transmettre l'octet leader littéral à l'application — sans quoi `Ctrl+A` deviendrait inaccessible dans tous les shells du jalon 3. Une touche inconnue après le leader désarme et **n'émet rien**.

`ToggleMouse` et `ForceRepaint` n'ont pas d'effet sur le WM, et — vérifié dans `src/common/proto.hpp` — **aucun des deux n'exige de nouveau message de protocole** :

- `ForceRepaint` appelle `Differ::invalidate()`, qui existe depuis le jalon 1 et est déjà emprunté au `Resize` et au débordement de contre-pression. La frame suivante est un repaint complet.
- `ToggleMouse` émet la paire `\033[?1002l\033[?1006l` (ou `h`) **dans le flux de frames**. Le client écrit `FrameMsg::ansi` verbatim sur son terminal : c'est le mécanisme normal, pas un détournement. Le `TtyGuard` restaure de toute façon les deux modes à la sortie, quel que soit l'état laissé par la bascule.

---

## 10. `Session` recomposée, et flux d'une frame

```
InputEvent
  ├─ modale ouverte ?      -> elle capte tout, rien ne passe derrière
  ├─ LeaderDispatch::feed  -> Action ?  -> exécutée sur le WM
  ├─ souris                -> hit-test : modale, menu, panneau, fenêtres (avant -> arrière)
  └─ sinon                 -> App focalisée, en coordonnées locales

événement de descripteur
  └─ HostImpl : clé -> {window, fd} ; clé absente -> jetée
     -> App::on_io(fd, events) -> Closed ? -> file de fermetures

fin de tour de boucle
  ├─ drainer pending_close_    unwatch -> close -> ~App -> retrait de la fenêtre
  └─ si dirty et >= 33 ms : composer
        fond -> fenêtres (arrière -> avant) -> panneau -> menu -> modale
     -> diff -> socket
```

`Session` conserve son interface publique actuelle (`resize`, `on_input`, `render`, `wants_quit`) et gagne deux points d'entrée : `on_fd_event(uint64_t key, uint32_t events)` et `tick()` (horloge et chien de garde de glissement).

---

## 11. Erreurs

| Panne | Réponse |
|---|---|
| Exception dans un rappel d'application | Fenêtre « plantée », message affiché à la place du contenu, `App` détruite par la file de fermetures, démon vit |
| `on_io` renvoie `Closed` | Fermeture différée en fin de tour, jamais en cours de lot |
| Clé d'événement absente de la table | Événement jeté sans bruit — définition d'un événement périmé |
| 65ᵉ fenêtre demandée | Lancement refusé, notification dans le panneau |
| Zone de travail sous 40×12 | Écran « terminal trop petit », état du bureau conservé intact |
| Fermeture demandée pendant un glissement | Glissement annulé d'abord, puis fermeture |
| Modale demandée alors qu'une modale est ouverte | Demande ignorée, pas empilée |

---

## 12. Tests

### 12.1 Six familles

1. **Unitaires purs**, sans démon ni socket : réversibilité `user_rect → display_rect` sur l'aller-retour 160×50 → 80×24 → 160×50 ; hit-test de chaque cible ; position des boutons à toutes les largeurs, plancher compris ; panneau aux quatre bords avec débordement, élision et repli `»N` ; filtrage du menu ; `LeaderDispatch` (leader armé, `<leader><leader>`, touche inconnue, séquence interrompue) ; quantification du thème vers 256 et 16 ; `View::box` dans les deux profils de bordure.

2. **Propriété — le hit-test est l'inverse exact de la composition.** On compose une scène, on note pour chaque cellule quel élément l'a peinte en dernier, puis on interroge `Session::hit()` sur chaque cellule de l'écran et on exige la correspondance. Cette propriété tue par construction toute la famille « je clique la barre des tâches et c'est la fenêtre derrière qui répond », et elle reste vraie quand on ajoutera des éléments.

   Exigence d'implémentation qui en découle, à ne pas découvrir en cours de route : **la composition doit pouvoir tourner en mode « relevé de propriétaire »**, où chaque écriture de cellule enregistre en parallèle l'élément qui l'a produite. Concrètement, `Session::compose()` accepte un pointeur optionnel vers une grille de propriétaires, nul en production, fourni par le test. Aucun coût quand il est nul.

3. **Cycle de vie**, avec un `FdRegistrar` double : `unwatch` précède toujours `close` ; une clé générationnelle périmée est reconnue et jetée ; aucune fenêtre n'est détruite pendant la distribution d'un lot d'événements ; drain-puis-mort sur `EPOLLHUP`.

4. **Gestes scriptés** sur `Session`, avec `Platform` et `FdRegistrar` doubles : une suite d'événements d'entrée, puis des assertions de propriété sur la `Surface` composée (`text_row`, `cell().fg`). C'est l'équivalent jalon 2 du test phare du §13.5 du document maître.

5. **Exactement un `on_resize()` par geste de redimensionnement**, quel que soit le nombre de rapports de mouvement intermédiaires — le compteur de Bloc l'atteste.

6. **Goldens** : six à dix fichiers, en 60×20, profil de bordures ASCII, la couche couleur dans un fichier séparé du texte. `UPDATE_GOLDEN=1` **imprime le diff avant de régénérer**.

### 12.2 Règle de discrimination

Reprise du round `EPOLLHUP` soldé au commit `4aa774f`, et non négociable sur ce jalon :

> **Chaque garde ajoutée a un test qui échoue sans elle**, vérifié en retirant réellement la garde et en observant l'échec.

Elle vise en particulier les **sept chemins d'annulation de glissement** : un test par chemin, tous asseyant la même géométrie finale. Une garde qu'aucun test ne discrimine doit être déclarée telle dans le rapport de tâche, jamais couverte par un test complaisant.

Corollaire mesuré sur ce même round : un test censé discriminer est exécuté **10 à 20 fois** contre le code d'avant, jamais une seule — une exécution unique ne distingue pas « discriminant » de « discriminant une fois sur deux ».

---

## 13. Ordre de construction

Squelette vertical puis épaisseur : la première tâche traverse toute la pile dans sa version la plus mince, et **chaque tâche suivante laisse un bureau qui fonctionne**. Les questions d'intégration douloureuses — qui possède le hit-test, qui invalide, qui détruit — se paient au premier jour plutôt qu'au dernier.

| # | Tâche | Ce qu'on voit à la fin |
|---|---|---|
| 1 | `Theme`, `View::box`, interfaces `App`/`Host`, catalogue, **Bloc**, `Window`, `decor`, `Session` recomposée | Une fenêtre décorée contenant une vraie application, un panneau statique, à travers SSH |
| 2 | `hittest`, machine à états de glissement, contour élastique, sept chemins d'annulation, un `on_resize` par geste | La fenêtre se déplace et se redimensionne à la souris |
| 3 | z-order, focus, cascade, aimantation, plafond 64, boutons `[_][□][×]`, modes `Maximized` / `Fullscreen` / `minimized` | Plusieurs fenêtres, empilées et focalisables |
| 4 | `layout` : `user_rect` → `display_rect`, réversibilité, « terminal trop petit » | La disposition survit à un aller-retour 160×50 → 80×24 |
| 5 | `HostImpl`, clés générationnelles, `FdRegistrar`, fermetures différées, drain-puis-mort, **Battement** | Une application qui réagit à un descripteur |
| 6 | Panneau complet : quatre bords, disposition H/V, tâches, épinglées, débordement, horloge | La barre des tâches, basculable |
| 7 | Menu : superposition, recherche, filtrage, entrées de bord de panneau | On lance des applications depuis le menu |
| 8 | `LeaderDispatch` et exécution des `Action` | Tout le geste au clavier, plein écran compris |
| 9 | Modale, `can_close`, goldens, revue de jalon | La confirmation de fermeture, et le jalon complet |

---

## 14. Ce que ce jalon ne fait pas

Il ne lance aucun processus, n'ouvre aucun PTY, n'interprète aucune séquence VT venant d'un invité. Les fenêtres contiennent deux applications factices dont la raison d'être est d'exercer le modèle, pas de rendre un service.

Il ne lit aucun fichier de configuration : le bord du panneau et la touche leader sont modifiables à l'exécution, pas persistés.

Il n'implémente ni les accords `Alt`, ni l'aide de découvrabilité du `<leader>` seul, ni le scrollback.

À la fin du jalon 2, le bureau est complet et le geste entier est démontrable — il manque ce qu'on met dedans, et c'est exactement l'objet du jalon 3.
