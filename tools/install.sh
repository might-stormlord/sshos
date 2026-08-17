#!/bin/sh
# Installation de ssh_os dans le home de l'utilisateur, isolee de l'arbre de
# developpement.
#
# Ce script ne decide rien a la place de l'utilisateur, et il ne modifie
# aucun fichier de configuration sans un oui explicite.
#
# Conception : docs/superpowers/specs/2026-08-17-installation-et-mise-a-jour-design.md
set -eu

REPO_URL="https://github.com/might-stormlord/sshos.git"
API="https://api.github.com/repos/might-stormlord/sshos"
CODELOAD="https://codeload.github.com/might-stormlord/sshos"

# Par defaut curl suit une redirection https -> http, et les URL d'assets
# GitHub redirigent. On l'interdit.
CURL_OPTS="--proto =https --proto-redir =https --tlsv1.2 -fsSL"
WGET_OPTS="--https-only -q -O"

# --- valeurs par defaut, modifiables par drapeau ---------------------------
PREFIX="${HOME}/.local"
INSTANCE="local"
WANT_SOURCE="auto"      # auto | git | release | archive | local
ASSUME_YES="no"
WANT_LINGER="ask"       # ask | yes | no
LOCAL_TREE=""           # arbre a utiliser pour l'echelon 4

usage() {
  cat <<'FIN'
usage: install.sh [options]

  --prefix CHEMIN     ou installer (defaut : ~/.local)
  --instance NOM      valeur de SSHOS_BOOT_ID (defaut : local)
  --source QUOI       auto | git | release | archive | local (defaut : auto)
  --local-tree CHEMIN arbre a utiliser pour --source local (defaut : celui du script)
  --linger yes|no     poser ou non loginctl enable-linger, sans demander
  --yes               ne rien demander, prendre les defauts
  --help              ceci

Sans --yes et sur un terminal, quatre questions sont posees.
FIN
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)     PREFIX="$2"; shift 2 ;;
    --instance)   INSTANCE="$2"; shift 2 ;;
    --source)     WANT_SOURCE="$2"; shift 2 ;;
    --local-tree) LOCAL_TREE="$2"; shift 2 ;;
    --linger)     WANT_LINGER="$2"; shift 2 ;;
    --yes)        ASSUME_YES="yes"; shift ;;
    --help|-h)    usage; exit 0 ;;
    *) echo "install.sh: option inconnue : $1" >&2; usage >&2; exit 2 ;;
  esac
done

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ -n "$LOCAL_TREE" ] || LOCAL_TREE=$(dirname -- "$SELF_DIR")

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Une question fermee. Sans terminal ou avec --yes, le defaut gagne.
ask() { # ask <question> <defaut>
  _q="$1"; _d="$2"
  if [ "$ASSUME_YES" = yes ] || [ ! -t 0 ]; then
    printf '%s' "$_d"
    return 0
  fi
  printf '%s [%s] : ' "$_q" "$_d" >&2
  IFS= read -r _a || _a=""
  [ -n "$_a" ] || _a="$_d"
  printf '%s' "$_a"
}

# --- 1. etat des lieux -----------------------------------------------------
step "Etat des lieux"

HAVE_GIT=no;   have git   && HAVE_GIT=yes
HAVE_TAR=no;   have tar   && HAVE_TAR=yes
HAVE_CC=no;    have c++   && HAVE_CC=yes
HAVE_CMAKE=no; have cmake && HAVE_CMAKE=yes
HAVE_SHA=no;   have sha256sum && HAVE_SHA=yes
NET=""
have curl && NET=curl
[ -z "$NET" ] && have wget && NET=wget

printf '  compilateur C++ : %s\n' "$HAVE_CC"
printf '  cmake           : %s\n' "$HAVE_CMAKE"
printf '  git             : %s\n' "$HAVE_GIT"
printf '  reseau          : %s\n' "${NET:-aucun}"
printf '  tar             : %s\n' "$HAVE_TAR"

fetch() { # fetch <url> <destination>
  case "$NET" in
    curl) curl $CURL_OPTS -o "$2" "$1" ;;
    wget) wget $WGET_OPTS "$2" "$1" ;;
    *) return 1 ;;
  esac
}
fetch_stdout() { # fetch_stdout <url>
  case "$NET" in
    curl) curl $CURL_OPTS "$1" ;;
    wget) wget $WGET_OPTS - "$1" ;;
    *) return 1 ;;
  esac
}

# --- 2. les quatre questions ----------------------------------------------
step "Installation"

