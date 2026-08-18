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
INSTANCE="bureau01"
WANT_SOURCE="auto"      # auto | git | release | archive | local
ASSUME_YES="no"
# loginctl enable-linger n'est PLUS demande. Le demon avertit lui-meme, au
# premier lancement et seulement quand le cas se presente (main.cpp) :
# « logind est configure avec KillUserProcesses=yes ». Le bon message, au bon
# moment, par le bon composant -- le doubler ici a l'aveugle n'apportait rien.
# Le drapeau reste, pour qui sait qu'il en a besoin.
WANT_LINGER="no"        # yes | no, via --linger
WANT_PATH="ask"         # ask | yes | no
LOCAL_TREE=""           # arbre a utiliser pour l'echelon 4

usage() {
  cat <<'FIN'
usage: install.sh [options]

  --prefix CHEMIN     ou installer (defaut : ~/.local)
  --instance NOM      nom de cette instance (defaut : bureau01)
  --source QUOI       auto | git | release | archive | local (defaut : auto)
  --local-tree CHEMIN arbre a utiliser pour --source local (defaut : celui du script)
  --linger yes|no     poser ou non loginctl enable-linger, sans demander
  --path yes|no       ajouter ou non le repertoire au PATH, sans demander
  --yes               ne rien demander, prendre les defauts
  --help              ceci

Sur un terminal, un assistant pose trois reglages. Sans terminal ou
avec --yes, les defauts s'appliquent sans rien demander.
FIN
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)     PREFIX="$2"; shift 2 ;;
    --instance)   INSTANCE="$2"; shift 2 ;;
    --source)     WANT_SOURCE="$2"; shift 2 ;;
    --local-tree) LOCAL_TREE="$2"; shift 2 ;;
    --linger)     WANT_LINGER="$2"; shift 2 ;;
    --path)       WANT_PATH="$2"; shift 2 ;;
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

# --- 2. les reglages -------------------------------------------------------
# Trois reglages, poses par un assistant quand il y a un terminal, par des
# questions simples sinon. Les DEUX chemins remplissent exactement les memes
# variables : PREFIX, WANT_PATH, INSTANCE.
. "$SELF_DIR/tui.sh"

# L'etat vit TOUJOURS sous le home de l'utilisateur, quel que soit le
# prefixe : il lui est propre, pas propre a l'installation.
SHARE="${XDG_DATA_HOME:-$HOME/.local/share}/sshos"
mkdir -p "$SHARE"

# Le verrou couvre TOUTE la sequence : rotation du binaire, pose, ecriture
# de l'etat. Sans lui, deux installations concurrentes font que
# sshos.previous finit par contenir le NOUVEAU binaire, et le retour arriere
# restaure alors la version cassee.
if have flock; then
  exec 9>"$SHARE/lock"
  flock -n 9 || die "une autre installation ou mise a jour est en cours"
fi

# Le fichier ou la ligne de PATH doit aller.
#
# ~/.profile, ET PAS ~/.bashrc. Deux raisons, toutes deux verifiees :
#
#  - un shell de CONNEXION -- celui d'une session SSH -- ne lit ~/.bashrc que
#    si ~/.profile le source. C'est le cas sur Ubuntu, ce n'est pas une regle.
#    Ecrire dans ~/.bashrc laisse donc « sshos » introuvable sur toute machine
#    ou ce chainage n'existe pas ;
#  - ~/.bashrc commence presque toujours par « [ -z "$PS1" ] && return », donc
#    la ligne n'y vaudrait que pour les shells INTERACTIFS : « ssh machine
#    sshos » ne la verrait jamais. Depuis ~/.profile, si.
#
# zsh ne lit pas ~/.profile : pour lui, ~/.zprofile, qui joue le meme role.
profile_file() {
  _sh=$(getent passwd "$(id -un)" 2>/dev/null | cut -d: -f7)
  [ -n "$_sh" ] || _sh="${SHELL:-/bin/sh}"
  case "$_sh" in
    */zsh) printf '%s' "$HOME/.zprofile" ;;
    *)     printf '%s' "$HOME/.profile" ;;
  esac
}

# Deja joignable, ou deja ecrit ? On ne pose pas deux fois la meme ligne.
path_already_set() { # path_already_set <repertoire bin>
  case ":$PATH:" in *":$1:"*) return 0 ;; esac
  [ -f "$(profile_file)" ] && grep -qF "$1" "$(profile_file)" && return 0
  return 1
}

# ---------------------------------------------------------------- assistant
WIZ_STEPS=3
W_PREFIX_LABEL=""
W_PATH_LABEL=""

