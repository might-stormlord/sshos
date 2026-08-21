#!/bin/sh
# Verification, application et retour arriere de la mise a jour de ssh_os.
#
# LE DEMON NE FAIT QUE LIRE CE QUE CE SCRIPT ECRIT. C'est ce qui garde git,
# cmake et le reseau hors de son fil unique.
#
# usage : termos-update --check | --apply | --rollback
#
# Conception : docs/superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md
set -eu

# L'URL du depot. Surchargeable pour la meme raison que boot_id_path dans
# net.hpp : sans ca, la sonde bout-en-bout devrait parler au vrai GitHub et
# compiler le vrai projet a chaque cas -- des minutes par verdict, et un
# reseau dans la boucle d'un test.
REPO_URL="${TERMOS_REPO_URL:-https://github.com/might-stormlord/termos.git}"
API="https://api.github.com/repos/might-stormlord/termos"
CODELOAD="https://codeload.github.com/might-stormlord/termos"

# Par defaut curl suit une redirection https -> http, et les URL d'assets
# GitHub redirigent. On l'interdit.
CURL_OPTS="--proto =https --proto-redir =https --tlsv1.2 -fsSL"

# Une verification est un aller-retour reseau ; une application compile et
# passe la suite complete. Sans ces bornes, « en cours » est un etat dont on
# ne sort jamais.
CHECK_TIMEOUT=60
APPLY_TIMEOUT=1800

# --- ou l'on vit ----------------------------------------------------------
# Le prefixe se deduit du chemin du script -- <prefixe>/libexec/termos-update --
# et jamais de ~ en dur : c'est ce qui permet a la sonde de ne pas ecraser
# l'installation reelle.
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX="${TERMOS_PREFIX:-$(dirname -- "$SELF")}"

# L'etat est propre a l'UTILISATEUR, pas a l'installation.
STATE_DIR="${TERMOS_STATE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/termos}"
STATE="$STATE_DIR/state"
LOCK="$STATE_DIR/lock"
SRC="$STATE_DIR/src"
GOLDEN="$STATE_DIR/golden"

EXE="$PREFIX/libexec/termos"
UPDATER="$PREFIX/libexec/termos-update"
VERSIONER="$PREFIX/libexec/termos-version"

mkdir -p "$STATE_DIR" "$PREFIX/libexec"

have() { command -v "$1" >/dev/null 2>&1; }

# --- lecture de l'etat ----------------------------------------------------
# La PREMIERE occurrence gagne, comme cote C++ : une ligne ajoutee apres coup
# ne doit pas ecraser une valeur deja lue.
get() { # get <cle>
  [ -f "$STATE" ] || return 0
  sed -n "s/^$1=\\(.*\\)$/\\1/p" "$STATE" | head -1
}

INSTALLED_VERSION=$(get installed_version)
REMOTE_VERSION=$(get remote_version)
COMMITS_AHEAD=$(get commits_ahead)
# Ou en est le travail. Le demon le lit et le montre : une fenetre qui dit
# « en cours » pendant deux minutes sans rien preciser laisse croire a un
# blocage.
STAGE=""
# OU EN EST LE TRAVAIL, EN POUR CENT, ou vide quand on ne sait pas. Cinq
# libelles d'etape couvraient une a deux minutes d'attente : « compilation »
# restait affiche sans bouger, et rien ne distinguait un travail qui avance
# d'un travail bloque. On ne l'invente pas : cmake ecrit deja son propre
# pourcentage, et la suite de tests une ligne par cas. Le C++ ne calcule
# rien, il lit -- et refuse tout ce qui n'est pas un entier de 0 a 100.
PROGRESS=""
# Les paliers du travail entier. Ils sont un CHOIX, pas une mesure fine : la
# compilation et la suite pesent ensemble la quasi-totalite du temps (une a
# deux minutes contre quelques secondes pour tout le reste), et c'est la
# seule chose que ce decoupage doit respecter. Le chiffre ne recule jamais.
P_SOURCES=5
P_BUILD=10
P_TESTS=75
P_INSTALL=95
SOURCE=$(get source);            [ -n "$SOURCE" ] || SOURCE=git
INSTALLED=$(get installed_commit); [ -n "$INSTALLED" ] || INSTALLED=unknown
PREVIOUS=$(get previous_commit)
CHECKED=$(get checked_at);       [ -n "$CHECKED" ] || CHECKED=0
REMOTE=$(get remote_commit)
# LE FAIT, SEPARE DE LA CONCLUSION.
#
# `status` porte la CONCLUSION de la dernière vérification -- à jour,
# disponible, échouée... `restart_pending` porte un FAIT tout autre : un
# binaire est posé et ce n'est pas celui qui tourne. Les avoir confondus dans
# une seule clé rendait le fait effaçable par n'importe quelle conclusion, et
# la vérification quotidienne le faisait en silence.
#
# MIGRATION : un fichier écrit par un script antérieur n'a pas la clé, mais
# son `status=restart-pending` dit exactement la même chose. On la reconstruit
# plutôt que de perdre un redémarrage en attente au moment de la mise à jour
# qui introduit la clé.
STATUT_AVANT=$(get status)
RESTART_PENDING=$(get restart_pending)
if [ -z "$RESTART_PENDING" ] && [ "$STATUT_AVANT" = restart-pending ]; then
  RESTART_PENDING=1
