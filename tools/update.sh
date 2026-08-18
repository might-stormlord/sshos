#!/bin/sh
# Verification, application et retour arriere de la mise a jour de ssh_os.
#
# LE DEMON NE FAIT QUE LIRE CE QUE CE SCRIPT ECRIT. C'est ce qui garde git,
# cmake et le reseau hors de son fil unique.
#
# usage : sshos-update --check | --apply | --rollback
#
# Conception : docs/superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md
set -eu

# L'URL du depot. Surchargeable pour la meme raison que boot_id_path dans
# net.hpp : sans ca, la sonde bout-en-bout devrait parler au vrai GitHub et
# compiler le vrai projet a chaque cas -- des minutes par verdict, et un
# reseau dans la boucle d'un test.
REPO_URL="${SSHOS_REPO_URL:-https://github.com/might-stormlord/sshos.git}"
API="https://api.github.com/repos/might-stormlord/sshos"
CODELOAD="https://codeload.github.com/might-stormlord/sshos"

# Par defaut curl suit une redirection https -> http, et les URL d'assets
# GitHub redirigent. On l'interdit.
CURL_OPTS="--proto =https --proto-redir =https --tlsv1.2 -fsSL"

# Une verification est un aller-retour reseau ; une application compile et
# passe la suite complete. Sans ces bornes, « en cours » est un etat dont on
# ne sort jamais.
CHECK_TIMEOUT=60
APPLY_TIMEOUT=1800

# --- ou l'on vit ----------------------------------------------------------
# Le prefixe se deduit du chemin du script -- <prefixe>/libexec/sshos-update --
# et jamais de ~ en dur : c'est ce qui permet a la sonde de ne pas ecraser
# l'installation reelle.
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX="${SSHOS_PREFIX:-$(dirname -- "$SELF")}"

# L'etat est propre a l'UTILISATEUR, pas a l'installation.
STATE_DIR="${SSHOS_STATE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/sshos}"
STATE="$STATE_DIR/state"
LOCK="$STATE_DIR/lock"
SRC="$STATE_DIR/src"
GOLDEN="$STATE_DIR/golden"

EXE="$PREFIX/libexec/sshos"
UPDATER="$PREFIX/libexec/sshos-update"
VERSIONER="$PREFIX/libexec/sshos-version"

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
SOURCE=$(get source);            [ -n "$SOURCE" ] || SOURCE=git
INSTALLED=$(get installed_commit); [ -n "$INSTALLED" ] || INSTALLED=unknown
PREVIOUS=$(get previous_commit)
CHECKED=$(get checked_at);       [ -n "$CHECKED" ] || CHECKED=0
REMOTE=$(get remote_commit)

# --- ecriture de l'etat ---------------------------------------------------
# UN SEUL RETOUR A LA LIGNE DANS UN MESSAGE FORGERAIT UNE PAIRE CLE=VALEUR.
# Le §8 verse ici le resume d'une compilation cassee : l'assainissement est
# donc obligatoire, et il se fait chez l'ECRIVAIN.
sanitize() {
  printf '%s' "$1" | tr -d '\000-\037\177' | cut -c1-200
}

write_state() { # write_state <status> <message> [pid]
  _st="$1"; _msg=$(sanitize "${2:-}"); _pid="${3:-}"
  # Le fichier temporaire est dans le MEME repertoire, donc le meme systeme
  # de fichiers : c'est ce qui rend le rename atomique.
  cat > "$STATE.tmp" <<FIN
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
checked_at=$CHECKED
status=$_st
pid=$_pid
message=$_msg
FIN
  mv -f "$STATE.tmp" "$STATE"
}

fail() { # fail <status> <message>
  write_state "$1" "$2"
  printf 'sshos-update: %s\n' "$2" >&2
  exit 1
}

# --- le verrou ------------------------------------------------------------
# Il couvre TOUTE la sequence : rotation du binaire, pose, ecriture d'etat.
# Sans lui, deux applications concurrentes font que sshos.previous finit par
# contenir le NOUVEAU binaire, et --rollback restaure alors la version
# cassee : le filet de securite detruit par la course qu'il devait rattraper.
if have flock; then
  exec 9>"$LOCK"
  flock -n 9 || { echo "sshos-update: un autre travail est en cours" >&2; exit 1; }
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
# sshos-version, que l'installeur pose a cote de ce script.
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
  if [ "$SOURCE" = local ]; then
    write_state updates-disabled "installation locale, pas de source distante"
    return 0
  fi
  write_state checking "" "$$"

  _remote=$(remote_head || true)
  if [ -z "$_remote" ]; then
    fail check-failed "verification impossible : reseau ou depot injoignable"
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
    write_state up-to-date ""
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
  SSHOS_GOLDEN_DIR="$2" timeout "$APPLY_TIMEOUT" "$1" >"$STATE_DIR/tests.log" 2>&1
}

do_apply() {
  if [ "$SOURCE" = local ]; then
    write_state updates-disabled "installation locale, pas de source distante"
    return 0
  fi
  STAGE="preparation"
  write_state applying "" "$$"

  _tmp=$(mktemp -d)
  # PAS DE DESCENTE D'ECHELON PENDANT UNE MISE A JOUR. Un echec de l'echelon
  # inscrit dans source= donne apply-failed, jamais un repli silencieux vers
  # un canal moins controle -- sinon il suffirait de servir un binaire
  # illisible pour forcer la degradation.
  case "$SOURCE" in
    git)
      have git || { rm -rf "$_tmp"; fail apply-failed "git a disparu"; }
      STAGE="recuperation des sources"; write_state applying "" "$$"
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
  STAGE="installation"; write_state applying "" "$$"
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
  # RESTART-PENDING, PAS UP-TO-DATE. Le binaire pose n'est pas celui qui
  # tourne : annoncer « a jour » eteindrait la pastille alors que
  # l'utilisateur continue sur l'ancienne version.
  write_state restart-pending "redemarrez pour terminer"
}

build_and_test_tree() { # build_and_test_tree <racine> <tmp>
  have cmake || { fail apply-failed "cmake absent"; return 1; }
  have c++ || { fail apply-failed "compilateur absent"; return 1; }
  STAGE="compilation"; write_state applying "" "$$"
  if ! timeout "$APPLY_TIMEOUT" cmake -S "$1" -B "$1/build-release" \
        -DCMAKE_BUILD_TYPE=Release >"$STATE_DIR/build.log" 2>&1; then
    fail apply-failed "configuration cmake echouee"; return 1
  fi
  if ! timeout "$APPLY_TIMEOUT" cmake --build "$1/build-release" \
        -j"$(nproc)" >>"$STATE_DIR/build.log" 2>&1; then
    fail apply-failed "compilation echouee"; return 1
  fi
  # LE CRITERE EST LE CODE DE RETOUR, jamais un compte de cas : le total
  # perime a chaque commit qui ajoute un test.
  STAGE="suite de tests"; write_state applying "" "$$"
  if ! run_suite "$1/build-release/sshos_tests" "$1/tests/golden"; then
    fail apply-failed "la suite de tests echoue, rien n'est installe"; return 1
  fi
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
  # Il n'y a qu'UN SEUL niveau de retour : sshos.previous est ecrase a
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
  write_state available "version precedente restauree"
}

case "${1:---check}" in
  --check)    do_check ;;
  --apply)    do_apply ;;
  --rollback) do_rollback ;;
  --help|-h)  echo "usage: sshos-update --check | --apply | --rollback" ;;
  *) echo "sshos-update: mode inconnu : $1" >&2; exit 2 ;;
esac