# Les lignes d'ecran ou tombent les choix, remplies pendant le dessin : sans
# elles le clic ne saurait pas ce qu'on vise. C'est la meme discipline que le
# panneau du bureau -- ce qu'on clique est ce qu'on voit, parce que les deux
# lisent la meme chose.
CLICK1=0; CLICK2=0; CLICK3=0

wiz_load() { # wiz_load <etape>
  CN=0; SEL_MAX=0
  CH1=""; CD1=""; CH2=""; CD2=""; CH3=""; CD3=""
  NOTE=""; NOTE2=""
  MODE=choix
  case "$1" in
    1) QUESTION="Ou installer ?"
       NOTE="Le binaire et le lanceur vont la. Votre etat de mise a jour,"
       NOTE2="lui, reste toujours sous votre home."
       CH1="~/.local";    CD1="votre home, aucun droit root"
       CH2="/usr/local";  CD2="toute la machine, sudo, et pas de maj auto"
       CH3="autre...";    CD3="saisir un chemin absolu"
       CN=3 ;;
    2) QUESTION="Ajouter au PATH ?"
       NOTE="La ligne ira dans ~/.profile, que tout shell de connexion lit."
       NOTE2="Sans elle, il faut taper le chemin complet a chaque fois."
       CH1="oui";  CD1="taper sshos depuis n'importe ou"
       CH2="non";  CD2="lancer par son chemin complet"
       CN=2 ;;
    3) QUESTION="Nom d'instance"
       NOTE="Ce nom compose l'adresse du socket, et c'est la SEULE chose"
       NOTE2="qui separe ce bureau de l'arbre de dev et des autres."
       MODE=saisie
       CN=0 ;;
  esac
  SEL_MAX=$CN
}

wiz_answered_row() { # wiz_answered_row <libelle> <valeur>
  _t=$(printf '%-30s %s' "$1" "$2")
  printf '%s \033[32m%s\033[0m ' "$G_V" "$G_OK"
  ui_pad "$_t" $((UI_COLS - 6))
  printf ' %s\n' "$G_V"
}

wiz_choice_row() { # wiz_choice_row <indice> <libelle> <description>
  _t=$(printf '  %-14s %s' "$2" "$3")
  if [ "$1" = "$SEL" ]; then
    printf '%s \033[36m%s\033[0m \033[1m' "$G_V" "$G_SEL"
    ui_pad "$_t" $((UI_COLS - 6))
    printf '\033[0m %s\n' "$G_V"
  else
    printf '%s   \033[2m' "$G_V"
    ui_pad "$_t" $((UI_COLS - 6))
    printf '\033[0m %s\n' "$G_V"
  fi
}

wiz_draw() {
  ui_home
  R=1
  ui_top;                                      R=$((R + 1))
  ui_title "Installation de ssh_os";           R=$((R + 1))
  ui_blank;                                    R=$((R + 1))

  [ -n "$W_PREFIX_LABEL" ] && { wiz_answered_row "Ou installer ?" "$W_PREFIX_LABEL"; R=$((R + 1)); }
  [ -n "$W_PATH_LABEL" ]   && { wiz_answered_row "Ajouter au PATH ?" "$W_PATH_LABEL"; R=$((R + 1)); }
    [ "$STEP" -gt 1 ] && { ui_blank; R=$((R + 1)); }

  printf '%s \033[36m%s\033[0m \033[1m' "$G_V" "$G_SEL"
  ui_pad "$QUESTION" $((UI_COLS - 6))
  printf '\033[0m %s\n' "$G_V"
  R=$((R + 1))
  if [ -n "$NOTE" ]; then
    printf '%s   \033[2m' "$G_V"
    ui_pad "$NOTE" $((UI_COLS - 6))
    printf '\033[0m %s\n' "$G_V"
    R=$((R + 1))
    if [ -n "$NOTE2" ]; then
      printf '%s   \033[2m' "$G_V"
      ui_pad "$NOTE2" $((UI_COLS - 6))
      printf '\033[0m %s\n' "$G_V"
      R=$((R + 1))
    fi
    ui_blank; R=$((R + 1))
  else
    ui_blank; R=$((R + 1))
  fi

  CLICK1=0; CLICK2=0; CLICK3=0
  if [ "$MODE" = saisie ]; then
    printf '%s   [ \033[1m' "$G_V"
    ui_pad "$BUF" 20
    printf '\033[0m]'
    ui_pad "" $((UI_COLS - 29))
    printf ' %s\n' "$G_V"
    R=$((R + 1))
    ui_row ' ' "  adresse du socket : sshos/$(id -u)/$BUF"
    R=$((R + 1))
  else
    [ "$CN" -ge 1 ] && { CLICK1=$R; wiz_choice_row 1 "$CH1" "$CD1"; R=$((R + 1)); }
    [ "$CN" -ge 2 ] && { CLICK2=$R; wiz_choice_row 2 "$CH2" "$CD2"; R=$((R + 1)); }
    [ "$CN" -ge 3 ] && { CLICK3=$R; wiz_choice_row 3 "$CH3" "$CD3"; R=$((R + 1)); }
  fi

  _n=$((3 - CN))
  [ "$MODE" = saisie ] && _n=1
  while [ "$_n" -gt 0 ]; do ui_blank; R=$((R + 1)); _n=$((_n - 1)); done

  ui_blank;                       R=$((R + 1))
  ui_bar $((STEP - 1)) $WIZ_STEPS; R=$((R + 1))
  ui_blank;                       R=$((R + 1))

  if [ "$MODE" = saisie ]; then
    ui_row ' ' "taper le nom   Entree valider   Gauche revenir"
  else
    ui_row ' ' "Haut/Bas choisir   Entree valider   clic aussi   Gauche revenir"
  fi
  R=$((R + 1))
  ui_bottom
}