fi
[ "$RESTART_PENDING" = 1 ] || RESTART_PENDING=""

# LE SEUL ENDROIT QUI PUISSE CONSTATER QUE LE REDEMARRAGE A EU LIEU.
#
# Le démon nous a lancés par un `fork()` SIMPLE suivi d'un `execv` (voir
# `launch_updater`, src/daemon/session.cpp) : notre parent EST donc le démon,
# et comparer l'inode de son binaire à celle du binaire posé dit si c'est bien
# lui qui tourne. `stat -L` déréférence le lien magique -- sans le `-L` on
# mesurerait `/proc/PID/exe` lui-même, qui vit sur procfs, et la comparaison
# serait toujours fausse (§9 du dossier de reprise).
#
# Tapé à la main depuis un shell, notre parent n'est pas un démon : on ne
# conclut pas, et le fait est conservé. C'est le sens sûr de l'incertitude --
# garder un redémarrage en attente ne coûte qu'une proposition de trop, le
# perdre coûte un bureau qui tourne sur l'ancien binaire sans le dire.
redemarrage_fait() {
  [ -n "$RESTART_PENDING" ] || return 1
  [ -r "/proc/$PPID/exe" ] || return 1
  _parent=$(stat -Lc '%d:%i' "/proc/$PPID/exe" 2>/dev/null) || return 1
  _pose=$(stat -c '%d:%i' "$EXE" 2>/dev/null) || return 1
  [ "$_parent" = "$_pose" ]
}

# --- ecriture de l'etat ---------------------------------------------------
# UN SEUL RETOUR A LA LIGNE DANS UN MESSAGE FORGERAIT UNE PAIRE CLE=VALEUR.
# Le §8 verse ici le resume d'une compilation cassee : l'assainissement est
# donc obligatoire, et il se fait chez l'ECRIVAIN.
sanitize() {
  printf '%s' "$1" | tr -d '\000-\037\177' | cut -c1-200
}

# CE QUI A CHANGE, ET PAS SEULEMENT COMBIEN.
#
# « 5 nouveautes » ne dit pas LESQUELLES, et c'est justement ce qu'on veut
# savoir avant de lancer une compilation de deux minutes. Le C++ n'a pas git
# et ne l'aura jamais -- c'est la contrainte qui garde le demon vivant -- donc
# c'est ici, et seulement ici, qu'on lit l'historique.
#
# On ne garde que « feat » et « fix » : docs, test et ci sont du travail
# interne, et les lister noierait ce qui se voit. Le prefixe « type(portee): »
# part, il ne dit rien a l'utilisateur.
#
# Six au plus, la septieme ligne comptant le reste. Chacune est tronquee a 76
# caracteres, comme kMaxUpdateNoteChars du cote C++ -- une modale qui deborde
# de son cadre a deja ete un defaut de ce projet -- et le fichier d'etat
# entier doit rester sous kMaxStateBytes, faute de quoi il est lu comme
# vierge.
NOTES=""