PREFIX=$(ask "Ou installer ?" "$PREFIX")
case "$PREFIX" in
  /*) ;;
  *) die "le prefixe doit etre un chemin absolu : $PREFIX" ;;
esac

BIN_DIR="$PREFIX/bin"
LIBEXEC="$PREFIX/libexec"
# L'etat vit TOUJOURS sous le home de l'utilisateur, quel que soit le
# prefixe : il lui est propre, pas propre a l'installation.
SHARE="${XDG_DATA_HOME:-$HOME/.local/share}/sshos"

EXE="$LIBEXEC/sshos"
LAUNCHER="$BIN_DIR/sshos"
UPDATER="$LIBEXEC/sshos-update"
SRC="$SHARE/src"
GOLDEN="$SHARE/golden"
STATE="$SHARE/state"
LOCK="$SHARE/lock"
LOG="$SHARE/update.log"

mkdir -p "$BIN_DIR" "$LIBEXEC" "$SHARE"

# Le verrou couvre TOUTE la sequence : rotation du binaire, pose, ecriture
# de l'etat. Sans lui, deux installations concurrentes font que
# sshos.previous finit par contenir le NOUVEAU binaire, et le retour
# arriere restaure alors la version cassee.
if have flock; then
  exec 9>"$LOCK"
  flock -n 9 || die "une autre installation ou mise a jour est en cours"
fi

# Le PATH : on donne la ligne exacte, on ne touche pas au profil.
case ":$PATH:" in
  *":$BIN_DIR:"*) say "  $BIN_DIR est dans le PATH." ;;
  *)
    say "  ATTENTION : $BIN_DIR n'est pas dans votre PATH."
    say "  Ajoutez cette ligne a votre profil (je n'y touche pas) :"
    say ""
    say "      export PATH=\"$BIN_DIR:\$PATH\""
    say ""
    ;;
esac

# La persistance apres deconnexion complete : c'est de la configuration
# systeme, donc elle se demande.
if [ "$WANT_LINGER" = ask ]; then
  say "  Faire survivre le demon a une deconnexion COMPLETE demande"
  say "  « loginctl enable-linger », qui est une configuration systeme."
  _r=$(ask "  La poser maintenant ? (oui/non)" "non")
  case "$_r" in oui|o|yes|y) WANT_LINGER=yes ;; *) WANT_LINGER=no ;; esac
fi

INSTANCE=$(ask "Nom d'instance (valeur de SSHOS_BOOT_ID)" "$INSTANCE")
[ -n "$INSTANCE" ] || die "le nom d'instance ne peut pas etre vide"

# --- 3. l'echelle d'acquisition -------------------------------------------
TMP=$(mktemp -d)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

SOURCE=""
COMMIT="unknown"
BUILT_EXE=""
BUILT_TESTS=""
GOLDEN_SRC=""

# Eprouve un binaire AVANT de l'installer. La sonde est un appel avec un
# drapeau inconnu : main.cpp repond « usage: » sur stderr et rend 2. Elle
# s'execute avec SSHOS_BOOT_ID posee, sans quoi main() sort en 1 AVANT meme
# de lire argv quand /proc/sys est masque -- un binaire sain serait alors
# classe casse.
probe_binary() { # probe_binary <chemin>
  _out=$(SSHOS_BOOT_ID=probe "$1" --probe-unknown-flag 2>&1) && _rc=0 || _rc=$?
  case "$_rc" in
    2) printf '%s' "$_out" | grep -q '^usage:' && return 0 || return 1 ;;
    1) return 0 ;;   # charge, environnement incomplet : ce n'est pas le binaire
    *) return 1 ;;   # 126/127/signal : l'editeur de liens l'a refuse
  esac
}

build_tree() { # build_tree <racine> -> pose BUILT_EXE/BUILT_TESTS/GOLDEN_SRC
  _root="$1"
  [ "$HAVE_CC" = yes ] || die "aucun compilateur C++ : impossible de compiler"
  [ "$HAVE_CMAKE" = yes ] || die "cmake absent : impossible de compiler"
  say "  compilation dans $_root ..."
  cmake -S "$_root" -B "$_root/build-release" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$_root/build-release" -j"$(nproc)" >/dev/null
  BUILT_EXE="$_root/build-release/sshos"
  BUILT_TESTS="$_root/build-release/sshos_tests"
  GOLDEN_SRC="$_root/tests/golden"
}

# L'arbre de compilation n'est reutilise que s'il est bien ce qu'on croit.
prepare_git_tree() {
  if [ -d "$SRC/.git" ] && \
     [ "$(git -C "$SRC" remote get-url origin 2>/dev/null || true)" = "$REPO_URL" ]; then
    git -C "$SRC" fetch --quiet origin main
  else
    rm -rf "$SRC"
    git clone --quiet "$REPO_URL" "$SRC" || return 1
  fi
  git -C "$SRC" checkout --quiet -B main origin/main
  COMMIT=$(git -C "$SRC" rev-parse HEAD)
}

try_git() {
  [ "$HAVE_GIT" = yes ] || return 1
  [ "$HAVE_CC" = yes ] && [ "$HAVE_CMAKE" = yes ] || return 1
  step "Echelon 1 : git"
  prepare_git_tree || return 1
  build_tree "$SRC"
  SOURCE=git
}

try_release() {
  [ -n "$NET" ] || return 1
  [ "$HAVE_TAR" = yes ] || return 1
  step "Echelon 2 : binaire publie"
  _json="$TMP/rel.json"
  fetch "$API/releases/latest" "$_json" || return 1
  _base=$(sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' "$_json" | head -1)
  [ -n "$_base" ] || { say "  aucune release publiee"; return 1; }
  _dir=$(dirname "$_base")
  for f in sshos sshos_tests golden.tar.gz SHA256SUMS; do
    fetch "$_dir/$f" "$TMP/$f" || { say "  $f absent de la release"; return 1; }
  done
  if [ "$HAVE_SHA" = yes ]; then
    ( cd "$TMP" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) || {
      say "  SHA256 non concordant : abandon de cet echelon"; return 1; }
  else
    say "  sha256sum absent : integrite NON verifiee, echelon abandonne"
    return 1
  fi
  chmod 0755 "$TMP/sshos" "$TMP/sshos_tests"
  probe_binary "$TMP/sshos" || { say "  le binaire ne se charge pas ici"; return 1; }
  mkdir -p "$TMP/refs" && tar xzf "$TMP/golden.tar.gz" -C "$TMP/refs"
  BUILT_EXE="$TMP/sshos"
  BUILT_TESTS="$TMP/sshos_tests"
  GOLDEN_SRC="$TMP/refs/golden"
  COMMIT=$(sed -n 's/.*"tag_name": *"commit-\([0-9a-f]*\)".*/\1/p' "$_json" | head -1)
  [ -n "$COMMIT" ] || COMMIT=unknown
  SOURCE=release
}

