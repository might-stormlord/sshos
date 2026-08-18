# Interface en mode texte pour tools/install.sh. A sourcer, pas a lancer.
#
# POURQUOI ELLE EST ECRITE A LA MAIN. L'installeur tourne AVANT que quoi que
# ce soit soit compile : il ne peut pas emprunter le moteur de rendu du
# projet. C'est donc du sh POSIX et des sequences ANSI.
#
# TROIS CONTRAINTES DE TERRAIN :
#   - /bin/sh est dash : pas de « read -n1 », pas de tableaux. Les octets se
#     lisent par dd et se decodent par od.
#   - ${#s} compte des OCTETS, pas des cellules. Tout le texte affiche est
#     donc en ASCII -- ce que la convention du projet impose deja aux
#     libelles -- et les seuls glyphes multi-octets (cadre, marqueur) sont
#     poses a des positions connues, hors du calcul de remplissage.
#   - \033, jamais \e : « \e » n'est pas POSIX pour printf.
#
# LA SOURIS D'ABORD. C'est la regle du projet, et elle a deja du etre redite
# deux fois : un assistant qui n'obeirait qu'aux fleches en serait le
# contre-exemple. Cliquer un choix le selectionne ET le valide.

ESC=$(printf '\033')

UI_ON=no
UI_STTY=""
UI_COLS=52

KEY=""
KEY_CHAR=""
MOUSE_X=0
MOUSE_Y=0

# --- disponibilite ---------------------------------------------------------
# Sans terminal, sans stty, ou avec --yes, on retombe sur les questions
# simples : c'est ce que la CI et la sonde utilisent, et elles ne doivent
# jamais se retrouver bloquees devant un assistant.
ui_possible() {
  [ "${ASSUME_YES:-no}" = yes ] && return 1
  [ -t 0 ] || return 1
  [ -t 1 ] || return 1
  command -v stty >/dev/null 2>&1 || return 1
  command -v dd >/dev/null 2>&1 || return 1
  command -v od >/dev/null 2>&1 || return 1
  return 0
}

ui_utf8() {
  case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8*|*utf-8*|*UTF8*|*utf8*) return 0 ;;
  esac
  return 1
}

ui_glyphs() {
  if ui_utf8; then
    G_TL='╭'; G_TR='╮'; G_BL='╰'; G_BR='╯'; G_H='─'; G_V='│'
    G_SEL='▸'; G_OK='✓'; G_FULL='▓'; G_EMPTY='░'
  else
    G_TL='+'; G_TR='+'; G_BL='+'; G_BR='+'; G_H='-'; G_V='|'
    G_SEL='>'; G_OK='x'; G_FULL='#'; G_EMPTY='.'
  fi
}

ui_measure() {
  _s=$(stty size 2>/dev/null) || _s=""
  _c=${_s#* }
  case "$_c" in ''|*[!0-9]*) _c=80 ;; esac
  # Assez large pour la ligne d'aide, assez etroit pour rester lisible.
  [ "$_c" -lt 54 ] && _c=54
  [ "$_c" -gt 74 ] && _c=74
  UI_COLS=$_c
}

# --- entree et sortie brutes ----------------------------------------------
ui_begin() {
  ui_glyphs
  ui_measure
  UI_STTY=$(stty -g 2>/dev/null || true)
  stty -echo -icanon min 1 time 0 2>/dev/null || true
  # Ecran alterne comme le bureau : l'historique du terminal n'est pas
  # saccage. Curseur cache, souris en SGR (1006) pour ne pas etre borne a
  # 223 colonnes.
  printf '\033[?1049h\033[?25l\033[?1000h\033[?1006h'
  UI_ON=yes
}

ui_end() {
  [ "$UI_ON" = yes ] || return 0
  printf '\033[?1006l\033[?1000l\033[?25h\033[?1049l'
  [ -n "$UI_STTY" ] && stty "$UI_STTY" 2>/dev/null
  UI_ON=no
}

# Rend le code decimal de l'octet lu, ou vide si rien n'est venu.
ui_byte() {
  dd bs=1 count=1 2>/dev/null | od -An -tu1 | tr -dc '0-9'
}

ui_chr() { printf "\\$(printf '%03o' "$1")"; }