build_notes() { # build_notes <installe> <distant>
  NOTES=""
  [ "$SOURCE" = git ] || return 0
  have git || return 0
  [ -d "$SRC/.git" ] || return 0
  [ "$1" != unknown ] || return 0

  # LA FIN DE LIGNE SURVIT AU MENAGE, et c'est tout sauf un detail : `tr -d
  # '\000-\037\177'` -- la plage qu'utilise sanitize() plus haut, sur une
  # valeur d'UNE ligne -- emporte aussi \012, donc colle tous les sujets en
  # un seul que `cut` tronque ensuite a 76 caracteres. Vu pour de vrai en
  # essayant cette fonction. On garde donc \012 et on retire le reste.
  _sujets=$(git -C "$SRC" log --format=%s "$1..$2" 2>/dev/null \
            | grep -E '^(feat|fix)\(' \
            | sed -E 's/^[a-z]+\([^)]*\): *//' \
            | tr -d '\000-\011\013-\037\177' \
            | cut -c1-76) || return 0
  [ -n "$_sujets" ] || return 0

  _total=$(printf '%s\n' "$_sujets" | wc -l | tr -d ' ')
  _rang=0
  # `sed -n Np` plutot qu'une boucle `read` : un `while read` derriere un
  # tube tourne dans un SOUS-SHELL, et tout ce qu'il assignerait a NOTES
  # serait perdu au retour. Piege classique de sh, et il ne se voit pas.
  # LA LIGNE DE RESTE COMPTE DANS LE PLAFOND. Le lecteur en garde six et
  # s'arrete la : en ecrire six PUIS un « ... et N autres » en septieme
  # position ferait jeter precisement la ligne qui explique qu'il en manque.
  # Donc six sujets quand il y en a six ou moins, cinq plus le reste sinon.
  _garde=6
  [ "$_total" -le 6 ] || _garde=5

  _n=0
  while [ "$_n" -lt "$_garde" ]; do
    _n=$((_n + 1))
    _s=$(printf '%s\n' "$_sujets" | sed -n "${_n}p")
    [ -n "$_s" ] || break
    _rang=$_n
    NOTES="${NOTES}note_$_n=$_s
"
  done
  if [ "$_total" -gt "$_rang" ] && [ "$_rang" -gt 0 ]; then
    _reste=$((_total - _rang))
    _mot=autres
    [ "$_reste" -gt 1 ] || _mot=autre
    NOTES="${NOTES}note_$((_rang + 1))=... et $_reste $_mot
"
  fi
}

# WSUF distingue le fichier temporaire du surveillant de progression de
# celui du script principal. Les deux ecrivent le meme etat pendant une
# compilation ; s'ils partageaient « .tmp », l'un renommerait le fichier a
# moitie ecrit par l'autre.
WSUF="tmp"
write_state() { # write_state <status> <message> [pid]
  _st="$1"; _msg=$(sanitize "${2:-}"); _pid="${3:-}"
  # Le fichier temporaire est dans le MEME repertoire, donc le meme systeme
  # de fichiers : c'est ce qui rend le rename atomique.
  cat > "$STATE.$WSUF" <<FIN
schema=1
prefix=$PREFIX
source=$SOURCE
installed_commit=$INSTALLED
previous_commit=$PREVIOUS
remote_commit=$REMOTE
installed_version=$INSTALLED_VERSION
remote_version=$REMOTE_VERSION
commits_ahead=$COMMITS_AHEAD
stage=$STAGE
progress=$PROGRESS
checked_at=$CHECKED
status=$_st
restart_pending=$RESTART_PENDING
pid=$_pid
message=$_msg
FIN
  # LES NOTES NE VALENT QUE POUR « available » : ailleurs elles decriraient
  # une mise a jour qui n'est plus a venir -- deja posee, ou echouee -- et un
  # fichier qui les garderait ferait mentir la prochaine lecture.
  if [ "$_st" = available ] && [ -n "$NOTES" ]; then
    printf '%s' "$NOTES" >> "$STATE.$WSUF"
  fi
  mv -f "$STATE.$WSUF" "$STATE"
}

fail() { # fail <status> <message>
  write_state "$1" "$2"
  printf 'termos-update: %s\n' "$2" >&2
  exit 1
}