try_archive() {
  [ -n "$NET" ] || return 1
  [ "$HAVE_TAR" = yes ] || return 1
  [ "$HAVE_CC" = yes ] && [ "$HAVE_CMAKE" = yes ] || return 1
  step "Echelon 3 : archive des sources"
  # Le sha D'ABORD, jamais refs/heads/main : une cible mouvante ne dirait
  # pas ce qu'on a pris, et installed_commit resterait inconnu a vie.
  _sha=$(fetch_stdout "$API/commits/main" \
         | sed -n 's/.*"sha": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1) || return 1
  [ -n "$_sha" ] || return 1
  fetch "$CODELOAD/tar.gz/$_sha" "$TMP/src.tar.gz" || return 1
  rm -rf "$SRC" && mkdir -p "$SRC"
  # GitHub ajoute un repertoire de tete : on l'absorbe.
  tar xzf "$TMP/src.tar.gz" -C "$SRC" --strip-components=1
  COMMIT="$_sha"
  build_tree "$SRC"
  SOURCE=archive
}

try_local() {
  step "Echelon 4 : arbre local"
  [ -f "$LOCAL_TREE/CMakeLists.txt" ] || return 1
  if [ "$HAVE_GIT" = yes ] && [ -d "$LOCAL_TREE/.git" ]; then
    COMMIT=$(git -C "$LOCAL_TREE" rev-parse HEAD 2>/dev/null || echo unknown)
  fi
  rm -rf "$SRC" && mkdir -p "$SRC"
  ( cd "$LOCAL_TREE" && tar cf - \
      --exclude=./build-release --exclude=./build-debug --exclude=./build-dbg \
      --exclude=./.git . ) | ( cd "$SRC" && tar xf - )
  build_tree "$SRC"
  SOURCE=local
}

case "$WANT_SOURCE" in
  git)     try_git     || die "echelon git impossible" ;;
  release) try_release || die "echelon release impossible" ;;
  archive) try_archive || die "echelon archive impossible" ;;
  local)   try_local   || die "echelon local impossible" ;;
  auto)
    try_git || try_release || try_archive || try_local || \
      die "aucun echelon praticable : il faut au minimum un compilateur, cmake et cet arbre"
    ;;
  *) die "source inconnue : $WANT_SOURCE" ;;