# Une position de souris SGR : \033[<bouton;colonne;ligneM (appui) ou m
# (relachement). On ne retient que l'appui du bouton gauche.
ui_mouse() {
  _acc=""
  _final=""
  while :; do
    _b=$(ui_byte)
    [ -z "$_b" ] && break
    if [ "$_b" = 77 ] || [ "$_b" = 109 ]; then _final=$_b; break; fi
    _acc="$_acc$(ui_chr "$_b")"
  done
  _btn=${_acc%%;*}
  _rest=${_acc#*;}
  MOUSE_X=${_rest%%;*}
  MOUSE_Y=${_rest#*;}
  case "$MOUSE_X" in ''|*[!0-9]*) MOUSE_X=0 ;; esac
  case "$MOUSE_Y" in ''|*[!0-9]*) MOUSE_Y=0 ;; esac
  if [ "$_final" = 77 ] && [ "$_btn" = 0 ]; then KEY=click; else KEY=other; fi
}

ui_escape_seq() {
  # Un ESC seul ne se distingue d'une sequence que par le temps : on attend
  # un dixieme de seconde, pas plus.
  stty min 0 time 1 2>/dev/null
  _b2=$(ui_byte)
  if [ "$_b2" != 91 ]; then
    stty min 1 time 0 2>/dev/null
    KEY=escape
    return
  fi
  _b3=$(ui_byte)
  case "$_b3" in
    65) KEY=up ;;
    66) KEY=down ;;
    67) KEY=right ;;
    68) KEY=left ;;
    60) ui_mouse ;;
    *)  KEY=other ;;
  esac
  stty min 1 time 0 2>/dev/null
}

ui_key() {
  KEY=""
  KEY_CHAR=""
  _b=$(ui_byte)
  case "$_b" in
    "")    KEY=none ;;
    3)     KEY=ctrlc ;;
    10|13) KEY=enter ;;
    8|127) KEY=backspace ;;
    27)    ui_escape_seq ;;
    *)
      if [ "$_b" -ge 32 ] 2>/dev/null && [ "$_b" -le 126 ]; then
        KEY=char
        KEY_CHAR=$(ui_chr "$_b")
      else
        KEY=other
      fi
      ;;
  esac
}

# --- dessin ----------------------------------------------------------------
# Le contenu est ASCII, le cadre ne l'est pas : le remplissage se calcule sur
# le texte seul, et les glyphes sont poses autour, a des positions connues.
#
# ATTENTION : ${#s} de dash compte des OCTETS. Tout texte passe ici doit donc
# etre en ASCII PUR -- pas meme des guillemets francais, qui valent une
# cellule pour deux octets et decalent le cadre d'autant. La regle a ete
# enfreinte des le premier essai, sur un « sshos » entre guillemets ;
# tools/verif_installeur.py mesure desormais la largeur de chaque ligne du
# cadre et mord si elle revient.
ui_pad() { # ui_pad <texte ASCII PUR> <largeur>
  _t="$1"; _w="$2"
  _n=$((_w - ${#_t}))
  [ "$_n" -lt 0 ] && _n=0
  printf '%s' "$_t"
  while [ "$_n" -gt 0 ]; do printf ' '; _n=$((_n - 1)); done
}

ui_rule() { # une ligne de cadre pleine
  _n=$((UI_COLS - 2))
  printf '%s' "$1"
  while [ "$_n" -gt 0 ]; do printf '%s' "$G_H"; _n=$((_n - 1)); done
  printf '%s\n' "$2"
}

ui_top()    { ui_rule "$G_TL" "$G_TR"; }
ui_bottom() { ui_rule "$G_BL" "$G_BR"; }

# Une ligne de contenu : bordure, marqueur d'UNE cellule, texte, bordure.
ui_row() { # ui_row <marqueur> <texte ascii>
  printf '%s %s ' "$G_V" "$1"
  ui_pad "$2" $((UI_COLS - 6))
  printf ' %s\n' "$G_V"
}

ui_blank() { ui_row ' ' ''; }

ui_title() { # une barre de titre, en gras
  printf '%s ' "$G_V"
  printf '\033[1m'
  ui_pad "$1" $((UI_COLS - 4))
  printf '\033[0m'
  printf ' %s\n' "$G_V"
}

ui_bar() { # ui_bar <fait> <total>
  _done="$1"; _tot="$2"
  _w=$((UI_COLS - 22))
  [ "$_w" -lt 8 ] && _w=8
  _f=$((_done * _w / _tot))
  printf '%s   ' "$G_V"
  _i=0
  while [ "$_i" -lt "$_w" ]; do
    if [ "$_i" -lt "$_f" ]; then printf '\033[36m%s\033[0m' "$G_FULL"
    else printf '\033[2m%s\033[0m' "$G_EMPTY"; fi
    _i=$((_i + 1))
  done
  _label="  $_done/$_tot"
  ui_pad "$_label" $((UI_COLS - 5 - _w))
  printf '%s\n' "$G_V"
}

ui_home() { printf '\033[H\033[2J'; }