# UNE VERIFICATION NE DEMENT PAS UN REDEMARRAGE EN ATTENTE.
#
# `restart-pending` n'est pas une conclusion, c'est un fait sur le disque : un
# binaire est posé et ce n'est pas celui qui tourne. Deux acteurs seulement
# peuvent l'effacer -- un `--apply`, qui repose un binaire et le réarme, et le
# démon, qui compare les inodes (`running_is_installed()`). Une vérification,
# elle, ne regarde que le dépôt distant.
#
# Sans cette règle, la vérification automatique -- une fois par jour, sans que
# personne ait rien demandé -- écrivait « up-to-date » par-dessus : la pastille
# s'éteignait, l'entrée redevenait « Verifier les mises a jour », et plus rien
# ne proposait le redémarrage alors que le binaire posé n'était toujours pas
# celui qui tournait. Le trou se refermait tout seul, en silence.
#
# Les conclusions qui PROPOSENT quelque chose, elles, gagnent : `available` et
# `history-rewritten` laissent une entrée actionnable, et l'application qui
# suivra reposera un binaire de toute façon.
conclure() { # conclure <status> <message> [pid]
  # TANT QUE LE FAIT TIENT, `status` LE REDIT. La clé `restart_pending`
  # suffirait à un démon récent, mais un démon ANTERIEUR à cette clé ne lit
  # que `status` : lui écrire « à jour » alors qu'un binaire posé attend son
  # redémarrage lui ferait éteindre la pastille pour toujours. C'est la
  # compatibilité qui l'impose, et elle ne coûte rien -- les deux disent la
  # même chose.
  if [ -n "$RESTART_PENDING" ]; then
    case "$1" in
      checking|up-to-date|check-failed)
        write_state restart-pending "redemarrez pour terminer" "${3:-}"
        return 0 ;;
    esac
  fi
  write_state "$1" "$2" "${3:-}"
}

# Comme fail(), mais sans effacer le fait ci-dessus. Le réseau qui tombe est
# le cas le plus probable des deux, et c'est celui qui ne compare même pas.
fail_check() { # fail_check <message>
  conclure check-failed "$1"
  printf 'termos-update: %s\n' "$1" >&2
  exit 1
}

# --- le verrou ------------------------------------------------------------
# Il couvre TOUTE la sequence : rotation du binaire, pose, ecriture d'etat.
# Sans lui, deux applications concurrentes font que termos.previous finit par
# contenir le NOUVEAU binaire, et --rollback restaure alors la version
# cassee : le filet de securite detruit par la course qu'il devait rattraper.
if have flock; then
  exec 9>"$LOCK"
  flock -n 9 || { echo "termos-update: un autre travail est en cours" >&2; exit 1; }
fi

fetch_stdout() {
  if have curl; then curl $CURL_OPTS "$1"
  elif have wget; then wget --https-only -q -O - "$1"
  else return 1
  fi
}
fetch_to() {
  if have curl; then curl $CURL_OPTS -o "$2" "$1"
  elif have wget; then wget --https-only -q -O "$2" "$1"
  else return 1
  fi
}

# --- l'arbre de compilation ----------------------------------------------
# Reutilise SEULEMENT s'il est bien ce qu'on croit : une installation par
# archive suivie de l'apparition de git donnerait un src/ sans .git, et
# git fetch y echouerait indefiniment.
ensure_git_tree() {
  if [ -d "$SRC/.git" ] && \
     [ "$(git -C "$SRC" remote get-url origin 2>/dev/null || true)" = "$REPO_URL" ]; then
    timeout "$CHECK_TIMEOUT" git -C "$SRC" fetch --quiet origin main || return 1
  else
    rm -rf "$SRC" || return 1
    timeout "$APPLY_TIMEOUT" git clone --quiet "$REPO_URL" "$SRC" || return 1
  fi
}

# « 1.12 » plutot que « cce9d11 ». Le calcul vit dans un seul endroit,
# termos-version, que l'installeur pose a cote de ce script.
version_of() { # version_of <arbre> <ref>
  [ -x "$VERSIONER" ] || return 1
  "$VERSIONER" "$1" "$2" 2>/dev/null
}

remote_head() {
  case "$SOURCE" in
    git)
      timeout "$CHECK_TIMEOUT" git ls-remote "$REPO_URL" refs/heads/main 2>/dev/null \
        | awk '{print $1; exit}'
      ;;
    release)
      fetch_stdout "$API/releases/latest" 2>/dev/null \
        | sed -n 's/.*"tag_name": *"commit-\([0-9a-f]*\)".*/\1/p' | head -1
      ;;
    archive)
      fetch_stdout "$API/commits/main" 2>/dev/null \
        | sed -n 's/.*"sha": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1
      ;;
    *) return 1 ;;
  esac
}