esac

say "  echelon retenu : $SOURCE (commit ${COMMIT})"

# --- 4. la suite complete AVANT de poser quoi que ce soit -------------------
step "Suite de tests"
mkdir -p "$GOLDEN"
rm -rf "$GOLDEN"
cp -r "$GOLDEN_SRC" "$GOLDEN"

# Le critere est le CODE DE RETOUR, jamais un compte de cas : le total
# perime a chaque commit qui ajoute un test.
if SSHOS_GOLDEN_DIR="$GOLDEN" "$BUILT_TESTS" >"$TMP/tests.log" 2>&1; then
  say "  suite au vert"
else
  tail -20 "$TMP/tests.log" >&2
  die "la suite de tests echoue : rien n'est installe"
fi

# --- 5. la pose ------------------------------------------------------------
step "Pose"

# COPIE vers .previous, puis ecriture dans .new, puis rename. Une ecriture
# EN PLACE sur un binaire en cours d'execution rend ETXTBSY a tous les
# coups, c'est-a-dire exactement dans le cas ou l'on met a jour ; et deux
# « mv » successifs ouvrent une fenetre pendant laquelle le chemin n'existe
# pas.
if [ -f "$EXE" ]; then
  cp "$EXE" "$EXE.previous"
fi
cp "$BUILT_EXE" "$EXE.new"
chmod 0755 "$EXE.new"
mv -f "$EXE.new" "$EXE"
say "  binaire : $EXE"

# Le lanceur. Il pose l'identite du bureau et son chemin de relance ; le
# demon relance par spawn_detached en herite, donc demon et clients
# composent le meme nom de socket.
cat > "$LAUNCHER.new" <<FIN
#!/bin/sh
# Lanceur de ssh_os. Ecrit par tools/install.sh -- ne pas editer a la main.
SSHOS_BOOT_ID="\${SSHOS_BOOT_ID:-$INSTANCE}"
SSHOS_EXE="$EXE"
export SSHOS_BOOT_ID SSHOS_EXE
exec "\$SSHOS_EXE" "\$@"
FIN
chmod 0755 "$LAUNCHER.new"
mv -f "$LAUNCHER.new" "$LAUNCHER"
say "  lanceur : $LAUNCHER"

# Le script de mise a jour, pose a cote du binaire. La mise a jour le
# remplacera lui aussi : un script fige piloterait un binaire qui a evolue.
if [ -f "$SRC/tools/update.sh" ]; then
  cp "$SRC/tools/update.sh" "$UPDATER.new"
  chmod 0755 "$UPDATER.new"
  mv -f "$UPDATER.new" "$UPDATER"
  say "  mise a jour : $UPDATER"
fi

: > "$LOG" 2>/dev/null || true

# --- 6. l'etat initial -----------------------------------------------------
# L'installeur VIENT de verifier : checked_at est maintenant. Ecrire 0
# ferait partir une verification trente secondes apres la premiere
# ouverture ; ecrire autre chose ferait attendre un jour.
STATUS=up-to-date
MESSAGE=
case "$PREFIX" in
  "$HOME"/*) ;;
  *) STATUS=updates-disabled
     MESSAGE="prefixe systeme : mise a jour depuis le bureau desactivee" ;;
esac

cat > "$STATE.tmp" <<FIN
schema=1
prefix=$PREFIX
source=$SOURCE
installed_commit=$COMMIT
previous_commit=
remote_commit=
checked_at=$(date +%s)
status=$STATUS
pid=
message=$MESSAGE
FIN
mv -f "$STATE.tmp" "$STATE"
say "  etat : $STATE"

# --- 7. la persistance, seulement sur un oui -------------------------------
if [ "$WANT_LINGER" = yes ]; then
  if have loginctl; then
    loginctl enable-linger "$(id -un)" && say "  linger active"
  else
    say "  loginctl absent : linger non active"
  fi
fi

step "Termine"
say "  Lancez : $LAUNCHER"
say "  Instance : SSHOS_BOOT_ID=$INSTANCE"
if [ "$STATUS" = updates-disabled ]; then
  say "  Mise a jour depuis le bureau : DESACTIVEE ($MESSAGE)"
fi