wiz_apply() { # enregistre la reponse de l'etape courante
  case "$STEP" in
    1) case "$SEL" in
         1) PREFIX="$HOME/.local"; W_PREFIX_LABEL="~/.local" ;;
         2) PREFIX="/usr/local";   W_PREFIX_LABEL="/usr/local" ;;
         3) return 1 ;;  # « autre... » : bascule en saisie
       esac ;;
    2) case "$SEL" in
         1) WANT_PATH=yes; W_PATH_LABEL="ajoutee a $(basename "$(profile_file)")" ;;
         2) WANT_PATH=no;  W_PATH_LABEL="non, chemin complet" ;;
       esac ;;
  esac
  return 0
}

wiz_tui() {
  ui_begin
  STEP=1
  SEL=1
  BUF=""
  wiz_load "$STEP"

  while [ "$STEP" -le "$WIZ_STEPS" ]; do
    # L'etape du PATH ne se pose pas quand le repertoire est deja joignable :
    # une question dont la reponse ne change rien n'est pas une question.
    if [ "$STEP" = 2 ] && path_already_set "$PREFIX/bin"; then
      WANT_PATH=no
      W_PATH_LABEL="deja joignable"
      STEP=3; SEL=1; wiz_load 3; BUF="$INSTANCE"
      continue
    fi

    wiz_draw
    ui_key

    case "$KEY" in
      ctrlc) ui_end; echo "install.sh: abandonne" >&2; exit 130 ;;
      up)    [ "$MODE" = choix ] && { SEL=$((SEL - 1)); [ "$SEL" -lt 1 ] && SEL=$SEL_MAX; } ;;
      down)  [ "$MODE" = choix ] && { SEL=$((SEL + 1)); [ "$SEL" -gt "$SEL_MAX" ] && SEL=1; } ;;
      left)
        if [ "$MODE" = saisie ] && [ "$STEP" = 1 ]; then
          MODE=choix; wiz_load 1; SEL=1
        elif [ "$STEP" -gt 1 ]; then
          STEP=$((STEP - 1)); SEL=1; BUF=""
          case "$STEP" in
            1) W_PREFIX_LABEL="" ;;
            2) W_PATH_LABEL="" ;;
          esac
          wiz_load "$STEP"
        fi ;;
      click)
        _hit=0
        [ "$CLICK1" -gt 0 ] && [ "$MOUSE_Y" = "$CLICK1" ] && { SEL=1; _hit=1; }
        [ "$CLICK2" -gt 0 ] && [ "$MOUSE_Y" = "$CLICK2" ] && { SEL=2; _hit=1; }
        [ "$CLICK3" -gt 0 ] && [ "$MOUSE_Y" = "$CLICK3" ] && { SEL=3; _hit=1; }
        # Cliquer un choix le selectionne ET le valide : c'est ce qu'on
        # attend d'un clic, et c'est la regle « la souris d'abord ».
        [ "$_hit" = 1 ] && KEY=enter
        ;;
    esac

    if [ "$KEY" = enter ]; then
      if [ "$MODE" = saisie ]; then
        case "$STEP" in
          1) if [ -n "$BUF" ]; then
               case "$BUF" in
                 /*) PREFIX="$BUF"; W_PREFIX_LABEL="$BUF"
                     MODE=choix; STEP=2; SEL=1; BUF=""; wiz_load 2 ;;
                 *)  : ;;  # un prefixe doit etre absolu : on ne bouge pas
               esac
             fi ;;
          3) [ -n "$BUF" ] && INSTANCE="$BUF"
             STEP=$((STEP + 1)) ;;
        esac
      else
        if wiz_apply; then
          STEP=$((STEP + 1))
          SEL=1
          [ "$STEP" -le "$WIZ_STEPS" ] && wiz_load "$STEP"
          [ "$STEP" = 3 ] && BUF="$INSTANCE"
        else
          MODE=saisie
          BUF=""
        fi
      fi
      continue
    fi

    if [ "$MODE" = saisie ]; then
      case "$KEY" in
        char)      BUF="$BUF$KEY_CHAR" ;;
        backspace) BUF=${BUF%?} ;;
      esac
    fi
  done

  ui_end
}

wiz_plain() {
  step "Installation"
  PREFIX=$(ask "Ou installer ?" "$PREFIX")
  if ! path_already_set "$PREFIX/bin"; then
    if [ "$WANT_PATH" = ask ]; then
      say "  $PREFIX/bin n'est pas dans votre PATH."
      _r=$(ask "  Ajouter la ligne a $(profile_file) ? (oui/non)" "oui")
      case "$_r" in oui|o|yes|y) WANT_PATH=yes ;; *) WANT_PATH=no ;; esac
    fi
  else
    WANT_PATH=no
    say "  $PREFIX/bin est deja joignable."
  fi
  INSTANCE=$(ask "Nom d'instance (separe ce bureau des autres)" "$INSTANCE")
}

if ui_possible; then
  wiz_tui
else
  wiz_plain
fi

case "$PREFIX" in
  /*) ;;
  *) die "le prefixe doit etre un chemin absolu : $PREFIX" ;;
esac
[ -n "$INSTANCE" ] || die "le nom d'instance ne peut pas etre vide"

BIN_DIR="$PREFIX/bin"
LIBEXEC="$PREFIX/libexec"
EXE="$LIBEXEC/sshos"
LAUNCHER="$BIN_DIR/sshos"
UPDATER="$LIBEXEC/sshos-update"
SRC="$SHARE/src"
GOLDEN="$SHARE/golden"
STATE="$SHARE/state"
LOG="$SHARE/update.log"
mkdir -p "$BIN_DIR" "$LIBEXEC"

step "Reglages retenus"
say "  emplacement  : $PREFIX"
say "  instance     : $INSTANCE"

# La ligne de PATH, si elle a ete acceptee. Idempotente : ni si le
# repertoire est deja joignable, ni si le profil la contient deja.
PROFILE=$(profile_file)
PATH_LINE="export PATH=\"$BIN_DIR:\$PATH\""
if [ "$WANT_PATH" = yes ] && ! path_already_set "$BIN_DIR"; then
  {
    printf '\n# Ajoute par tools/install.sh de ssh_os.\n'
    printf '%s\n' "$PATH_LINE"
  } >> "$PROFILE"
  say "  PATH         : ligne ajoutee a $PROFILE"
  say "                 effet au prochain shell, ou tout de suite : . $PROFILE"
elif path_already_set "$BIN_DIR"; then
  say "  PATH         : $BIN_DIR est deja joignable"
else
  say "  PATH         : inchange, lancez par $LAUNCHER"
fi

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

# UN BUREAU DEJA OUVERT. Installer, c'est repartir a neuf : un demon en cours
# tourne sur l'ANCIEN binaire et continuerait de le faire apres la pose, sans
# que rien ne le signale. On demande, et on l'arrete.
#
# ICI, et pas plus haut : a ce point la suite est verte et le binaire est bon.
# Demander avant la compilation ferait perdre un bureau pour une compilation
# qui echoue ensuite.
if [ -x "$EXE" ] && SSHOS_BOOT_ID="$INSTANCE" "$EXE" --status 2>/dev/null \
     | grep -q 'demon actif'; then
  say ""
  say "  Un bureau de l'instance $INSTANCE tourne : il sera ARRETE."
  say "  Ses fenetres et les programmes qui y tournent seront perdus."
  _d=non
  [ "$ASSUME_YES" = yes ] && _d=oui
  _r=$(ask "  Continuer ?" "$_d")
  case "$_r" in
    oui|o|yes|y) ;;
    *) die "installation abandonnee, le bureau est intact" ;;
  esac

  say "  arret du bureau..."
  SSHOS_BOOT_ID="$INSTANCE" "$EXE" --kill >/dev/null 2>&1 || true
  _n=50
  while [ "$_n" -gt 0 ]; do
    SSHOS_BOOT_ID="$INSTANCE" "$EXE" --status 2>/dev/null \
      | grep -q 'demon actif' || break
    sleep 0.1 2>/dev/null || sleep 1
    _n=$((_n - 1))
  done
  [ "$_n" -eq 0 ] && die "le bureau refuse de s'arreter, rien n'a ete installe"
  say "  bureau arrete"
fi

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