# --- --check --------------------------------------------------------------
do_check() {
  # LE FAIT SE RESOUT AVANT TOUT LE RESTE. Si notre parent est le demon et
  # qu'il tourne DEJA le binaire pose, le redemarrage a eu lieu : le fait
  # tombe, et `status` peut enfin dire la verite. C'est la seule occasion ou
  # quelqu'un puisse le constater -- le demon le sait aussi, mais le C++
  # n'ecrit jamais ce fichier.
  if redemarrage_fait; then
    RESTART_PENDING=""
  fi
  if [ "$SOURCE" = local ]; then
    write_state updates-disabled "installation locale, pas de source distante"
    return 0
  fi
  conclure checking "" "$$"

  _remote=$(remote_head || true)
  if [ -z "$_remote" ]; then
    fail_check "verification impossible : reseau ou depot injoignable"
  fi
  REMOTE="$_remote"
  CHECKED=$(date +%s)

  if [ "$INSTALLED" = unknown ]; then
    # On ne sait pas d'ou vient l'installation : aucune comparaison n'est
    # possible, et on ne pretend pas le contraire. Le service proposera une
    # reinstallation.
    write_state available "provenance inconnue"
    return 0
  fi
  # LES NUMEROS SE CALCULENT AVANT DE TRANCHER, pas seulement quand une mise
  # a jour existe : savoir « vous etes a jour, version 1.2 » vaut mieux que
  # « vous etes a jour » tout court, et c'est le meme travail.
  #
  # Ils demandent un arbre git sous la main. Sans lui ils restent vides, et
  # l'affichage retombe sur ce qu'il sait dire.
  if [ "$SOURCE" = git ] && have git && ensure_git_tree; then
    _iv=$(version_of "$SRC" "$INSTALLED" || true)
    _rv=$(version_of "$SRC" "$REMOTE" || true)
    [ -n "$_iv" ] && INSTALLED_VERSION="$_iv"
    [ -n "$_rv" ] && REMOTE_VERSION="$_rv"
    if [ "$REMOTE" != "$INSTALLED" ]; then
      COMMITS_AHEAD=$(git -C "$SRC" rev-list --count "$INSTALLED..$REMOTE" 2>/dev/null || echo "")
    else
      COMMITS_AHEAD=""
    fi
  fi

  if [ "$REMOTE" = "$INSTALLED" ]; then
    conclure up-to-date ""
    return 0
  fi

  # LE CONTROLE DE DESCENDANCE. Ce depot a force-pousse main deux fois ; sans
  # ce test, une reecriture ferait proposer une « mise a jour » vers un
  # historique sans relation avec celui en place, potentiellement plus
  # ancien.
  if [ "$SOURCE" = git ] && have git; then
    if ensure_git_tree; then
      if ! git -C "$SRC" merge-base --is-ancestor "$INSTALLED" "$REMOTE" 2>/dev/null; then
        write_state history-rewritten "historique reecrit, reinstallation necessaire"
        return 0
      fi
    fi
  fi

  build_notes "$INSTALLED" "$REMOTE"
  write_state available ""
}

# --- --apply --------------------------------------------------------------
# LA POSE : copie vers .previous, ecriture dans .new, rename. Une ecriture EN
# PLACE sur un binaire en cours d'execution rend ETXTBSY a tous les coups,
# c'est-a-dire exactement dans le cas ou l'on met a jour ; et deux « mv »
# successifs ouvrent une fenetre pendant laquelle le chemin n'existe pas.
place() { # place <binaire neuf>
  if [ -f "$EXE" ]; then
    cp "$EXE" "$EXE.previous"
  fi
  cp "$1" "$EXE.new"
  chmod 0755 "$EXE.new"
  mv -f "$EXE.new" "$EXE"
}

run_suite() { # run_suite <binaire de tests> <repertoire des references>
  TERMOS_GOLDEN_DIR="$2" timeout "$APPLY_TIMEOUT" "$1" >"$STATE_DIR/tests.log" 2>&1
}

do_apply() {
  if [ "$SOURCE" = local ]; then
    write_state updates-disabled "installation locale, pas de source distante"
    return 0
  fi
  STAGE="preparation"; PROGRESS=0
  write_state applying "" "$$"

  _tmp=$(mktemp -d)
  # PAS DE DESCENTE D'ECHELON PENDANT UNE MISE A JOUR. Un echec de l'echelon
  # inscrit dans source= donne apply-failed, jamais un repli silencieux vers
  # un canal moins controle -- sinon il suffirait de servir un binaire
  # illisible pour forcer la degradation.
  case "$SOURCE" in
    git)
      have git || { rm -rf "$_tmp"; fail apply-failed "git a disparu"; }
      STAGE="recuperation des sources"; PROGRESS=$P_SOURCES
      write_state applying "" "$$"
      ensure_git_tree || { rm -rf "$_tmp"; fail apply-failed "clone ou fetch impossible"; }
      git -C "$SRC" checkout --quiet -B main origin/main \
        || { rm -rf "$_tmp"; fail apply-failed "checkout impossible"; }
      _new=$(git -C "$SRC" rev-parse HEAD)
      build_and_test_tree "$SRC" "$_tmp" || { rm -rf "$_tmp"; exit 1; }
      _bin="$SRC/build-release/sshos"
      _refs="$SRC/tests/golden"
      ;;
    archive)
      _new=$(fetch_stdout "$API/commits/main" 2>/dev/null \
             | sed -n 's/.*"sha": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1)
      [ -n "$_new" ] || { rm -rf "$_tmp"; fail apply-failed "sha distant introuvable"; }
      fetch_to "$CODELOAD/tar.gz/$_new" "$_tmp/src.tar.gz" \
        || { rm -rf "$_tmp"; fail apply-failed "telechargement impossible"; }
      rm -rf "$SRC" && mkdir -p "$SRC"
      tar xzf "$_tmp/src.tar.gz" -C "$SRC" --strip-components=1 \
        || { rm -rf "$_tmp"; fail apply-failed "archive illisible"; }
      build_and_test_tree "$SRC" "$_tmp" || { rm -rf "$_tmp"; exit 1; }
      _bin="$SRC/build-release/sshos"
      _refs="$SRC/tests/golden"
      ;;
    release)
      _json="$_tmp/rel.json"
      fetch_to "$API/releases/latest" "$_json" \
        || { rm -rf "$_tmp"; fail apply-failed "release injoignable"; }
      _new=$(sed -n 's/.*"tag_name": *"commit-\([0-9a-f]*\)".*/\1/p' "$_json" | head -1)
      _base=$(sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' "$_json" | head -1)
      [ -n "$_base" ] || { rm -rf "$_tmp"; fail apply-failed "aucun asset publie"; }
      _dir=$(dirname "$_base")
      for f in sshos sshos_tests golden.tar.gz SHA256SUMS; do
        fetch_to "$_dir/$f" "$_tmp/$f" \
          || { rm -rf "$_tmp"; fail apply-failed "$f absent de la release"; }
      done
      have sha256sum || { rm -rf "$_tmp"; fail apply-failed "sha256sum absent"; }
      ( cd "$_tmp" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) \
        || { rm -rf "$_tmp"; fail apply-failed "SHA256 non concordant"; }
      chmod 0755 "$_tmp/sshos" "$_tmp/sshos_tests"
      mkdir -p "$_tmp/refs" && tar xzf "$_tmp/golden.tar.gz" -C "$_tmp/refs"
      run_suite "$_tmp/sshos_tests" "$_tmp/refs/golden" \
        || { rm -rf "$_tmp"; fail apply-failed "la suite de tests echoue"; }
      _bin="$_tmp/sshos"
      _refs="$_tmp/refs/golden"
      ;;
    *) rm -rf "$_tmp"; fail apply-failed "source inconnue : $SOURCE" ;;
  esac

  # Les references suivent le binaire : sans elles, un sshos_tests publie
  # chercherait le repertoire de la machine qui l'a compile.
  STAGE="installation"; PROGRESS=$P_INSTALL; write_state applying "" "$$"
  rm -rf "$GOLDEN" && cp -r "$_refs" "$GOLDEN"

  place "$_bin"
  # Le script se remplace LUI AUSSI : un script fige piloterait un binaire
  # qui a evolue. Et l'outil de version avec lui -- l'oublier laissait
  # installed_version vide apres chaque mise a jour, donc « cce9d11 ->
  # 3512ffe » au lieu de « 1.1 -> 1.2 », alors que tout etait en place pour
  # l'afficher.
  if [ -f "$SRC/tools/update.sh" ]; then
    cp "$SRC/tools/update.sh" "$UPDATER.new" && chmod 0755 "$UPDATER.new" \
      && mv -f "$UPDATER.new" "$UPDATER"
  fi
  if [ -f "$SRC/tools/version.sh" ]; then
    cp "$SRC/tools/version.sh" "$VERSIONER.new" && chmod 0755 "$VERSIONER.new" \
      && mv -f "$VERSIONER.new" "$VERSIONER"
  fi

  PREVIOUS="$INSTALLED"
  INSTALLED="$_new"
  REMOTE="$_new"
  if [ -d "$SRC/.git" ]; then
    _nv=$(version_of "$SRC" "$_new" || true)
    [ -n "$_nv" ] && INSTALLED_VERSION="$_nv" && REMOTE_VERSION="$_nv"
  fi
  COMMITS_AHEAD=""
  CHECKED=$(date +%s)
  rm -rf "$_tmp"
  # LE FAIT EST ARME ICI, ET NULLE PART AILLEURS. Le binaire pose n'est pas
  # celui qui tourne : annoncer « a jour » eteindrait la pastille alors que
  # l'utilisateur continue sur l'ancienne version. `status` le redit pour les
  # demons anterieurs a la cle, qui ne lisent que lui.
  STAGE=""; PROGRESS=""; RESTART_PENDING=1
  write_state restart-pending "redemarrez pour terminer"
}

# --- la progression, telle que les outils la disent deja -------------------
#
# On n'invente aucun chiffre. `cmake --build` ecrit « [ 57%] » sur chacune de
# ses lignes, et le lanceur de tests une ligne « - <nom> » par cas -- le
# total des cas se compte sur l'arbre qu'on vient de sortir. Le surveillant
# ne fait que lire ces deux traces et les replier sur l'echelle du travail
# entier.
#
# IL TOURNE EN FOND, et c'est la seule facon de le faire sans changer le
# code de retour de ce qu'il surveille : mettre cmake dans un tube ferait
# repondre le tube a sa place, et `pipefail` n'existe pas en sh POSIX.
pourcentage_compilation() { # <journal>
  sed -n 's/^\[ *\([0-9]\{1,3\}\)%\].*/\1/p' "$1" 2>/dev/null | tail -1
}

pourcentage_tests() { # <journal> <total>
  [ "${2:-0}" -gt 0 ] || return 0
  _faits=$(grep -c '^- ' "$1" 2>/dev/null) || _faits=0
  echo $((_faits * 100 / $2))
}

# Combien de cas la suite de CET arbre declare. Le compte sert a DESSINER une
# barre, jamais a juger la suite : le critere reste le code de retour, parce
# qu'un total perime a chaque commit qui ajoute un cas.
total_des_cas() { # <racine>
  # `-h` PLUTOT QU'UN DECOUPAGE SUR « : ». `grep -c` ne prefixe le nom du
  # fichier que lorsqu'il en recoit PLUSIEURS : sur un seul, il imprime le
  # compte nu, et un `awk -F: '{s+=$2}'` additionne alors du vide. Le total
  # tombe silencieusement a zero -- donc aucune barre -- et rien ne le dit.
  # Avec `-h`, chaque fichier rend son compte nu, qu'il y en ait un ou
  # soixante.
  grep -ch '^TEST(' "$1"/tests/test_*.cpp 2>/dev/null \
    | awk '{s+=$1} END {print s+0}'
}

# Relit le journal toutes les demi-secondes et repose l'etat. Le demon le
# relit une fois par seconde : plus fin ne se verrait pas.
surveiller() { # surveiller <mode> <journal> <plancher> <plafond> [total]
  WSUF="tmpw"
  _mode="$1"; _log="$2"; _bas="$3"; _haut="$4"; _tot="${5:-0}"
  while :; do
    sleep 0.5
    if [ "$_mode" = compilation ]; then
      _p=$(pourcentage_compilation "$_log")
    else
      _p=$(pourcentage_tests "$_log" "$_tot")
    fi
    [ -n "$_p" ] || continue
    PROGRESS=$((_bas + (_haut - _bas) * _p / 100))
    write_state applying "" "$SUPERVISE"
  done
}

# Le pid du script, tel qu'il doit apparaitre dans l'etat : c'est LUI que le
# demon interroge pour savoir si le travail court encore, jamais celui du
# surveillant, qui est un detail d'implementation.
SUPERVISE=$$

demarrer_surveillance() { # <mode> <journal> <plancher> <plafond> [total]
  surveiller "$@" &
  SURVEILLANT=$!
}

# `set -eu` est actif : un `kill` sur un processus deja parti et un `wait`
# sur un travail tue rendent tous deux un code non nul, et feraient avorter
# la mise a jour au moment precis ou elle vient de reussir. Les deux sont
# donc neutralises -- l'arret du surveillant ne peut pas etre un echec.
arreter_surveillance() {
  [ -n "${SURVEILLANT:-}" ] || return 0
  kill "$SURVEILLANT" 2>/dev/null || :
  wait "$SURVEILLANT" 2>/dev/null || :
  SURVEILLANT=""
}

# UN SIGNAL VENU DE L'EXTERIEUR NE DOIT PAS LAISSER LE VERROU PRIS.
#
# Le surveillant hérite du descripteur 9, et un verrou `flock` appartient à la
# DESCRIPTION de fichier ouverte, partagée par le fork : si le script principal
# meurt sans passer par arreter_surveillance -- un SIGTERM ne vise que son
# pid --, le fils survit, garde le verrou POUR TOUJOURS et réécrit l'état
# toutes les demi-secondes avec un instantané figé. Toute invocation ultérieure
# répondrait « un autre travail est en cours », sans qu'aucun travail ne coure.
#
# Aucun chemin interne n'y mène : tous les échecs appellent déjà
# arreter_surveillance. C'est le signal externe que ce piège attrape.
trap 'arreter_surveillance' HUP INT TERM

build_and_test_tree() { # build_and_test_tree <racine> <tmp>
  have cmake || { fail apply-failed "cmake absent"; return 1; }
  have c++ || { fail apply-failed "compilateur absent"; return 1; }
  STAGE="compilation"; PROGRESS=$P_BUILD; write_state applying "" "$$"
  if ! timeout "$APPLY_TIMEOUT" cmake -S "$1" -B "$1/build-release" \
        -DCMAKE_BUILD_TYPE=Release >"$STATE_DIR/build.log" 2>&1; then
    fail apply-failed "configuration cmake echouee"; return 1
  fi
  demarrer_surveillance compilation "$STATE_DIR/build.log" "$P_BUILD" "$P_TESTS"
  if ! timeout "$APPLY_TIMEOUT" cmake --build "$1/build-release" \
        -j"$(nproc)" >>"$STATE_DIR/build.log" 2>&1; then
    arreter_surveillance
    fail apply-failed "compilation echouee"; return 1
  fi
  arreter_surveillance
  # LE CRITERE EST LE CODE DE RETOUR, jamais un compte de cas : le total
  # perime a chaque commit qui ajoute un test.
  STAGE="suite de tests"; PROGRESS=$P_TESTS; write_state applying "" "$$"
  : > "$STATE_DIR/tests.log"
  demarrer_surveillance tests "$STATE_DIR/tests.log" "$P_TESTS" "$P_INSTALL" \
    "$(total_des_cas "$1")"
  if ! run_suite "$1/build-release/sshos_tests" "$1/tests/golden"; then
    arreter_surveillance
    fail apply-failed "la suite de tests echoue, rien n'est installe"; return 1
  fi
  arreter_surveillance
}

# --- --rollback -----------------------------------------------------------
do_rollback() {
  [ -f "$EXE.previous" ] || fail apply-failed "aucune version precedente"
  cp "$EXE.previous" "$EXE.new"
  chmod 0755 "$EXE.new"
  mv -f "$EXE.new" "$EXE"

  # L'ETAT DOIT DIRE LA VERITE. Sans cette reecriture, il continuerait
  # d'annoncer le commit neuf : la verification suivante repondrait « a
  # jour » et l'utilisateur ne pourrait PLUS JAMAIS reappliquer la mise a
  # jour dont il vient de revenir.
  #
  # Il n'y a qu'UN SEUL niveau de retour : termos.previous est ecrase a
  # chaque mise a jour.
  INSTALLED="${PREVIOUS:-unknown}"
  PREVIOUS=""
  if [ -d "$SRC/.git" ] && [ "$INSTALLED" != unknown ]; then
    _pv=$(version_of "$SRC" "$INSTALLED" || true)
    INSTALLED_VERSION="${_pv:-}"
  else
    INSTALLED_VERSION=""
  fi
  COMMITS_AHEAD=""
  # UN RETOUR ARRIERE POSE UN BINAIRE, LUI AUSSI -- avec une inode neuve, donc
  # ce n'est pas celui qui tourne. Le fait vaut ici exactement comme apres une
  # application ; `status` reste `available`, puisqu'on PEUT reappliquer.
  RESTART_PENDING=1
  write_state available "version precedente restauree"
}

case "${1:---check}" in
  --check)    do_check ;;
  --apply)    do_apply ;;
  --rollback) do_rollback ;;
  --help|-h)  echo "usage: termos-update --check | --apply | --rollback" ;;
  *) echo "termos-update: mode inconnu : $1" >&2; exit 2 ;;
esac
